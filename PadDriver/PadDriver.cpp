#include <ntddk.h>
#include "Common.h"
#include "PadDriver.h"

#define TAG "WPKS"
#define TAG_MEM 'SKPW'

struct DeviceExtension
{
	PDEVICE_OBJECT PDO;
	PDEVICE_OBJECT LowerDeviceObject;
	short index;
	IO_REMOVE_LOCK RemoveLock;
};

struct Monitored_Device
{
	WCHAR name[NAME_BUFFER];
	PDEVICE_OBJECT DeviceObject;
	bool blocked;
};

struct g_data
{
	PDEVICE_OBJECT CDO;
	Monitored_Device devices[MAX_DEVICES];
	short Count;
	FAST_MUTEX fMutex;
};

g_data* globals;

NTSTATUS CompleteIrp(PIRP Irp, NTSTATUS status = STATUS_SUCCESS, ULONG_PTR info = 0)
{
	Irp->IoStatus.Status = status;
	Irp->IoStatus.Information = info;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return status;
}


void SampleUnload(_In_ PDRIVER_OBJECT DriverObject)
{
	ExFreePoolWithTag(globals, TAG_MEM);
	UNICODE_STRING symLink = RTL_CONSTANT_STRING(L"\\??\\HidePad");
	IoDeleteSymbolicLink(&symLink);
	IoDeleteDevice(DriverObject->DeviceObject);
	KdPrint(("Unloaded driver.\n"));
}

NTSTATUS FilterAddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PDO)
{
	KdPrint((TAG ": AddDevice INVOKED."));
	short index = -1;
	PDEVICE_OBJECT DeviceObject;
	NTSTATUS status;
	if (globals->Count > 63)
	{
		return STATUS_TOO_MANY_ADDRESSES;
	}
	status = IoCreateDevice(DriverObject, sizeof(DeviceExtension), nullptr, FILE_DEVICE_UNKNOWN, 0, FALSE, &DeviceObject);
	//UNREFERENCED_PARAMETER(DriverObject);
	if (!NT_SUCCESS(status))
	{
		KdPrint((TAG ": ERROR(IoCreateDevice, AddDevice) (0x%X)\n", status));
		return status;
	}

	auto ext = (DeviceExtension*)DeviceObject->DeviceExtension;
	ext->index = -1;

	status = IoAttachDeviceToDeviceStackSafe(DeviceObject, PDO, &ext->LowerDeviceObject);

	if (!NT_SUCCESS(status))
	{
		KdPrint((TAG ": ERROR(IoAttachDeviceToDeviceStackSafe, AddDevice) (0x%X)\n", status));
		IoDeleteDevice(DeviceObject);
		return status;
	}

	ext->PDO = PDO;

	WCHAR temp_name[NAME_BUFFER];
	ULONG size = 0;
	status = IoGetDeviceProperty(PDO, DevicePropertyHardwareID, 0, NULL, &size);
	if (!NT_SUCCESS(status))
	{
		KdPrint((TAG ": ERROR(IoGetDeviceProperty1, AddDevice) (0x%X)\n", status));
		IoDetachDevice(ext->LowerDeviceObject);
		IoDeleteDevice(DeviceObject);
		return status;
	}

	if (size > sizeof(WCHAR) * NAME_BUFFER)
	{
		KdPrint((TAG ": ERROR(NAME_BUFFER TOO SMALL[ %lu / %hd ], AddDevice) (0x%X)\n", size, NAME_BUFFER, status));
		IoDetachDevice(ext->LowerDeviceObject);
		IoDeleteDevice(DeviceObject);
		return STATUS_BUFFER_TOO_SMALL;
	}

	status = IoGetDeviceProperty(PDO, DevicePropertyHardwareID, NAME_BUFFER * sizeof(WCHAR), temp_name, &size);

	if (!NT_SUCCESS(status))
	{
		KdPrint((TAG ": ERROR(IoGetDeviceProperty2, AddDevice) (0x%X)\n", status));
		IoDetachDevice(ext->LowerDeviceObject);
		IoDeleteDevice(DeviceObject);
		return status;
	}

	ExAcquireFastMutex(&globals->fMutex);
	for (short i = 0; i < MAX_DEVICES; i++)
	{
		if (globals->devices[i].DeviceObject == nullptr)
		{
			if (index < 0)
			{
				index = i;
			}
		}
		else
		{
			if (_wcsicmp(temp_name, globals->devices[i].name) == 0)
			{
				globals->devices[i].DeviceObject = DeviceObject; //if exists, insert pointer
				ext->index = i;
				break;
			}
		}
	}
	if (ext->index == -1)
	{
		ext->index = index; //if doesnt exist, insert into first available slot
		globals->devices[ext->index].DeviceObject = DeviceObject;
		globals->devices[ext->index].blocked = false;
		wcsncpy(globals->devices[ext->index].name, temp_name, NAME_BUFFER);
		//status = IoGetDeviceProperty(PDO, DevicePropertyHardwareID, NAME_BUFFER * sizeof(WCHAR), globals->devices[ext->index].name, &size);
		/*if (!NT_SUCCESS(status))
		{
			KdPrint((TAG ": ERROR(IoGetDeviceProperty2, AddDevice) (0x%X)\n", status));
			IoDetachDevice(ext->LowerDeviceObject);
			IoDeleteDevice(DeviceObject);
			return status;
		}*/
		globals->Count++;
	}
	ExReleaseFastMutex(&globals->fMutex);
	IoInitializeRemoveLock(&ext->RemoveLock, TAG_MEM, 0, 0);
	DeviceObject->DeviceType = ext->LowerDeviceObject->DeviceType;
	DeviceObject->Flags |= ext->LowerDeviceObject->Flags & (DO_BUFFERED_IO | DO_DIRECT_IO);

	DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
	DeviceObject->Flags |= DO_POWER_PAGABLE;

	KdPrint((TAG ": Connected to device named %ws, index: %hd, globals_count: %hd\n", temp_name, ext->index, globals->Count));
	//UNREFERENCED_PARAMETER(PDO);
	return status;
}

