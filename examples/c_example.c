#include "../src/burning_utility.h"
#include <stdio.h>
#include <stdlib.h>

int main() {
    printf("=== Burning Utility C Example ===\n\n");

    /* GetPlatformInfo and GetSupportedFormats return pointers to internal
     * static buffers. They must NOT be freed with FreeStringMemory.
     * Only strings returned via malloc'd pointers (e.g. future dynamic
     * error strings) should be freed. */
    const char* platformInfo = GetPlatformInfo();
    printf("Platform: %s\n", platformInfo ? platformInfo : "Unknown");

    const char* formats = GetSupportedFormats();
    printf("Supported formats: %s\n", formats ? formats : "Unknown");

    printf("\nTool availability:\n");
    printf("  growisofs: %s\n", CheckToolAvailability("growisofs") ? "Available" : "Not Available");
    printf("  mkisofs: %s\n", CheckToolAvailability("mkisofs") ? "Available" : "Not Available");
    printf("  genisoimage: %s\n", CheckToolAvailability("genisoimage") ? "Available" : "Not Available");
    printf("  wodim: %s\n", CheckToolAvailability("wodim") ? "Available" : "Not Available");

    printf("\n=== Available Devices ===\n");
    DeviceInfo* devices = NULL;
    int deviceCount = 0;
    if (ListBurningDevices(&devices, &deviceCount) == BURN_SUCCESS) {
        if (deviceCount == 0) {
            printf("No devices found\n");
        } else {
            for (int i = 0; i < deviceCount; ++i) {
                printf("Device %d:\n", i);
                printf("  Path: %s\n", devices[i].devicePath);
                printf("  Name: %s\n", devices[i].deviceName);
                printf("  Media Type: %s\n", devices[i].mediaType);
                printf("  Writable: %s\n", devices[i].isWritable ? "Yes" : "No");
                printf("  Empty: %s\n", devices[i].isEmpty ? "Yes" : "No");
                printf("\n");
            }
        }
        FreeDeviceList(devices, deviceCount);
    } else {
        printf("Failed to list devices: %s\n", GetLastErrorMessage());
    }

    printf("=== Configuration ===\n");
    SetDefaultDevice("D:");
    SetDefaultDiscType("DVD-R");
    SetDefaultSpeed(4);

    char defaultDev[512];
    if (GetDefaultDevice(defaultDev, sizeof(defaultDev)) == BURN_SUCCESS) {
        printf("Default device: %s\n", defaultDev);
    }

    printf("\n=== Example completed ===\n");

    return 0;
}
