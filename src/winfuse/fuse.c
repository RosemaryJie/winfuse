/**
 * @file winfuse/fuse.c
 *
 * @copyright 2019-2020 Bill Zissimopoulos
 */
/*
 * This file is part of WinFuse.
 *
 * You can redistribute it and/or modify it under the terms of the GNU
 * Affero General Public License version 3 as published by the Free
 * Software Foundation.
 *
 * Licensees holding a valid commercial license may use this software
 * in accordance with the commercial license agreement provided in
 * conjunction with the software.  The terms and conditions of any such
 * commercial license agreement shall govern, supersede, and render
 * ineffective any application of the AGPLv3 license to this software,
 * notwithstanding of any reference thereto in the software or
 * associated repository.
 */

#include <winfuse/driver.h>

static NTSTATUS FuseDeviceInit(PDEVICE_OBJECT DeviceObject, FSP_FSCTL_VOLUME_PARAMS *VolumeParams);
static VOID FuseDeviceFini(PDEVICE_OBJECT DeviceObject);
static VOID FuseDeviceExpirationRoutine(PDEVICE_OBJECT DeviceObject, UINT64 ExpirationTime);
static NTSTATUS FuseDeviceTransact(PDEVICE_OBJECT DeviceObject, PIRP Irp);

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, FuseDeviceInit)
#pragma alloc_text(PAGE, FuseDeviceFini)
#pragma alloc_text(PAGE, FuseDeviceExpirationRoutine)
#pragma alloc_text(PAGE, FuseDeviceTransact)
#endif

static NTSTATUS FuseDeviceInit(PDEVICE_OBJECT DeviceObject, FSP_FSCTL_VOLUME_PARAMS *VolumeParams)
{
    PAGED_CODE();

    KeEnterCriticalRegion();

    FUSE_INSTANCE *Instance = FuseInstanceFromDeviceObject(DeviceObject);
    FUSE_IOQ *Ioq = 0;
    FUSE_CACHE *Cache = 0;
    NTSTATUS Result;

    /* ensure that VolumeParams can be used for FUSE operations */
    VolumeParams->CaseSensitiveSearch = 1;
    VolumeParams->CasePreservedNames = 1;
    VolumeParams->PersistentAcls = 1;
    VolumeParams->ReparsePoints = 1;
    VolumeParams->ReparsePointsAccessCheck = 0;
    VolumeParams->NamedStreams = 0;
    VolumeParams->ReadOnlyVolume = 0;
    VolumeParams->PostCleanupWhenModifiedOnly = 1;
    VolumeParams->PassQueryDirectoryFileName = 1;
    VolumeParams->DeviceControl = 1;
    VolumeParams->DirectoryMarkerAsNextOffset = 1;

    Result = FuseIoqCreate(&Ioq);
    if (!NT_SUCCESS(Result))
        goto fail;

    Result = FuseCacheCreate(0, !VolumeParams->CaseSensitiveSearch, &Cache);
    if (!NT_SUCCESS(Result))
        goto fail;

    Instance->VolumeParams = VolumeParams;
    FuseRwlockInitialize(&Instance->OpGuardLock);
    Instance->Ioq = Ioq;
    Instance->Cache = Cache;
    KeInitializeEvent(&Instance->InitEvent, NotificationEvent, FALSE);

    FuseFileInstanceInit(Instance);

    Result = FuseProtoPostInit(Instance);
    if (!NT_SUCCESS(Result))
        goto fail;

    KeLeaveCriticalRegion();

    return STATUS_SUCCESS;

fail:
    if (0 != Cache)
        FuseCacheDelete(Cache);

    if (0 != Ioq)
        FuseIoqDelete(Ioq);

    KeLeaveCriticalRegion();

    return Result;
}

