/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS Kernel Streaming
 * FILE:            drivers/wdm/audio/sysaudio/control.c
 * PURPOSE:         System Audio graph builder
 * PROGRAMMER:      Johannes Anderwald
 */

#include "sysaudio.h"

const GUID KSPROPSETID_Sysaudio                 = {0xCBE3FAA0L, 0xCC75, 0x11D0, {0xB4, 0x65, 0x00, 0x00, 0x1A, 0x18, 0x18, 0xE6}};
const GUID KSPROPSETID_Sysaudio_Pin             = {0xA3A53220L, 0xC6E4, 0x11D0, {0xB4, 0x65, 0x00, 0x00, 0x1A, 0x18, 0x18, 0xE6}};
const GUID KSPROPSETID_General                  = {0x1464EDA5L, 0x6A8F, 0x11D1, {0x9A, 0xA7, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96}};
const GUID KSPROPSETID_Pin                     = {0x8C134960L, 0x51AD, 0x11CF, {0x87, 0x8A, 0x94, 0xF8, 0x01, 0xC1, 0x00, 0x00}};
const GUID KSPROPSETID_Connection              = {0x1D58C920L, 0xAC9B, 0x11CF, {0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00}};
const GUID KSPROPSETID_Topology                 = {0x720D4AC0L, 0x7533, 0x11D0, {0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00}};
const GUID KSDATAFORMAT_TYPE_AUDIO              = {0x73647561L, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID KSDATAFORMAT_SUBTYPE_PCM             = {0x00000001L, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID KSDATAFORMAT_SPECIFIER_WAVEFORMATEX  = {0x05589f81L, 0xc356, 0x11ce, {0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a}};

NTSTATUS
ComputeCompatibleFormat(
    IN PKSAUDIO_SUBDEVICE_ENTRY Entry,
    IN ULONG PinId,
    IN PSYSAUDIODEVEXT DeviceExtension,
    IN PKSDATAFORMAT_WAVEFORMATEX ClientFormat,
    OUT PKSDATAFORMAT_WAVEFORMATEX MixerFormat);


NTSTATUS
NTAPI
KspCreateObjectType(
    IN HANDLE ParentHandle,
    IN LPWSTR ObjectType,
    PVOID CreateParameters,
    UINT CreateParametersSize,
    IN  ACCESS_MASK DesiredAccess,
    OUT PHANDLE NodeHandle)
{
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatusBlock;
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING Name;

    Name.Length = Name.MaximumLength = (wcslen(ObjectType) + 1) * sizeof(WCHAR) + CreateParametersSize;
    Name.MaximumLength += sizeof(WCHAR);
    Name.Buffer = ExAllocatePool(NonPagedPool, Name.MaximumLength);

    if (!Name.Buffer)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    wcscpy(Name.Buffer, ObjectType);
    Name.Buffer[wcslen(ObjectType)] = '\\';

    RtlMoveMemory(Name.Buffer + wcslen(ObjectType) +1, CreateParameters, CreateParametersSize);

    Name.Buffer[Name.Length / 2] = L'\0';
    InitializeObjectAttributes(&ObjectAttributes, &Name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE | OBJ_OPENIF, ParentHandle, NULL);


    Status = IoCreateFile(NodeHandle,
                          DesiredAccess,
                          &ObjectAttributes,
                          &IoStatusBlock,
                          NULL,
                          0,
                          0,
                          FILE_OPEN,
                          FILE_SYNCHRONOUS_IO_NONALERT,
                          NULL,
                          0,
                          CreateFileTypeNone,
                          NULL,
                          IO_NO_PARAMETER_CHECKING | IO_FORCE_ACCESS_CHECK);

    ExFreePool(Name.Buffer);

    return Status;
}

KSDDKAPI
NTSTATUS
NTAPI
KsoCreatePin(
    IN  HANDLE FilterHandle,
    IN  PKSPIN_CONNECT Connect,
    IN  ACCESS_MASK DesiredAccess,
    OUT PHANDLE ConnectionHandle,
    IN LPWSTR ObjectClass)
{
    WCHAR szBuffer[100];
    UINT ConnectSize = sizeof(KSPIN_CONNECT);

    PKSDATAFORMAT_WAVEFORMATEX Format = (PKSDATAFORMAT_WAVEFORMATEX)(Connect + 1);
    if (Format->DataFormat.FormatSize == sizeof(KSDATAFORMAT) ||
        Format->DataFormat.FormatSize == sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEX))
    {
        ConnectSize += Format->DataFormat.FormatSize;
    }

    swprintf(szBuffer, L"%s\\{146F1A80-4791-11D0-A5D6-28DB04C10000}", ObjectClass);

    return KspCreateObjectType(FilterHandle,
                               szBuffer,
                               (PVOID)Connect,
                               ConnectSize,
                               DesiredAccess,
                               ConnectionHandle);
}


NTSTATUS
SetIrpIoStatus(
    IN PIRP Irp,
    IN NTSTATUS Status,
    IN ULONG Length)
{
    Irp->IoStatus.Information = Length;
    Irp->IoStatus.Status = Status;
    if (Status != STATUS_PENDING)
    {
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }
    else
    {
        IoMarkIrpPending(Irp);
    }
    return Status;

}

PKSAUDIO_SUBDEVICE_ENTRY
GetListEntry(
    IN PLIST_ENTRY Head,
    IN ULONG Index)
{
    PKSAUDIO_DEVICE_ENTRY DeviceEntry;
    PKSAUDIO_SUBDEVICE_ENTRY SubDeviceEntry;
    PLIST_ENTRY SubEntry, Entry = Head->Flink;

    while(Entry != Head)
    {
        DeviceEntry = (PKSAUDIO_DEVICE_ENTRY)CONTAINING_RECORD(Entry, KSAUDIO_DEVICE_ENTRY, Entry);
        if (Index < DeviceEntry->NumSubDevices)
        {
            SubEntry = DeviceEntry->SubDeviceList.Flink;
            while(SubEntry != &DeviceEntry->SubDeviceList && Index--)
                SubEntry = SubEntry->Flink;

            SubDeviceEntry = (PKSAUDIO_SUBDEVICE_ENTRY)CONTAINING_RECORD(SubEntry, KSAUDIO_SUBDEVICE_ENTRY, Entry);
            return SubDeviceEntry;
        }
        else
        {
            Index -= DeviceEntry->NumSubDevices;
        }

        Entry = Entry->Flink;

    }
    DPRINT1("Not Found index %u\n", Index);

    return NULL;
}

NTSTATUS
SysAudioOpenVirtualDevice(
    IN PIRP Irp,
    IN ULONG DeviceNumber,
    PSYSAUDIODEVEXT DeviceExtension)
{
    PKSAUDIO_SUBDEVICE_ENTRY Entry;
    PKSOBJECT_CREATE_ITEM CreateItem;

    /* access the create item */
    CreateItem = KSCREATE_ITEM_IRP_STORAGE(Irp);
    ASSERT(CreateItem);

    if (DeviceNumber >= DeviceExtension->NumberOfKsAudioDevices)
    {
        /* invalid device index */
        return SetIrpIoStatus(Irp, STATUS_INVALID_PARAMETER, 0);
    }

    /* get device context */
    Entry = GetListEntry(&DeviceExtension->KsAudioDeviceList, DeviceNumber);
    ASSERT(Entry != NULL);

    return SetIrpIoStatus(Irp, STATUS_SUCCESS, 0);
}

NTSTATUS
SetMixerInputOutputFormat(
    IN PFILE_OBJECT FileObject,
    IN PKSDATAFORMAT InputFormat,
    IN PKSDATAFORMAT OutputFormat)
{
    KSP_PIN PinRequest;
    ULONG BytesReturned;
    NTSTATUS Status;

    /* re-using pin */
    PinRequest.Property.Set = KSPROPSETID_Connection;
    PinRequest.Property.Flags = KSPROPERTY_TYPE_SET;
    PinRequest.Property.Id = KSPROPERTY_CONNECTION_DATAFORMAT;

    /* set the input format */
    PinRequest.PinId = 0;
    DPRINT("InputFormat %p Size %u WaveFormatSize %u DataFormat %u WaveEx %u\n", InputFormat, InputFormat->FormatSize, sizeof(KSDATAFORMAT_WAVEFORMATEX), sizeof(KSDATAFORMAT), sizeof(WAVEFORMATEX));
    Status = KsSynchronousIoControlDevice(FileObject, KernelMode, IOCTL_KS_PROPERTY,
                                          (PVOID)&PinRequest,
                                           sizeof(KSP_PIN),
                                          (PVOID)InputFormat,
                                           InputFormat->FormatSize,
                                          &BytesReturned);
    if (!NT_SUCCESS(Status))
        return Status;

    /* set the the output format */
    PinRequest.PinId = 1;
    DPRINT("OutputFormat %p Size %u WaveFormatSize %u DataFormat %u WaveEx %u\n", OutputFormat, OutputFormat->FormatSize, sizeof(KSDATAFORMAT_WAVEFORMATEX), sizeof(KSDATAFORMAT), sizeof(WAVEFORMATEX));
    Status = KsSynchronousIoControlDevice(FileObject, KernelMode, IOCTL_KS_PROPERTY,
                                          (PVOID)&PinRequest,
                                           sizeof(KSP_PIN),
                                          (PVOID)OutputFormat,
                                           OutputFormat->FormatSize,
                                          &BytesReturned);
    return Status;
}


NTSTATUS
CreateMixerPinAndSetFormat(
    IN HANDLE KMixerHandle,
    IN KSPIN_CONNECT *PinConnect,
    IN PKSDATAFORMAT InputFormat,
    IN PKSDATAFORMAT OutputFormat,
    OUT PHANDLE MixerPinHandle)
{
    NTSTATUS Status;
    HANDLE PinHandle;
    PFILE_OBJECT FileObject;

    Status = KsoCreatePin(KMixerHandle, PinConnect, GENERIC_READ | GENERIC_WRITE, &PinHandle, L"KMixer");

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to create Mixer Pin with %x\n", Status);
        return STATUS_UNSUCCESSFUL;
    }

    Status = ObReferenceObjectByHandle(PinHandle,
                                       GENERIC_READ | GENERIC_WRITE, 
                                       IoFileObjectType, KernelMode, (PVOID*)&FileObject, NULL);

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to get file object with %x\n", Status);
        return STATUS_UNSUCCESSFUL;
    }

    Status = SetMixerInputOutputFormat(FileObject, InputFormat, OutputFormat);
    if (!NT_SUCCESS(Status))
    {
        ObDereferenceObject(FileObject);
        ZwClose(PinHandle);
    }

    ObDereferenceObject(FileObject);

    *MixerPinHandle = PinHandle;
     return Status;
}


