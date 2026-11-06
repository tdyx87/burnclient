#include "../src/burning_utility.h"
#include <iostream>
#include <vector>

void progressCallback(int percent, const char* status, void* userData) {
    std::cout << "Progress: " << percent << "% - " << status << std::endl;
}

int main() {
    std::cout << "=== Burning Utility C++ Example ===" << std::endl << std::endl;
    
    BurningUtility util;
    util.SetProgressCallback(progressCallback, nullptr);
    
    std::cout << "Platform: " << util.GetPlatformInfo() << std::endl;
    std::cout << "Supported formats: " << util.GetSupportedFormats() << std::endl;
    
    std::cout << "\nTool availability:" << std::endl;
    std::cout << "  growisofs: " << (util.CheckToolAvailability("growisofs") ? "Available" : "Not Available") << std::endl;
    std::cout << "  mkisofs: " << (util.CheckToolAvailability("mkisofs") ? "Available" : "Not Available") << std::endl;
    std::cout << "  genisoimage: " << (util.CheckToolAvailability("genisoimage") ? "Available" : "Not Available") << std::endl;
    std::cout << "  wodim: " << (util.CheckToolAvailability("wodim") ? "Available" : "Not Available") << std::endl;
    
    std::cout << "\n=== Available Devices ===" << std::endl;
    std::vector<DeviceInfo> devices = util.ListBurningDevices();
    if (devices.empty()) {
        std::cout << "No devices found" << std::endl;
    } else {
        for (size_t i = 0; i < devices.size(); ++i) {
            std::cout << "Device " << i << ":" << std::endl;
            std::cout << "  Path: " << devices[i].devicePath << std::endl;
            std::cout << "  Name: " << devices[i].deviceName << std::endl;
            std::cout << "  Media Type: " << devices[i].mediaType << std::endl;
            std::cout << "  Writable: " << (devices[i].isWritable ? "Yes" : "No") << std::endl;
            std::cout << "  Empty: " << (devices[i].isEmpty ? "Yes" : "No") << std::endl << std::endl;
        }
    }
    
    std::cout << "=== Configuration ===" << std::endl;
    util.SetDefaultDevice("D:");
    util.SetDefaultDiscType("DVD-R");
    util.SetDefaultSpeed(4);
    std::cout << "Default device: " << util.GetDefaultDevice() << std::endl;
    
    std::cout << "\n=== Example Usage ===" << std::endl;
    std::cout << "// util.CreateISOImage(\"./data\", \"./output.iso\", \"MYDISC\");" << std::endl;
    std::cout << "// util.CreateISOToDisc(\"./output.iso\", \"D:\", \"DVD-R\", 4);" << std::endl;
    std::cout << "// util.BurnDirectoryToDisc(\"./data\", \"D:\", \"DVD-R\", 4);" << std::endl;
    
    std::string lastError = util.GetLastErrorMessage();
    if (!lastError.empty()) {
        std::cout << "\nLast error: " << lastError << std::endl;
    }
    
    std::cout << "\n=== Example completed ===" << std::endl;
    return 0;
}