static VOID FuseDeviceFini(PDEVICE_OBJECT DeviceObject)
{
    PAGED_CODE();

    KeEnterCriticalRegion();

    FUSE_INSTANCE *Instance = FuseInstanceFromDeviceObject(DeviceObject);

    /*
     * The order of finalization is IMPORTANT:
     *
     * FuseIoqDelete must precede FuseFileDeviceFini, because the Ioq may contain Contexts
     * that hold File's.
     *
     * FuseIoqDelete must precede FuseCacheDelete, because the Ioq may contain Contexts
     * that hold CacheGen references.
     *
     * FuseFileDeviceFini must precede FuseCacheDelete, because some Files may hold
     * CacheItem references.
     */

    FuseIoqDelete(Instance->Ioq);

    FuseFileInstanceFini(Instance);

    FuseCacheDelete(Instance->Cache);

    FuseRwlockFinalize(&Instance->OpGuardLock);

    KeLeaveCriticalRegion();
}

static VOID FuseDeviceExpirationRoutine(PDEVICE_OBJECT DeviceObject, UINT64 ExpirationTime)
{
    PAGED_CODE();

    KeEnterCriticalRegion();

    FUSE_INSTANCE *Instance = FuseInstanceFromDeviceObject(DeviceObject);

    FuseCacheExpirationRoutine(Instance->Cache, Instance, ExpirationTime);

    KeLeaveCriticalRegion();
}