VOID
NTAPI
CreatePinWorkerRoutine(
    IN PDEVICE_OBJECT DeviceObject,
    IN PVOID  Context)
{
    NTSTATUS Status;
    HANDLE RealPinHandle = NULL, VirtualPinHandle = NULL, Filter;
    PFILE_OBJECT VirtualFileObject = NULL;
    PKSDATAFORMAT_WAVEFORMATEX InputFormat;
    PKSDATAFORMAT_WAVEFORMATEX OutputFormat = NULL;
    PKSPIN_CONNECT MixerPinConnect = NULL;
    PPIN_WORKER_CONTEXT WorkerContext = (PPIN_WORKER_CONTEXT)Context;

    Filter = WorkerContext->PinConnect->PinToHandle;

    WorkerContext->PinConnect->PinToHandle = NULL;

    DPRINT("CreatePinWorkerRoutine entered\n");

    ASSERT(WorkerContext->Entry);
    ASSERT(WorkerContext->PinConnect);
    ASSERT(WorkerContext->Entry->Pins);
    ASSERT(WorkerContext->Entry->NumberOfPins > WorkerContext->PinConnect->PinId);

    /* Fetch input format */
    InputFormat = (PKSDATAFORMAT_WAVEFORMATEX)(WorkerContext->PinConnect + 1);


    /* Let's try to create the audio irp pin */
    Status = KsoCreatePin(WorkerContext->Entry->Handle, WorkerContext->PinConnect, GENERIC_READ | GENERIC_WRITE, &RealPinHandle, WorkerContext->Entry->ObjectClass);

    if (!NT_SUCCESS(Status))
    {
        /* the audio irp pin didnt accept the input format
         * let's compute a compatible format
         */

        MixerPinConnect = ExAllocatePool(NonPagedPool, sizeof(KSPIN_CONNECT) + sizeof(KSDATAFORMAT_WAVEFORMATEX));
        if (!MixerPinConnect)
        {
            SetIrpIoStatus(WorkerContext->Irp, STATUS_UNSUCCESSFUL, 0);
            ExFreePool(WorkerContext->DispatchContext);
            IoFreeWorkItem(WorkerContext->WorkItem);
            ExFreePool(WorkerContext);
            return;
        }

        /* Zero pin connect */
        RtlZeroMemory(MixerPinConnect, sizeof(KSPIN_CONNECT) + sizeof(KSDATAFORMAT_WAVEFORMATEX));

        /* Copy initial connect details */
        RtlMoveMemory(MixerPinConnect, WorkerContext->PinConnect, sizeof(KSPIN_CONNECT));


        OutputFormat = (PKSDATAFORMAT_WAVEFORMATEX)(MixerPinConnect + 1);

        Status = ComputeCompatibleFormat(WorkerContext->Entry, WorkerContext->PinConnect->PinId, WorkerContext->DeviceExtension, InputFormat, OutputFormat);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("ComputeCompatibleFormat failed with %x\n", Status);
            SetIrpIoStatus(WorkerContext->Irp, STATUS_UNSUCCESSFUL, 0);
            ExFreePool(WorkerContext->DispatchContext);
            ExFreePool(MixerPinConnect);
            IoFreeWorkItem(WorkerContext->WorkItem);
            ExFreePool(WorkerContext);
            return;
        }

        /* Retry with Mixer format */
        Status = KsoCreatePin(WorkerContext->Entry->Handle, MixerPinConnect, GENERIC_READ | GENERIC_WRITE, &RealPinHandle, WorkerContext->Entry->ObjectClass);
        if (!NT_SUCCESS(Status))
        {
           /* This should not fail */
            DPRINT1("KsCreatePin failed with %x\n", Status);
            DPRINT1(" InputFormat: SampleRate %u Bits %u Channels %u\n", InputFormat->WaveFormatEx.nSamplesPerSec, InputFormat->WaveFormatEx.wBitsPerSample, InputFormat->WaveFormatEx.nChannels);
            DPRINT1("OutputFormat: SampleRate %u Bits %u Channels %u\n", OutputFormat->WaveFormatEx.nSamplesPerSec, OutputFormat->WaveFormatEx.wBitsPerSample, OutputFormat->WaveFormatEx.nChannels);
 
            SetIrpIoStatus(WorkerContext->Irp, STATUS_UNSUCCESSFUL, 0);
            ExFreePool(WorkerContext->DispatchContext);
            ExFreePool(MixerPinConnect);
            IoFreeWorkItem(WorkerContext->WorkItem);
            ExFreePool(WorkerContext);
            return;
        }
    }

    ASSERT(WorkerContext->Entry->Pins[WorkerContext->PinConnect->PinId].MaxPinInstanceCount);

    WorkerContext->Entry->Pins[WorkerContext->PinConnect->PinId].References = 0;
    WorkerContext->DispatchContext->Handle = RealPinHandle;
    WorkerContext->DispatchContext->PinId = WorkerContext->PinConnect->PinId;
    WorkerContext->DispatchContext->AudioEntry = WorkerContext->Entry;

    /* Do we need to transform the audio stream */
    if (OutputFormat != NULL)
    {
        /* Now create the mixer pin */
        Status = CreateMixerPinAndSetFormat(WorkerContext->DeviceExtension->KMixerHandle,
                                            MixerPinConnect,
                                            (PKSDATAFORMAT)InputFormat,
                                            (PKSDATAFORMAT)OutputFormat,
                                            &WorkerContext->DispatchContext->hMixerPin);


        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Failed to create Mixer Pin with %x\n", Status);
            goto cleanup;
        }

    }

    DPRINT1("creating virtual pin\n");
    /* now create the virtual audio pin which is exposed to wdmaud */
    Status = KsoCreatePin(Filter, WorkerContext->PinConnect, GENERIC_READ | GENERIC_WRITE, &VirtualPinHandle, L"SysAudio");

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to create virtual pin %x\n", Status);
        goto cleanup;
    }

   /* get pin file object */
    Status = ObReferenceObjectByHandle(VirtualPinHandle,
                                      GENERIC_READ | GENERIC_WRITE, 
                                      IoFileObjectType, KernelMode, (PVOID*)&VirtualFileObject, NULL);

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to get file object with %x\n", Status);
        goto cleanup;
    }

    ASSERT(WorkerContext->Entry->Pins != NULL);
    ASSERT(WorkerContext->Entry->NumberOfPins > WorkerContext->PinConnect->PinId);

    /* increment reference count */
    WorkerContext->Entry->Pins[WorkerContext->PinConnect->PinId].References++;

    /* store the pin handle there if the pin can only be instantiated once*/
    WorkerContext->Entry->Pins[WorkerContext->PinConnect->PinId].PinHandle = VirtualPinHandle;

    /* store pin context */
    VirtualFileObject->FsContext2 = (PVOID)WorkerContext->DispatchContext;

    /* release virtual file object */
    ObDereferenceObject(VirtualFileObject);

    DPRINT("Successfully created virtual pin %p\n", VirtualPinHandle);
    *((PHANDLE)WorkerContext->Irp->UserBuffer) = VirtualPinHandle;

    SetIrpIoStatus(WorkerContext->Irp, STATUS_SUCCESS, sizeof(HANDLE));
    IoFreeWorkItem(WorkerContext->WorkItem);
    ExFreePool(WorkerContext);
    return;

