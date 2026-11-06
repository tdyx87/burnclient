#include "burning_utility.h"
#include <string>
#include <vector>
#include <memory>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * macOS backend
 *
 * libburn has NO Darwin backend (upstream libburn ships os-linux.h,
 * os-freebsd.h and os-solaris.h, but no os-darwin.h), and Homebrew has
 * no formula for libburn / libisoburn. Requiring them in CMake/CI was
 * therefore impossible to satisfy.
 *
 * macOS ships its own command line tools for the same jobs:
 *   hdiutil makehybrid  -> build an ISO-9660/Joliet image from a directory
 *   drutil burn         -> burn an image to the optical drive
 *   drutil list         -> enumerate optical drives
 *
 * This backend uses only those tools plus the C runtime, so the library
 * builds on macOS with no third-party dependencies.
 * ------------------------------------------------------------------------- */
#ifdef __APPLE__

#include <unistd.h>
#include <sys/wait.h>

/* Internal C++ helpers — NOT extern "C" (they take std::string) */
void SetBurnLastError(const std::string& error);
void NotifyProgressBurn(int percent, const char* status);

namespace {

/* Single-quote a path so it survives shell interpretation. */
std::string ShellQuote(const std::string& value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

/* Run a command, optionally capturing stdout+stderr.
 * Returns the process exit code, or -1 if it could not be started. */
int RunCommand(const std::string& command, std::string* output = nullptr) {
    std::string full = command + " 2>&1";
    FILE* pipe = popen(full.c_str(), "r");
    if (!pipe) {
        return -1;
    }

    if (output) {
        output->clear();
    }
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        if (output) {
            output->append(buffer);
        }
    }

    int status = pclose(pipe);
    if (status < 0) {
        return -1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

std::string FailureDetail(const std::string& output, int rc) {
    if (!output.empty()) {
        /* Keep the message short; full output can be very large. */
        std::string detail = output.substr(0, 256);
        while (!detail.empty() && (detail.back() == '\n' || detail.back() == '\r')) {
            detail.pop_back();
        }
        return detail;
    }
    return "exit code " + std::to_string(rc);
}

DeviceInfo MakeDevice(const char* path, const char* name, const char* media) {
    DeviceInfo info = {};
    info.isWritable = 1;
    info.isEmpty = 1;
    strncpy(info.devicePath, path, sizeof(info.devicePath) - 1);
    strncpy(info.deviceName, name, sizeof(info.deviceName) - 1);
    strncpy(info.mediaType, media, sizeof(info.mediaType) - 1);
    return info;
}

} /* namespace */

class MacPlatform : public IPlatform {
public:
    std::vector<DeviceInfo> ListDevices() override {
        std::vector<DeviceInfo> devices;
        std::string output;
        int rc = RunCommand("drutil list", &output);

        /* drutil writes a table to stdout. Rather than depend on its exact
         * column layout (which differs between macOS releases), expose the
         * default burner — drutil burn targets it when no -drive is given. */
        bool hasBurner = (rc == 0 &&
                          output.find("No") == std::string::npos &&
                          !output.empty());
        if (hasBurner) {
            devices.push_back(MakeDevice("default", "Default Optical Drive (drutil)", "CD/DVD"));
        } else {
            devices.push_back(MakeDevice("default", "No optical drive detected", "CD/DVD"));
        }
        return devices;
    }

    bool BurnISO(const std::string& isoPath, const std::string& devicePath,
                 const std::string& discType, int speed) override {
        (void)devicePath;
        (void)discType;
        (void)speed;

        NotifyProgressBurn(10, "Preparing to burn with drutil...");

        std::string output;
        std::string command = "drutil burn -noverify -eject " + ShellQuote(isoPath);

        NotifyProgressBurn(50, "Burning in progress...");
        int rc = RunCommand(command, &output);
        if (rc != 0) {
            SetBurnLastError("drutil burn failed: " + FailureDetail(output, rc));
            NotifyProgressBurn(100, "Burn failed");
            return false;
        }

        NotifyProgressBurn(100, "Burn completed");
        return true;
    }

    bool CreateISO(const std::string& sourcePath, const std::string& isoPath,
                   const std::string& volumeLabel) override {
        NotifyProgressBurn(10, "Creating ISO image with hdiutil...");

        /* -iso -joliet produces an ISO-9660 + Joliet image; the -o argument
         * must end in .iso, which GenerateTempISOPath() guarantees. */
        std::string command = "hdiutil makehybrid -iso -joliet -default-volume-name " +
                              ShellQuote(volumeLabel) + " -o " + ShellQuote(isoPath) +
                              " " + ShellQuote(sourcePath);

        std::string output;
        NotifyProgressBurn(50, "Writing ISO image...");
        int rc = RunCommand(command, &output);
        if (rc != 0) {
            SetBurnLastError("hdiutil makehybrid failed: " + FailureDetail(output, rc));
            return false;
        }

        NotifyProgressBurn(100, "ISO creation completed");
        return true;
    }

    std::string GetPlatformInfo() override {
        return "macOS (hdiutil / drutil)";
    }

    bool CheckToolAvailability(const std::string& toolName) override {
        const char* dirs[] = {
            "/usr/bin/", "/usr/sbin/", "/sbin/", "/bin/", "/usr/local/bin/", nullptr
        };
        for (int i = 0; dirs[i]; ++i) {
            std::string full = std::string(dirs[i]) + toolName;
            if (access(full.c_str(), X_OK) == 0) {
                return true;
            }
        }
        return false;
    }
};

std::unique_ptr<IPlatform> CreatePlatformImpl() {
    return std::make_unique<MacPlatform>();
}

#endif /* __APPLE__ */