NTSTATUS DriverRead(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	UNREFERENCED_PARAMETER(DeviceObject);
	//NTSTATUS status = STATUS_SUCCESS;
	auto stack = IoGetCurrentIrpStackLocation(Irp);
	auto len = stack->Parameters.Read.Length;
	if (len < sizeof(DEVICE_LIST_DATA))
	{
		return CompleteIrp(Irp, STATUS_INVALID_BUFFER_SIZE);
	}
	
	PDEVICE_LIST_DATA buffer = (PDEVICE_LIST_DATA)Irp->AssociatedIrp.SystemBuffer;

	memset(buffer, 0, sizeof(DEVICE_LIST_DATA));

	short deviceCount = 0;
	ExAcquireFastMutex(&globals->fMutex);
	for (int i = 0; i < MAX_DEVICES; i++)
	{
		if (globals->devices[i].name[0] != '\0')
		{
			wcsncpy(buffer->DeviceNames[deviceCount], globals->devices[i].name, NAME_BUFFER);
			deviceCount++;
		}
	}
	ExReleaseFastMutex(&globals->fMutex);
	buffer->Count = deviceCount;

	return CompleteIrp(Irp, STATUS_SUCCESS, sizeof(DEVICE_LIST_DATA));
}

NTSTATUS DriverDeviceIntercept(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
	auto deviceState = (PULONG)Irp->IoStatus.Information;
	auto status = Irp->IoStatus.Status;
	if (deviceState == nullptr)
	{
		KdPrint((TAG ": ERROR(DEVICE STATE NULL POINTER, DeviceIntercept) (0x%X)\n", status));
		return status;
	}
	UNREFERENCED_PARAMETER(Context);
	for (int i = 0; i < MAX_DEVICES; i++)
	{
		if (globals->devices[i].DeviceObject != DeviceObject)
		{
			continue;
		}
		if (globals->devices[i].blocked == true)
		{
			*deviceState |= (PNP_DEVICE_DONT_DISPLAY_IN_UI | PNP_DEVICE_DISABLED);
		}
		else
		{
			*deviceState &= ~(PNP_DEVICE_DONT_DISPLAY_IN_UI | PNP_DEVICE_DISABLED);
		}
	}
	return status;
	//devices->Objects[0]->
}
/*
NTSTATUS DriverQueryDeviceRelations(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	NTSTATUS status = STATUS_SUCCESS;
	auto stack = IoGetCurrentIrpStackLocation(Irp);
	auto type = stack->Parameters.QueryDeviceRelations.Type;
	auto ext = (DeviceExtension*)DeviceObject->DeviceExtension;
	if (globals->Count > 0)
	{
		if (type == BusRelations)
		{
			IoCopyCurrentIrpStackLocationToNext(Irp);
			IoSetCompletionRoutine(Irp, DriverDeviceIntercept, NULL, TRUE, FALSE, FALSE);

		}
	}
	else
	{
		IoSkipCurrentIrpStackLocation(Irp);
		status = IoCallDriver(ext->LowerDeviceObject, Irp);
	}

	return status;
}*/

NTSTATUS DriverQueryDeviceState(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	NTSTATUS status = STATUS_SUCCESS;
	//auto stack = IoGetCurrentIrpStackLocation(Irp);
	auto ext = (DeviceExtension*)DeviceObject->DeviceExtension;
	if (globals->Count > 0)
	{
		IoCopyCurrentIrpStackLocationToNext(Irp);
		IoSetCompletionRoutine(Irp, DriverDeviceIntercept, NULL, TRUE, FALSE, FALSE); // only on success
	}
	else
	{
		IoSkipCurrentIrpStackLocation(Irp);
		status = IoCallDriver(ext->LowerDeviceObject, Irp);
	}
	return status;
}