cleanup:
    if (RealPinHandle)
        ZwClose(RealPinHandle);

    if (WorkerContext->DispatchContext->hMixerPin)
        ZwClose(WorkerContext->DispatchContext->hMixerPin);


    ExFreePool(WorkerContext->DispatchContext);
    SetIrpIoStatus(WorkerContext->Irp, Status, 0);
    IoFreeWorkItem(WorkerContext->WorkItem);
    ExFreePool(WorkerContext);
}

NTSTATUS
HandleSysAudioFilterPinProperties(
    PIRP Irp,
    PKSPROPERTY Property,
    PSYSAUDIODEVEXT DeviceExtension)
{
    PIO_STACK_LOCATION IoStack;
    NTSTATUS Status;
    PKSAUDIO_SUBDEVICE_ENTRY Entry;
    ULONG BytesReturned;
    PKSP_PIN Pin;

    // in order to access pin properties of a sysaudio device
    // the caller must provide a KSP_PIN struct, where
    // Reserved member points to virtual device index

    IoStack = IoGetCurrentIrpStackLocation(Irp);
    if (IoStack->Parameters.DeviceIoControl.InputBufferLength < sizeof(KSP_PIN))
    {
        /* too small buffer */
        return SetIrpIoStatus(Irp, STATUS_BUFFER_TOO_SMALL, sizeof(KSPROPERTY) + sizeof(ULONG));
    }

    Pin = (PKSP_PIN)Property;

    Entry = GetListEntry(&DeviceExtension->KsAudioDeviceList, ((KSP_PIN*)Property)->Reserved);
    if (!Entry)
    {
        /* invalid device index */
        return SetIrpIoStatus(Irp, STATUS_INVALID_PARAMETER, 0);
    }

    if (!Entry->Pins)
    {
        /* expected pins */
        return SetIrpIoStatus(Irp, STATUS_UNSUCCESSFUL, 0);
    }

    if (Entry->NumberOfPins <= Pin->PinId)
    {
        /* invalid pin id */
        return SetIrpIoStatus(Irp, STATUS_INVALID_PARAMETER, 0);
    }

    if (Property->Id == KSPROPERTY_PIN_CTYPES)
    {
        if (IoStack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(ULONG))
        {
            /* too small buffer */
            return SetIrpIoStatus(Irp, STATUS_BUFFER_TOO_SMALL, sizeof(ULONG));
        }
        /* store result */
        *((PULONG)Irp->UserBuffer) = Entry->NumberOfPins;
        return SetIrpIoStatus(Irp, STATUS_SUCCESS, sizeof(ULONG));
    }
    else if (Property->Id == KSPROPERTY_PIN_COMMUNICATION)
    {
        if (IoStack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(KSPIN_COMMUNICATION))
        {
            /* too small buffer */
            return SetIrpIoStatus(Irp, STATUS_BUFFER_TOO_SMALL, sizeof(KSPIN_COMMUNICATION));
        }
        /* store result */
        *((KSPIN_COMMUNICATION*)Irp->UserBuffer) = Entry->Pins[Pin->PinId].Communication;
        return SetIrpIoStatus(Irp, STATUS_SUCCESS, sizeof(KSPIN_COMMUNICATION));

    }
    else if (Property->Id == KSPROPERTY_PIN_DATAFLOW)
    {
        if (IoStack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(KSPIN_DATAFLOW))
        {
            /* too small buffer */
            return SetIrpIoStatus(Irp, STATUS_BUFFER_TOO_SMALL, sizeof(KSPIN_DATAFLOW));
        }
        /* store result */
        *((KSPIN_DATAFLOW*)Irp->UserBuffer) = Entry->Pins[Pin->PinId].DataFlow;
        return SetIrpIoStatus(Irp, STATUS_SUCCESS, sizeof(KSPIN_DATAFLOW));
    }
    else
    {
        /* forward request to the filter implementing the property */
        Status = KsSynchronousIoControlDevice(Entry->FileObject, KernelMode, IOCTL_KS_PROPERTY,
                                             (PVOID)IoStack->Parameters.DeviceIoControl.Type3InputBuffer,
                                             IoStack->Parameters.DeviceIoControl.InputBufferLength,
                                             Irp->UserBuffer,
                                             IoStack->Parameters.DeviceIoControl.OutputBufferLength,
                                             &BytesReturned);

        return SetIrpIoStatus(Irp, Status, BytesReturned);
    }
}


