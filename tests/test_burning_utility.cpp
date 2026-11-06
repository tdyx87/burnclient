#include <iostream>
#include <cassert>
#include "../src/burning_utility.h"

int main() {
    std::cout << "=== Burning Utility Unit Tests ===" << std::endl;
    
    const char* platformInfo = GetPlatformInfo();
    std::cout << "Platform: " << (platformInfo ? platformInfo : "Unknown") << std::endl;
    assert(platformInfo != nullptr);
    
    const char* formats = GetSupportedFormats();
    std::cout << "Supported formats: " << (formats ? formats : "Unknown") << std::endl;
    assert(formats != nullptr);
    
    std::cout << "Tool availability:" << std::endl;
    std::cout << "  growisofs: " << (CheckToolAvailability("growisofs") ? "Available" : "Not Available") << std::endl;
    std::cout << "  mkisofs: " << (CheckToolAvailability("mkisofs") ? "Available" : "Not Available") << std::endl;
    std::cout << "  genisoimage: " << (CheckToolAvailability("genisoimage") ? "Available" : "Not Available") << std::endl;
    
    DeviceInfo* devices = nullptr;
    int deviceCount = 0;
    if (ListBurningDevices(&devices, &deviceCount) == BURN_SUCCESS) {
        std::cout << "\nDevice count: " << deviceCount << std::endl;
        for (int i = 0; i < deviceCount; ++i) {
            std::cout << "  Device " << i << ": " << devices[i].devicePath << " (" << devices[i].deviceName << ")" << std::endl;
        }
        if (devices) {
            FreeDeviceList(devices, deviceCount);
        }
    }
    
    std::cout << "\n=== Testing C++ Wrapper ===" << std::endl;
    BurningUtility util;
    std::cout << "Platform (C++): " << util.GetPlatformInfo() << std::endl;
    std::cout << "Formats (C++): " << util.GetSupportedFormats() << std::endl;
    
    ClearLastError();
    std::cout << "Last error: " << GetLastErrorMessage() << std::endl;
    
    std::cout << "\n=== All tests completed ===" << std::endl;
    std::cout << "SUCCESS" << std::endl;
    
    return 0;
}