NTSTATUS DriverPNP(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	NTSTATUS status = STATUS_SUCCESS;
	auto stack = IoGetCurrentIrpStackLocation(Irp);
	auto code = stack->MinorFunction;
	auto ext = (DeviceExtension*)DeviceObject->DeviceExtension;
	if (code == IRP_MN_REMOVE_DEVICE)
	{
		IoReleaseRemoveLockAndWait(&ext->RemoveLock, Irp);
		IoSkipCurrentIrpStackLocation(Irp);
		status = IoCallDriver(ext->LowerDeviceObject, Irp);

		IoDetachDevice(ext->LowerDeviceObject);
		ExAcquireFastMutex(&globals->fMutex);
		globals->devices[ext->index].DeviceObject = nullptr;
		if (globals->devices[ext->index].blocked == false)
		{
			memset(globals->devices[ext->index].name, 0, sizeof(globals->devices[ext->index].name));
			globals->Count--;
		}
		ExReleaseFastMutex(&globals->fMutex);
		IoDeleteDevice(DeviceObject);
		return status;
	}
	else
	{
		switch (code)
		{
			/*case IRP_MN_QUERY_DEVICE_RELATIONS:
				status = DriverQueryDeviceRelations(DeviceObject, Irp);
				break;*/
			case IRP_MN_QUERY_PNP_DEVICE_STATE:
				status = DriverQueryDeviceState(DeviceObject, Irp);
				break;

			default:
				IoSkipCurrentIrpStackLocation(Irp);
				status = IoCallDriver(ext->LowerDeviceObject, Irp);
				break;
		}
		IoReleaseRemoveLock(&ext->RemoveLock, Irp);
	}
	return status;
}

NTSTATUS DriverControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	UNREFERENCED_PARAMETER(DeviceObject);
	auto stack = IoGetCurrentIrpStackLocation(Irp);
	auto& dic = stack->Parameters.DeviceIoControl;
	//NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
	auto inputLen = dic.InputBufferLength;
	if (inputLen == 0 || inputLen > sizeof(WCHAR)*NAME_BUFFER)
	{
		return CompleteIrp(Irp, STATUS_INVALID_BUFFER_SIZE);
	}
	WCHAR* temp_name = (WCHAR*)Irp->AssociatedIrp.SystemBuffer;
	WCHAR buffer[256];
	memset(buffer, 0, sizeof(buffer));
	wcsncpy(buffer, temp_name, inputLen/sizeof(WCHAR) - 1);
	buffer[255] = '\0';

	switch (dic.IoControlCode)
	{
		case IOCTL_DEV_ADD:
			ExAcquireFastMutex(&globals->fMutex);
			for (int i = 0; i < MAX_DEVICES; i++)
			{
				if (_wcsicmp(buffer, globals->devices[i].name) == 0 && globals->devices[i].DeviceObject != nullptr)
				{
					globals->devices[i].blocked = true;
					auto ext = (DeviceExtension*)globals->devices[i].DeviceObject->DeviceExtension;
					ExReleaseFastMutex(&globals->fMutex);
					IoInvalidateDeviceState(ext->PDO);
					return CompleteIrp(Irp);
				}
			}
			ExReleaseFastMutex(&globals->fMutex);
			break;
		case IOCTL_DEV_REMOVE:
			ExAcquireFastMutex(&globals->fMutex);
			for (int i = 0; i < MAX_DEVICES; i++)
			{
				if (_wcsicmp(buffer, globals->devices[i].name) == 0 && globals->devices[i].DeviceObject == nullptr)
				{
					globals->devices[i].blocked = false;
					memset(globals->devices[i].name, 0, sizeof(globals->devices[i].name));
					ExReleaseFastMutex(&globals->fMutex);
					return CompleteIrp(Irp);
				}

				if (_wcsicmp(buffer, globals->devices[i].name) == 0 && globals->devices[i].DeviceObject != nullptr)
				{
					globals->devices[i].blocked = false;
					auto ext = (DeviceExtension*)globals->devices[i].DeviceObject->DeviceExtension;
					ExReleaseFastMutex(&globals->fMutex);
					IoInvalidateDeviceState(ext->PDO);
					return CompleteIrp(Irp);
				}
			}
			ExReleaseFastMutex(&globals->fMutex);
			break;
	}
	return CompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST);
}