NTSTATUS
ComputeCompatibleFormat(
    IN PKSAUDIO_SUBDEVICE_ENTRY Entry,
    IN ULONG PinId,
    IN PSYSAUDIODEVEXT DeviceExtension,
    IN PKSDATAFORMAT_WAVEFORMATEX ClientFormat,
    OUT PKSDATAFORMAT_WAVEFORMATEX MixerFormat)
{
    BOOL bFound;
    ULONG BytesReturned;
    PKSP_PIN PinRequest;
    NTSTATUS Status;
    PKSMULTIPLE_ITEM MultipleItem;
    ULONG Length;
    PKSDATARANGE_AUDIO AudioRange;
    ULONG Index;

    Length = sizeof(KSP_PIN) + sizeof(KSMULTIPLE_ITEM) + ClientFormat->DataFormat.FormatSize;
    PinRequest = ExAllocatePool(NonPagedPool, Length);
    if (!PinRequest)
        return STATUS_UNSUCCESSFUL;

    PinRequest->PinId = PinId;
    PinRequest->Property.Set = KSPROPSETID_Pin;
    PinRequest->Property.Flags = KSPROPERTY_TYPE_GET;
    PinRequest->Property.Id = KSPROPERTY_PIN_DATAINTERSECTION;

    MultipleItem = (PKSMULTIPLE_ITEM)(PinRequest + 1);
    MultipleItem->Count = 1;
    MultipleItem->Size = sizeof(KSMULTIPLE_ITEM) + ClientFormat->DataFormat.FormatSize;

    RtlMoveMemory(MultipleItem + 1, ClientFormat, ClientFormat->DataFormat.FormatSize);
    /* Query the miniport data intersection handler */
    Status = KsSynchronousIoControlDevice(Entry->FileObject, KernelMode, IOCTL_KS_PROPERTY, (PVOID)PinRequest, Length, (PVOID)MixerFormat, sizeof(KSDATAFORMAT_WAVEFORMATEX), &BytesReturned);

    DPRINT("Status %x\n", Status);

    if (NT_SUCCESS(Status))
    {
        ExFreePool(PinRequest);
        return Status;
    }

    /* Setup request block */
    PinRequest->Property.Id = KSPROPERTY_PIN_DATARANGES;
    /* Query pin data ranges */
    Status = KsSynchronousIoControlDevice(Entry->FileObject, KernelMode, IOCTL_KS_PROPERTY, (PVOID)PinRequest, sizeof(KSP_PIN), NULL, 0, &BytesReturned);

    if (Status != STATUS_BUFFER_TOO_SMALL)
    {
        /* Failed to data ranges */
        return Status;
    }

    MultipleItem = ExAllocatePool(NonPagedPool, BytesReturned);
    if (!MultipleItem)
    {
        ExFreePool(PinRequest);
        return STATUS_NO_MEMORY;
    }

    Status = KsSynchronousIoControlDevice(Entry->FileObject, KernelMode, IOCTL_KS_PROPERTY, (PVOID)PinRequest, sizeof(KSP_PIN), (PVOID)MultipleItem, BytesReturned, &BytesReturned);
    if (!NT_SUCCESS(Status))
    {
        DPRINT("Property Request KSPROPERTY_PIN_DATARANGES failed with %x\n", Status);
        ExFreePool(MultipleItem);
        ExFreePool(PinRequest);
        return STATUS_UNSUCCESSFUL;
    }

    AudioRange = (PKSDATARANGE_AUDIO)(MultipleItem + 1);
    bFound = FALSE;
    for(Index = 0; Index < MultipleItem->Count; Index++)
    {
        if (AudioRange->DataRange.FormatSize != sizeof(KSDATARANGE_AUDIO))
        {
            UNIMPLEMENTED
            AudioRange = (PKSDATARANGE_AUDIO)((PUCHAR)AudioRange + AudioRange->DataRange.FormatSize);
            continue;
        }
        /* Select best quality available */

        MixerFormat->DataFormat.FormatSize = sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEX);
        MixerFormat->DataFormat.Flags = 0;
        MixerFormat->DataFormat.Reserved = 0;
        MixerFormat->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
        MixerFormat->DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        MixerFormat->DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
        MixerFormat->DataFormat.SampleSize = 4;
        MixerFormat->WaveFormatEx.wFormatTag = ClientFormat->WaveFormatEx.wFormatTag;
#ifndef NO_AC97_HACK
        /* HACK: AC97 does not support mono render / record */
        MixerFormat->WaveFormatEx.nChannels = 2;
        /*HACK: AC97 only supports 16-Bit Bits */
        MixerFormat->WaveFormatEx.wBitsPerSample = 16;

#else
        MixerFormat->WaveFormatEx.nChannels = min(ClientFormat->WaveFormatEx.nSamplesPerSec, AudioRange->MaximumChannels);
        MixerFormat->WaveFormatEx.wBitsPerSample = AudioRange->MaximumBitsPerSample;
#endif

#ifdef KMIXER_RESAMPLING_IMPLEMENTED
        MixerFormat->WaveFormatEx.nSamplesPerSec = AudioRange->MaximumSampleFrequency;
#else
        MixerFormat->WaveFormatEx.nSamplesPerSec = max(AudioRange->MinimumSampleFrequency, min(ClientFormat->WaveFormatEx.nSamplesPerSec, AudioRange->MaximumSampleFrequency));
#endif

        MixerFormat->WaveFormatEx.cbSize = 0;
        MixerFormat->WaveFormatEx.nBlockAlign = (MixerFormat->WaveFormatEx.nChannels * MixerFormat->WaveFormatEx.wBitsPerSample) / 8;
        MixerFormat->WaveFormatEx.nAvgBytesPerSec = MixerFormat->WaveFormatEx.nChannels * MixerFormat->WaveFormatEx.nSamplesPerSec * (MixerFormat->WaveFormatEx.wBitsPerSample / 8);

        bFound = TRUE;
        break;

        AudioRange = (PKSDATARANGE_AUDIO)((PUCHAR)AudioRange + AudioRange->DataRange.FormatSize);
    }

