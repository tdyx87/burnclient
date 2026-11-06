#include "../src/burning_utility.h"
#include <iostream>
#include <vector>
#include <string>
#include <cstring>

static void progressCallback(int percent, const char* status, void* userData) {
    std::cout << "\rProgress: " << percent << "% - " << status << std::flush;
    if (percent >= 100) std::cout << std::endl;
}

static void printUsage() {
    std::cout << "Usage: burn_cli <command> [args]\n"
              << "Commands:\n"
              << "  list-devices              List available burning devices\n"
              << "  platform-info            Show platform info\n"
              << "  tool-avail <tool>         Check tool availability\n"
              << "  create-iso <src> <iso> [label]   Create ISO from directory\n"
              << "  burn-iso <iso> <device> [type] [speed]  Burn ISO to device\n"
              << "  burn-dir <src> <device> [type] [speed] Burn directory to device\n"
              << "  verify <device> <iso>     Verify disc\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) { printUsage(); return 1; }
    std::string cmd = argv[1];
    BurningUtility util;
    util.SetProgressCallback(progressCallback, nullptr);

    if (cmd == "list-devices") {
        auto devices = util.ListBurningDevices();
        std::cout << "Devices: " << devices.size() << "\n";
        for (size_t i = 0; i < devices.size(); ++i) {
            std::cout << i << ": " << devices[i].devicePath << " (" << devices[i].deviceName
                      << ") type=" << devices[i].mediaType
                      << " writable=" << devices[i].isWritable
                      << " empty=" << devices[i].isEmpty << "\n";
        }
        return 0;
    }
    if (cmd == "platform-info") {
        std::cout << util.GetPlatformInfo() << "\n";
        return 0;
    }
    if (cmd == "tool-avail" && argc >= 3) {
        std::cout << (util.CheckToolAvailability(argv[2]) ? "available" : "not available") << "\n";
        return 0;
    }
    if (cmd == "create-iso" && argc >= 4) {
        std::string label = (argc >= 5) ? argv[4] : "DISC";
        bool ok = util.CreateISOImage(argv[2], argv[3], label);
        std::cout << (ok ? "OK" : "FAILED: " + util.GetLastErrorMessage()) << "\n";
        return ok ? 0 : 1;
    }
    if (cmd == "burn-iso" && argc >= 4) {
        std::string type = (argc >= 5) ? argv[4] : "CD-R";
        int speed = (argc >= 6) ? std::stoi(argv[5]) : 4;
        bool ok = util.CreateISOToDisc(argv[2], argv[3], type, speed);
        std::cout << (ok ? "OK" : "FAILED: " + util.GetLastErrorMessage()) << "\n";
        return ok ? 0 : 1;
    }
    if (cmd == "burn-dir" && argc >= 4) {
        std::string type = (argc >= 5) ? argv[4] : "CD-R";
        int speed = (argc >= 6) ? std::stoi(argv[5]) : 4;
        bool ok = util.BurnDirectoryToDisc(argv[2], argv[3], type, speed);
        std::cout << (ok ? "OK" : "FAILED: " + util.GetLastErrorMessage()) << "\n";
        return ok ? 0 : 1;
    }
    if (cmd == "verify" && argc >= 4) {
        bool ok = util.VerifyDisc(argv[2], argv[3]);
        std::cout << (ok ? "OK" : "FAILED: " + util.GetLastErrorMessage()) << "\n";
        return ok ? 0 : 1;
    }
    printUsage();
    return 1;
}