static NTSTATUS FuseDeviceTransact(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PAGED_CODE();

    ASSERT(KeAreApcsDisabled());

    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    ASSERT(IRP_MJ_FILE_SYSTEM_CONTROL == IrpSp->MajorFunction);
    ASSERT(IRP_MN_USER_FS_REQUEST == IrpSp->MinorFunction);
    ASSERT(FUSE_FSCTL_TRANSACT == IrpSp->Parameters.FileSystemControl.FsControlCode);
    ASSERT(METHOD_BUFFERED == (IrpSp->Parameters.FileSystemControl.FsControlCode & 3));
    ASSERT(IrpSp->FileObject->FsContext2 == DeviceObject);

    /* check parameters */
    ULONG InputBufferLength = IrpSp->Parameters.FileSystemControl.InputBufferLength;
    ULONG OutputBufferLength = IrpSp->Parameters.FileSystemControl.OutputBufferLength;
    FUSE_PROTO_RSP *FuseResponse = 0 != InputBufferLength ? Irp->AssociatedIrp.SystemBuffer : 0;
    FUSE_PROTO_REQ *FuseRequest = 0 != OutputBufferLength ? Irp->AssociatedIrp.SystemBuffer : 0;
    if (0 != FuseResponse)
    {
        if (FUSE_PROTO_RSP_HEADER_SIZE > InputBufferLength ||
            FUSE_PROTO_RSP_HEADER_SIZE > FuseResponse->len ||
            FuseResponse->len > InputBufferLength)
            return STATUS_INVALID_PARAMETER;
    }
    if (0 != FuseRequest)
    {
        if (FUSE_PROTO_REQ_SIZEMIN > OutputBufferLength)
            return STATUS_BUFFER_TOO_SMALL;
    }

    FUSE_INSTANCE *Instance = FuseInstanceFromDeviceObject(DeviceObject);
    FSP_FSCTL_TRANSACT_REQ *InternalRequest = 0;
    FSP_FSCTL_TRANSACT_RSP InternalResponse;
    FUSE_CONTEXT *Context;
    BOOLEAN Continue;
    NTSTATUS Result;

    if (0 != FuseResponse)
    {
        Context = FuseIoqEndProcessing(Instance->Ioq, FuseResponse->unique);
        if (0 == Context)
            goto request;

        Continue = FuseContextProcess(Context, FuseResponse, 0, 0);

        if (Continue)
            FuseIoqPostPending(Instance->Ioq, Context);
        else if (0 == Context->InternalRequest)
            FuseContextDelete(Context);
        else
        {
            ASSERT(FspFsctlTransactReservedKind != Context->InternalResponse->Kind);

            Result = FspFsextProviderTransact(
                IrpSp->DeviceObject, IrpSp->FileObject, Context->InternalResponse, 0);
            FuseContextDelete(Context);
            if (!NT_SUCCESS(Result))
                goto exit;
        }
    }

request:
    Irp->IoStatus.Information = 0;
    if (0 != FuseRequest)
    {
        RtlZeroMemory(FuseRequest, FUSE_PROTO_REQ_HEADER_SIZE);

        Context = FuseIoqNextPending(Instance->Ioq);
        if (0 == Context)
        {
            UINT32 VersionMajor = Instance->VersionMajor;
            MemoryBarrier();
            if (0 == VersionMajor)
            {
                Result = FsRtlCancellableWaitForSingleObject(&Instance->InitEvent,
                    0, Irp);
                if (STATUS_TIMEOUT == Result || STATUS_THREAD_IS_TERMINATING == Result)
                    Result = STATUS_CANCELLED;
                if (!NT_SUCCESS(Result))
                    goto exit;
                ASSERT(STATUS_SUCCESS == Result);

                VersionMajor = Instance->VersionMajor;
            }
            if ((UINT32)-1 == VersionMajor)
            {
                Result = STATUS_ACCESS_DENIED;
                goto exit;
            }

            Result = FspFsextProviderTransact(
                IrpSp->DeviceObject, IrpSp->FileObject, 0, &InternalRequest);
            if (!NT_SUCCESS(Result))
                goto exit;
            if (0 == InternalRequest)
            {
                Irp->IoStatus.Information = 0;
                Result = STATUS_SUCCESS;
                goto exit;
            }

            ASSERT(FspFsctlTransactReservedKind != InternalRequest->Kind);

            FuseContextCreate(&Context, Instance, InternalRequest);
            ASSERT(0 != Context);

            Continue = FALSE;
            if (!FuseContextIsStatus(Context))
            {
                InternalRequest = 0;
                Continue = FuseContextProcess(Context, 0, FuseRequest, OutputBufferLength);
            }
        }
        else
        {
            ASSERT(!FuseContextIsStatus(Context));
            Continue = FuseContextProcess(Context, 0, FuseRequest, OutputBufferLength);
        }

        if (Continue)
        {
            ASSERT(!FuseContextIsStatus(Context));
            FuseIoqStartProcessing(Instance->Ioq, Context);
        }
        else if (FuseContextIsStatus(Context))
        {
            ASSERT(0 != InternalRequest);
            RtlZeroMemory(&InternalResponse, sizeof InternalResponse);
            InternalResponse.Size = sizeof InternalResponse;
            InternalResponse.Kind = InternalRequest->Kind;
            InternalResponse.Hint = InternalRequest->Hint;
            InternalResponse.IoStatus.Status = FuseContextToStatus(Context);
            Result = FspFsextProviderTransact(
                IrpSp->DeviceObject, IrpSp->FileObject, &InternalResponse, 0);
            if (!NT_SUCCESS(Result))
                goto exit;
        }
        else if (0 == Context->InternalRequest)
        {
            switch (Context->InternalResponse->Hint)
            {
            case FUSE_PROTO_OPCODE_FORGET:
            case FUSE_PROTO_OPCODE_BATCH_FORGET:
                if (!IsListEmpty(&Context->Forget.ForgetList))
                    FuseIoqPostPending(Instance->Ioq, Context);
                else
                    FuseContextDelete(Context);
                break;
            }
        }
        else
        {
            Result = FspFsextProviderTransact(
                IrpSp->DeviceObject, IrpSp->FileObject, Context->InternalResponse, 0);
            FuseContextDelete(Context);
            if (!NT_SUCCESS(Result))
                goto exit;
        }

        Irp->IoStatus.Information = FuseRequest->len;
    }

    Result = STATUS_SUCCESS;

exit:
    if (0 != InternalRequest)
        FuseFreeExternal(InternalRequest);

    return Result;
}

FSP_FSEXT_PROVIDER FuseProvider =
{
    /* Version */
    sizeof FuseProvider,

    /* DeviceTransactCode */
    FUSE_FSCTL_TRANSACT,

    /* DeviceExtensionSize */
    sizeof(FUSE_INSTANCE),

    /* DeviceInit */
    FuseDeviceInit,

    /* DeviceFini */
    FuseDeviceFini,

    /* DeviceExpirationRoutine */
    FuseDeviceExpirationRoutine,

    /* DeviceTransact */
    FuseDeviceTransact,
};
