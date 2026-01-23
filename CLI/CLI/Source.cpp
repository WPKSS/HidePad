#include <stdio.h>
#include <Windows.h>
#include "PadDriver.h"
#include "Common.h"
int wmain(int argc, wchar_t* argv[])
{
	HANDLE hDevice = CreateFile(L"\\\\.\\PadDriver",
		GENERIC_READ | GENERIC_WRITE, 0,
		nullptr, OPEN_EXISTING, 0, nullptr);
	if (hDevice == INVALID_HANDLE_VALUE)
	{
		printf("ERROR\n");
		return 1;
	}
	PDEVICE_LIST_DATA data = (PDEVICE_LIST_DATA)malloc(sizeof(DEVICE_LIST_DATA));

	DWORD bytes;
	WCHAR buffer[NAME_BUFFER];
	while(true)
	{
		BOOL ok = ReadFile(hDevice, data, sizeof(*data), &bytes, nullptr);
		if (!ok)
		{
			printf("failed to read");
		}
		if (bytes != sizeof(*data))
			printf("Wrong number of bytes\n");
		int number = 0;
		for (int i = 0; i < data->Count; i++)
		{
			if(data->DeviceNames[i][0] != '\0') 
				printf("%d %ws, BLOCKED: %d\n", i, data->DeviceNames[i], data->blocked[i]);
		}
		printf("1. Hide device. \n 2. Refresh \n 3. Quit \n 4. Monitor device");
		scanf_s("%d", &number);
		if(number == 1)
		{
			int device = 0;
			printf("Write number of device to hide: ");
			scanf_s("%d", &device);
			if(device >= MAX_DEVICES)
			{
				printf("WRONG NUMBER, MAX 63"); continue;
			}
			DWORD bytesReturned;
			if(data->blocked[device] == false)
			{
				ok = DeviceIoControl(hDevice, IOCTL_DEV_ADD, data->DeviceNames[device], sizeof(data->DeviceNames[device]), nullptr, 0, &bytesReturned, nullptr);
			}
			else
			{
				ok = DeviceIoControl(hDevice, IOCTL_DEV_REMOVE, data->DeviceNames[device], sizeof(data->DeviceNames[device]), nullptr, 0, &bytesReturned, nullptr);
			}
		}
		else if(number == 2)
		{
			continue;
		}
		else if(number == 3)
		{
			break;
		}
		else if(number == 4)
		{
			memset(buffer, 0, sizeof(buffer));
			printf("Write name of the device: ");
			scanf_s("%ws", buffer, NAME_BUFFER);
			ok = WriteFile(hDevice,  &buffer, sizeof(buffer), &bytes, nullptr);
		}
        else
        {
            printf("WRONG NUMBER\n");
        }
	}
	return 0;
}