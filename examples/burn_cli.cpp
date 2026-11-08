/* "burning_utility.h" resolves both inside the source tree (CMake passes
 * -I src) and inside an installed SDK (compile with -I include). A path
 * like "../src/..." breaks the moment the file is shipped without src/,
 * which is exactly what the SDK package does. */
#include "burning_utility.h"
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <chrono>
#include <thread>

#ifdef _WIN32
    #include <direct.h>
    #include <process.h>
    #define MKDIR(p) _mkdir(p)
    #define RMDIR(p) _rmdir(p)
    #define GETPID _getpid
#else
    #include <sys/stat.h>
    #include <unistd.h>
    #define MKDIR(p) mkdir((p), 0755)
    #define RMDIR(p) rmdir(p)
    #define GETPID getpid
#endif

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
              << "  job-selftest [cancel]     Diagnostic job, optionally cancelled\n"
              << "  burn-test                 Integration test: burn a generated ISO\n"
              << "                            to the first writable device; SKIPs\n"
              << "                            cleanly when no writable burner exists\n";
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
            std::cout << "Burn_GetStatus failed" << std::endl;
            break;
        }
        std::cout << "state=" << status.state << " " << status.percent
                  << "% phase=" << status.phase << " " << status.message
                  << std::endl;
        if (status.state != BURN_JOB_PENDING && status.state != BURN_JOB_RUNNING) break;
        if (cancelAt20 && !cancelled && status.percent >= 20) {
            std::cout << "Cancelling..." << std::endl;
            Burn_Cancel(handle);
            cancelled = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    if (Burn_Wait(handle, 10000) != BURN_SUCCESS) {
        std::cout << "Timed out waiting for the job" << std::endl;
    }
    BurnJobStatus final;
    std::memset(&final, 0, sizeof(final));
    final.size = (int)sizeof(final);
    int result = 1;
    if (Burn_GetStatus(handle, &final) == BURN_SUCCESS) {
        std::cout << "final: state=" << final.state << " code=" << final.errorCode
                  << " elapsed=" << final.elapsedMs << "ms" << std::endl;
        if (final.errorMessage[0]) {
            std::cout << "error: " << final.errorMessage << std::endl;
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
        std::cout << "Devices: " << devices.size() << std::endl;
        for (size_t i = 0; i < devices.size(); ++i) {
            std::cout << i << ": " << devices[i].devicePath << " (" << devices[i].deviceName
                      << ") type=" << devices[i].mediaType
                      << " writable=" << devices[i].isWritable
                      << " empty=" << devices[i].isEmpty << std::endl;
        }
        return 0;
    }
    if (cmd == "platform-info") {
        std::cout << util.GetPlatformInfo() << std::endl;
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
    if (cmd == "burn-test") {
        /* Integration test against a real (possibly virtual) burner.
         * Exit codes: 0 = PASS or SKIP, 1 = FAIL. Designed so CI machines
         * without a burner stay green while machines with a burner get a
         * full create-image + async-burn + status-machine workout.
         * All output uses std::endl: on some hosts a teardown hook crashes
         * the process after main() returns, which would swallow buffered
         * stdout and make CI logs undiagnosable. */
        auto devices = util.ListBurningDevices();
        std::cout << "burn-test: " << devices.size() << " device(s) detected"
                  << std::endl;
        for (size_t i = 0; i < devices.size(); ++i) {
            std::cout << "  [" << i << "] " << devices[i].devicePath
                      << " \"" << devices[i].deviceName << "\""
                      << " writable=" << devices[i].isWritable << std::endl;
        }
        if (devices.empty()) {
            std::cout << "SKIP: no burning device on this machine" << std::endl;
            return 0;
        }
        const DeviceInfo& dev = devices[0];
        if (!dev.isWritable) {
            std::cout << "SKIP: device " << dev.devicePath
                      << " is not writable (write-protected driver or busy)"
                      << std::endl;
            return 0;
        }

        /* 1. temp dir with a deterministic payload */
        std::string tmpBase = ".";
        {
            const char* t = std::getenv("TEMP");
            if (t && *t) tmpBase = t;
        }
        char dirName[512];
        snprintf(dirName, sizeof(dirName), "%s/burntest_%d",
                 tmpBase.c_str(), (int)GETPID());
        if (MKDIR(dirName) != 0) {
            std::cout << "FAIL: cannot create temp dir " << dirName
                      << std::endl;
            return 1;
        }
        std::string payloadPath = std::string(dirName) + "/payload.bin";
        {
            std::ofstream f(payloadPath.c_str(), std::ios::binary);
            if (!f) {
                std::cout << "FAIL: cannot write payload" << std::endl;
                RMDIR(dirName);
                return 1;
            }
            /* 2 MiB deterministic pattern */
            unsigned long seed = 0x9E3779B9ul;
            for (int block = 0; block < 64; ++block) {
                char buf[32768];
                for (size_t i = 0; i < sizeof(buf); i += 4) {
                    seed = seed * 1103515245ul + 12345ul;
                    buf[i + 0] = (char)(seed >> 24);
                    buf[i + 1] = (char)(seed >> 16);
                    buf[i + 2] = (char)(seed >> 8);
                    buf[i + 3] = (char)(seed);
                }
                f.write(buf, sizeof(buf));
            }
        }

        /* 2. build the ISO through our own API */
        std::string isoPath = std::string(dirName) + "/burntest.iso";
        std::cout << "burn-test: creating ISO from " << dirName << std::endl;
        if (!util.CreateISOImage(dirName, isoPath, "BURNTEST")) {
            std::cout << "FAIL: CreateISOImage: "
                      << util.GetLastErrorMessage() << std::endl;
            std::remove(payloadPath.c_str());
            RMDIR(dirName);
            return 1;
        }

        /* 3. async burn via the handle API */
        std::cout << "burn-test: burning " << isoPath << " -> "
                  << dev.devicePath << std::endl;
        BurnJobOptions options = makeOptions(BURN_JOB_MODE_IMAGE);
        options.sourcePath = isoPath.c_str();
        options.devicePath = dev.devicePath;
        options.discType = "DVD-R";
        options.speed = 4;
        int rc = runJob(options, false);

        /* 4. cleanup */
        std::remove(isoPath.c_str());
        std::remove(payloadPath.c_str());
        RMDIR(dirName);

        std::cout << (rc == 0 ? "burn-test: PASS" : "burn-test: FAIL")
                  << std::endl;
        return rc;
    }

    printUsage();
    return 1;
}
