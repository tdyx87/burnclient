#include "../src/burning_utility.h"
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <chrono>
#include <thread>

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
              << "  verify <device> <iso>     Verify disc\n"
              << "  burn-job <iso> <device> [type] [speed]  Async burn with live status\n"
              << "  job-selftest [cancel]     Diagnostic job, optionally cancelled\n";
}

/* Start a job and poll its state machine. This is the pattern applications
 * should use: Start -> GetStatus loop -> (optional Cancel) -> Wait -> Release. */
static int runJob(const BurnJobOptions& options, bool cancelAt20) {
    BurnJobHandle handle = Burn_Start(&options);
    if (handle == BURN_INVALID_JOB_HANDLE) {
        std::cout << "Burn_Start failed: " << GetLastErrorMessage() << "\n";
        return 1;
    }

    bool cancelled = false;
    for (;;) {
        BurnJobStatus status;
        std::memset(&status, 0, sizeof(status));
        status.size = (int)sizeof(status);
        if (Burn_GetStatus(handle, &status) != BURN_SUCCESS) {
            std::cout << "Burn_GetStatus failed\n";
            break;
        }
        std::cout << "state=" << status.state << " " << status.percent
                  << "% phase=" << status.phase << " " << status.message << "\n";
        if (status.state != BURN_JOB_PENDING && status.state != BURN_JOB_RUNNING) break;
        if (cancelAt20 && !cancelled && status.percent >= 20) {
            std::cout << "Cancelling...\n";
            Burn_Cancel(handle);
            cancelled = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    if (Burn_Wait(handle, 10000) != BURN_SUCCESS) {
        std::cout << "Timed out waiting for the job\n";
    }
    BurnJobStatus final;
    std::memset(&final, 0, sizeof(final));
    final.size = (int)sizeof(final);
    int result = 1;
    if (Burn_GetStatus(handle, &final) == BURN_SUCCESS) {
        std::cout << "final: state=" << final.state << " code=" << final.errorCode
                  << " elapsed=" << final.elapsedMs << "ms\n";
        if (final.errorMessage[0]) {
            std::cout << "error: " << final.errorMessage << "\n";
        }
        result = final.state == BURN_JOB_COMPLETED ? 0 : 1;
    }
    Burn_Release(handle);
    return result;
}

static BurnJobOptions makeOptions(int mode) {
    BurnJobOptions options;
    std::memset(&options, 0, sizeof(options));
    options.size = (int)sizeof(options);
    options.mode = mode;
    return options;
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
    if (cmd == "burn-job" && argc >= 4) {
        BurnJobOptions options = makeOptions(BURN_JOB_MODE_IMAGE);
        options.sourcePath = argv[2];
        options.devicePath = argv[3];
        options.discType = (argc >= 5) ? argv[4] : "CD-R";
        options.speed = (argc >= 6) ? std::stoi(argv[5]) : 4;
        return runJob(options, false);
    }
    if (cmd == "job-selftest") {
        BurnJobOptions options = makeOptions(BURN_JOB_MODE_SELFTEST);
        return runJob(options, argc >= 3 && std::string(argv[2]) == "cancel");
    }

    printUsage();
    return 1;
}