#if 0
    DPRINT1("\nNum Max Channels %u Channels %u Old Channels %u\n Max SampleRate %u SampleRate %u Old SampleRate %u\n Max BitsPerSample %u BitsPerSample %u Old BitsPerSample %u\n",
           AudioRange->MaximumChannels, MixerFormat->WaveFormatEx.nChannels, ClientFormat->WaveFormatEx.nChannels,
           AudioRange->MaximumSampleFrequency, MixerFormat->WaveFormatEx.nSamplesPerSec, ClientFormat->WaveFormatEx.nSamplesPerSec,
           AudioRange->MaximumBitsPerSample, MixerFormat->WaveFormatEx.wBitsPerSample, ClientFormat->WaveFormatEx.wBitsPerSample);


#endif

    ExFreePool(MultipleItem);
    ExFreePool(PinRequest);

    if (bFound)
        return STATUS_SUCCESS;
    else
        return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
GetPinInstanceCount(
    PKSAUDIO_SUBDEVICE_ENTRY Entry,
    PKSPIN_CINSTANCES PinInstances,
    PKSPIN_CONNECT PinConnect)
{
    KSP_PIN PinRequest;
    ULONG BytesReturned;

    /* query the instance count */
    PinRequest.PinId = PinConnect->PinId;
    PinRequest.Property.Set = KSPROPSETID_Pin;
    PinRequest.Property.Flags = KSPROPERTY_TYPE_GET;
    PinRequest.Property.Id = KSPROPERTY_PIN_CINSTANCES;

    return KsSynchronousIoControlDevice(Entry->FileObject, KernelMode, IOCTL_KS_PROPERTY, (PVOID)&PinRequest, sizeof(KSP_PIN), (PVOID)PinInstances, sizeof(KSPIN_CINSTANCES), &BytesReturned);

}