NTSTATUS DriverDispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	NTSTATUS status;
	auto stack = IoGetCurrentIrpStackLocation(Irp);
	auto code = stack->MajorFunction;
	auto ext = (DeviceExtension*)DeviceObject->DeviceExtension;
	KdPrint((TAG ": Dispatch major function(%d) \n", stack->MajorFunction ));
	if (DeviceObject == globals->CDO)
	{ //is deviceobject our main device or filter devices(intercepters)
		switch (code)
		{
			case IRP_MJ_CREATE:
			case IRP_MJ_CLOSE:
			case IRP_MJ_WRITE:
				return CompleteIrp(Irp);
				break;
			case IRP_MJ_READ:
				return DriverRead(DeviceObject, Irp);
				break;
			case IRP_MJ_DEVICE_CONTROL:
				return DriverControl(DeviceObject, Irp);
				break;
			case IRP_MJ_SHUTDOWN:
			case IRP_MJ_CLEANUP:
				KdPrint((TAG ": Processing %s for CDO\n",
					(code == IRP_MJ_SHUTDOWN) ? "SHUTDOWN" : "CLEANUP"));
				return CompleteIrp(Irp);
			default:
				return CompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST);
		}
	}
	else
	{
		status = IoAcquireRemoveLock(&ext->RemoveLock, Irp);
		if (!NT_SUCCESS(status))
		{
			KdPrint((TAG ": ERROR(IoAcquireRemoveLock, DriverDispatch) (0x%X)\n", status));
			Irp->IoStatus.Status = status;
			CompleteIrp(Irp);
			return status;
		}
		if (code == IRP_MJ_PNP)
		{
			status = DriverPNP(DeviceObject, Irp);
		}
		else
		{
			IoSkipCurrentIrpStackLocation(Irp);
			status = IoCallDriver(ext->LowerDeviceObject, Irp);
			IoReleaseRemoveLock(&ext->RemoveLock, Irp);
		}
	}
	return status;
}

extern "C" NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
	//UNREFERENCED_PARAMETER(DriverObject);
	UNREFERENCED_PARAMETER(RegistryPath);
	UNICODE_STRING devName = RTL_CONSTANT_STRING(L"\\Device\\HidePad");
	PDEVICE_OBJECT DeviceObject;

	auto status = IoCreateDevice(DriverObject, 0, &devName, FILE_DEVICE_UNKNOWN, 0, TRUE, &DeviceObject);
	if (!NT_SUCCESS(status))
	{
		KdPrint((TAG ": ERROR(IoCreateDevice, DriverEntry) (0x%X)\n", status));
		return status;
	}
	DeviceObject->Flags |= DO_BUFFERED_IO;

	globals = (g_data*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(g_data), TAG_MEM);
	if (globals != nullptr)
	{
		memset(globals, 0, sizeof(g_data));
	}
	else
	{
		KdPrint((TAG ": ERROR(ExAllocatePool2, DriverEntry, size g_data: %lu) (0x%X)\n", sizeof(g_data), status));
		IoDeleteDevice(DeviceObject);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	globals->CDO = DeviceObject;

	UNICODE_STRING linkName = RTL_CONSTANT_STRING(L"\\??\\HidePad");
	status = IoCreateSymbolicLink(&linkName, &devName);
	if (!NT_SUCCESS(status))
	{
		KdPrint((TAG ": ERROR(IoCreateSymbolicLink, DriverEntry) (0x%X)\n", status));
		IoDeleteDevice(DeviceObject);
		ExFreePoolWithTag(globals, TAG_MEM);
		return status;
	}

	/*
	for (int i = 0; i < ARRAYSIZE(DriverObject->MajorFunction); i++)
	{
		DriverObject->MajorFunction[i] = DriverDispatch;
	}
	*/
	for (auto& func : DriverObject->MajorFunction)
		func = DriverDispatch;

	DriverObject->DriverExtension->AddDevice = FilterAddDevice;
	DriverObject->DriverUnload = SampleUnload;
	ExInitializeFastMutex(&globals->fMutex);
	KdPrint((TAG ": STARTED\n"));

	/*
	RTL_OSVERSIONINFOW info;
	char xd[20] = { 0 };
	//UNICODE_STRING myString = Init;
	KdPrint(("Loaded driver.\n"));
	RtlGetVersion(&info);
	KdPrint(("Build: %d \n", info.dwBuildNumber));
	KdPrint(("Major: %d\n", info.dwMajorVersion));
	KdPrint(("Minor: %d\n", info.dwMinorVersion));
	KdPrint(("Version: %d\n", info.dwOSVersionInfoSize));
	KdPrint(("Platform ID: %d\n", info.dwPlatformId));
	KdPrint(("CSD VERSION: %s\n", info.szCSDVersion));
	//RtlInitUnicodeString(&myString, xd);
	POOL_TYPE pool = PagedPool;
	*/
	return STATUS_SUCCESS;
}