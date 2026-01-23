#pragma once
//#include <Windows.h>
#define MAX_DEVICES 64
#define NAME_BUFFER 256

typedef struct _DEVICE_LIST_DATA {
	short Count;
	WCHAR DeviceNames[MAX_DEVICES][NAME_BUFFER];
	bool blocked[MAX_DEVICES];
} DEVICE_LIST_DATA, *PDEVICE_LIST_DATA;