NTSTATUS
HandleSysAudioFilterPinCreation(
    PIRP Irp,
    PKSPROPERTY Property,
    PSYSAUDIODEVEXT DeviceExtension,
    PDEVICE_OBJECT DeviceObject)
{
    ULONG Length;
    PKSAUDIO_SUBDEVICE_ENTRY Entry;
    KSPIN_CONNECT * PinConnect;
    PIO_STACK_LOCATION IoStack;
    PSYSAUDIO_INSTANCE_INFO InstanceInfo;
    PKSOBJECT_CREATE_ITEM CreateItem;
    NTSTATUS Status;
    KSPIN_CINSTANCES PinInstances;
    PPIN_WORKER_CONTEXT WorkerContext;
    PDISPATCH_CONTEXT DispatchContext;
    PIO_WORKITEM WorkItem;

    IoStack = IoGetCurrentIrpStackLocation(Irp);

    Length = sizeof(KSDATAFORMAT) + sizeof(KSPIN_CONNECT) + sizeof(SYSAUDIO_INSTANCE_INFO);
    if (IoStack->Parameters.DeviceIoControl.InputBufferLength < Length ||
        IoStack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(HANDLE))
    {
        /* invalid parameters */
        return SetIrpIoStatus(Irp, STATUS_INVALID_PARAMETER, 0);
    }

    /* access the create item */
    CreateItem = KSCREATE_ITEM_IRP_STORAGE(Irp);

    /* get input parameter */
    InstanceInfo = (PSYSAUDIO_INSTANCE_INFO)Property;
    if (DeviceExtension->NumberOfKsAudioDevices <= InstanceInfo->DeviceNumber)
    {
        /* invalid parameters */
        return SetIrpIoStatus(Irp, STATUS_INVALID_PARAMETER, 0);
    }

    /* get sysaudio entry */
    Entry = GetListEntry(&DeviceExtension->KsAudioDeviceList, InstanceInfo->DeviceNumber);
    if (!Entry)
    {
        /* invalid device index */
        return SetIrpIoStatus(Irp, STATUS_INVALID_PARAMETER, 0);
    }

    if (!Entry->Pins)
    {
        /* should not happen */
        return SetIrpIoStatus(Irp, STATUS_UNSUCCESSFUL, 0);
    }

    /* get connect details */
    PinConnect = (KSPIN_CONNECT*)(InstanceInfo + 1);

    if (Entry->NumberOfPins <= PinConnect->PinId)
    {
        DPRINT("Invalid PinId %x\n", PinConnect->PinId);
        return SetIrpIoStatus(Irp, STATUS_INVALID_PARAMETER, 0);
    }

    /* query instance count */
    Status = GetPinInstanceCount(Entry, &PinInstances, PinConnect);
    if (!NT_SUCCESS(Status))
    {
        DPRINT("Property Request KSPROPERTY_PIN_GLOBALCINSTANCES failed with %x\n", Status);
        return SetIrpIoStatus(Irp, Status, 0);
    }

    if (PinInstances.PossibleCount == 0)
    {
        /* caller wanted to open an instance-less pin */
        return SetIrpIoStatus(Irp, STATUS_UNSUCCESSFUL, 0);
    }

    if (PinInstances.CurrentCount == PinInstances.PossibleCount)
    {
        /* pin already exists */
        ASSERT(Entry->Pins[PinConnect->PinId].PinHandle != NULL);
        ASSERT(Entry->Pins[PinConnect->PinId].References);

        DPRINT1("Device %u Pin %u References %u is already occupied, try later\n", InstanceInfo->DeviceNumber, PinConnect->PinId, Entry->Pins[PinConnect->PinId].References);
        return SetIrpIoStatus(Irp, STATUS_UNSUCCESSFUL, 0);
    }
    /* create dispatch pin context */
    DispatchContext = ExAllocatePool(NonPagedPool, sizeof(DISPATCH_CONTEXT));
    if (!DispatchContext)
    {
        /* no memory */
        return SetIrpIoStatus(Irp, STATUS_NO_MEMORY, 0);
    }

    /* allocate worker context */
    WorkerContext = ExAllocatePool(NonPagedPool, sizeof(PIN_WORKER_CONTEXT));
    if (!WorkerContext)
    {
        /* no memory */
        ExFreePool(DispatchContext);
        return SetIrpIoStatus(Irp, STATUS_NO_MEMORY, 0);
    }

    /* allocate work item */
    WorkItem = IoAllocateWorkItem(DeviceObject);
    if (!WorkerContext)
    {
        /* no memory */
        ExFreePool(DispatchContext);
        ExFreePool(WorkerContext);
        return SetIrpIoStatus(Irp, STATUS_NO_MEMORY, 0);
    }

    /* prepare context */
    RtlZeroMemory(WorkerContext, sizeof(PIN_WORKER_CONTEXT));
    RtlZeroMemory(DispatchContext, sizeof(DISPATCH_CONTEXT));

    DPRINT("PinInstances.CurrentCount %u\n", PinInstances.CurrentCount);

    if (PinInstances.CurrentCount < PinInstances.PossibleCount)
    {
        WorkerContext->CreateRealPin = TRUE;
    }

    /* set up context */
    WorkerContext->DispatchContext = DispatchContext;
    WorkerContext->Entry = Entry;
    WorkerContext->Irp = Irp;
    WorkerContext->PinConnect = PinConnect;
    WorkerContext->DeviceExtension = DeviceExtension;
    WorkerContext->WorkItem = WorkItem;

    DPRINT("Queing Irp %p\n", Irp);
    /* queue the work item */
    IoMarkIrpPending(Irp);
    Irp->IoStatus.Status = STATUS_PENDING;
    Irp->IoStatus.Information = 0;
    IoQueueWorkItem(WorkItem, CreatePinWorkerRoutine, DelayedWorkQueue, (PVOID)WorkerContext);

    /* mark irp as pending */
    return STATUS_PENDING;
}

NTSTATUS
SysAudioHandleProperty(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    NTSTATUS Status = STATUS_NOT_IMPLEMENTED;
    KSPROPERTY PropertyRequest;
    KSCOMPONENTID ComponentId;
    PULONG Index;
    PKSPROPERTY Property;
    PSYSAUDIODEVEXT DeviceExtension;
    PKSAUDIO_SUBDEVICE_ENTRY Entry;
    PSYSAUDIO_INSTANCE_INFO InstanceInfo;
    ULONG BytesReturned;
    PKSOBJECT_CREATE_ITEM CreateItem;
    UNICODE_STRING GuidString;
    PKSP_PIN Pin;


    /* access the create item */
    CreateItem = KSCREATE_ITEM_IRP_STORAGE(Irp);

    IoStack = IoGetCurrentIrpStackLocation(Irp);

    if (IoStack->Parameters.DeviceIoControl.InputBufferLength < sizeof(KSPROPERTY))
    {
        /* buffer must be at least of sizeof KSPROPERTY */
        return SetIrpIoStatus(Irp, STATUS_BUFFER_TOO_SMALL, sizeof(KSPROPERTY));
    }

    Property = (PKSPROPERTY)IoStack->Parameters.DeviceIoControl.Type3InputBuffer;
    DeviceExtension = (PSYSAUDIODEVEXT)DeviceObject->DeviceExtension;

    if (IsEqualGUIDAligned(&Property->Set, &KSPROPSETID_Pin))
    {
        return HandleSysAudioFilterPinProperties(Irp, Property, DeviceExtension);
    }
    else if(IsEqualGUIDAligned(&Property->Set, &KSPROPSETID_Topology))
    {
        if (IoStack->Parameters.DeviceIoControl.InputBufferLength < sizeof(KSP_PIN))
        {
            /* too small buffer */
            return SetIrpIoStatus(Irp, STATUS_BUFFER_TOO_SMALL, sizeof(KSP_PIN));
        }
        Pin = (PKSP_PIN)IoStack->Parameters.DeviceIoControl.Type3InputBuffer;
        Entry = GetListEntry(&DeviceExtension->KsAudioDeviceList, Pin->Reserved);
        ASSERT(Entry != NULL);

        /* forward request to the filter implementing the property */
        Status = KsSynchronousIoControlDevice(Entry->FileObject, KernelMode, IOCTL_KS_PROPERTY,
                                             (PVOID)IoStack->Parameters.DeviceIoControl.Type3InputBuffer,
                                             IoStack->Parameters.DeviceIoControl.InputBufferLength,
                                             Irp->UserBuffer,
                                             IoStack->Parameters.DeviceIoControl.OutputBufferLength,
                                             &BytesReturned);

        return SetIrpIoStatus(Irp, Status, BytesReturned);
    }
    else if (IsEqualGUIDAligned(&Property->Set, &KSPROPSETID_Sysaudio))
    {
        if (Property->Id == KSPROPERTY_SYSAUDIO_COMPONENT_ID)
        {
            if (IoStack->Parameters.DeviceIoControl.InputBufferLength < sizeof(KSPROPERTY) + sizeof(ULONG))
            {
                /* too small buffer */
                return SetIrpIoStatus(Irp, STATUS_BUFFER_TOO_SMALL, sizeof(KSPROPERTY) + sizeof(ULONG));
            }

            if (IoStack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(KSCOMPONENTID))
            {
                /* too small buffer */
                return SetIrpIoStatus(Irp, STATUS_BUFFER_TOO_SMALL, sizeof(KSCOMPONENTID));
            }

            Index = (PULONG)(Property + 1);

            if (DeviceExtension->NumberOfKsAudioDevices <= *Index)
            {
                /* invalid index */
                return SetIrpIoStatus(Irp, STATUS_INVALID_PARAMETER, 0);
            }
            Entry = GetListEntry(&DeviceExtension->KsAudioDeviceList, *Index);
            ASSERT(Entry != NULL);

            PropertyRequest.Set = KSPROPSETID_General;
            PropertyRequest.Id = KSPROPERTY_GENERAL_COMPONENTID;
            PropertyRequest.Flags = KSPROPERTY_TYPE_GET;

            /* call the filter */
            Status = KsSynchronousIoControlDevice(Entry->FileObject, KernelMode, IOCTL_KS_PROPERTY, (PVOID)&PropertyRequest, sizeof(KSPROPERTY), (PVOID)&ComponentId, sizeof(KSCOMPONENTID), &BytesReturned);
            if (!NT_SUCCESS(Status))
            {
                DPRINT("KsSynchronousIoControlDevice failed with %x for KSPROPERTY_GENERAL_COMPONENTID\n", Status);
                return SetIrpIoStatus(Irp, Status, 0);
            }
            RtlMoveMemory(Irp->UserBuffer, &ComponentId, sizeof(KSCOMPONENTID));
            return SetIrpIoStatus(Irp, STATUS_SUCCESS, sizeof(KSCOMPONENTID));
        }
        else if (Property->Id == KSPROPERTY_SYSAUDIO_DEVICE_COUNT)
        {
            if (IoStack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(ULONG))
            {
                /* too small buffer */
                return SetIrpIoStatus(Irp, STATUS_BUFFER_TOO_SMALL, sizeof(ULONG));
            }

            *((PULONG)Irp->UserBuffer) = DeviceExtension->NumberOfKsAudioDevices;
            return SetIrpIoStatus(Irp, STATUS_SUCCESS, sizeof(ULONG));
        }
        else if (Property->Id == KSPROPERTY_SYSAUDIO_DEVICE_INSTANCE)
        {
            if (IoStack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(ULONG))
            {
                /* too small buffer */
                return SetIrpIoStatus(Irp, STATUS_BUFFER_TOO_SMALL, sizeof(ULONG));
            }

            if (Property->Flags & KSPROPERTY_TYPE_SET)
            {
                Index = (PULONG)Irp->UserBuffer;
                return SysAudioOpenVirtualDevice(Irp, *Index, DeviceExtension);
            }
        }
        else if (Property->Id == KSPROPERTY_SYSAUDIO_INSTANCE_INFO)
        {
            if (IoStack->Parameters.DeviceIoControl.InputBufferLength < sizeof(SYSAUDIO_INSTANCE_INFO))
            {
                /* too small buffer */
                return SetIrpIoStatus(Irp, STATUS_BUFFER_TOO_SMALL, sizeof(SYSAUDIO_INSTANCE_INFO));
            }

            /* get input parameter */
            InstanceInfo = (PSYSAUDIO_INSTANCE_INFO)Property;

            if (Property->Flags & KSPROPERTY_TYPE_SET)
            {
                return SysAudioOpenVirtualDevice(Irp, InstanceInfo->DeviceNumber, DeviceExtension);
            }
        }
        else if (Property->Id == (ULONG)-1)
        {
            /* ros specific pin creation request */
            DPRINT("Initiating create request\n");
            return HandleSysAudioFilterPinCreation(Irp, Property, DeviceExtension, DeviceObject);
        }
    }

    RtlStringFromGUID(&Property->Set, &GuidString);
    DPRINT1("Unhandeled property Set |%S| Id %u Flags %x\n", GuidString.Buffer, Property->Id, Property->Flags);
    DbgBreakPoint();
    RtlFreeUnicodeString(&GuidString);
    return SetIrpIoStatus(Irp, STATUS_UNSUCCESSFUL, 0);
}
