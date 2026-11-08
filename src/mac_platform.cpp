#include "burning_utility.h"
#include "burn_internal.h"
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * macOS backend
 *
 * libburn has NO Darwin backend (upstream libburn ships os-linux.h,
 * os-freebsd.h and os-solaris.h, but no os-darwin.h), and Homebrew has
 * no formula for libburn / libisoburn. CMake therefore links no third-party
 * libraries here.
 *
 * macOS ships its own tools for the same jobs:
 *   hdiutil makehybrid  -> build an ISO-9660/Joliet image from a directory
 *   drutil burn         -> burn an image to the optical drive
 *   drutil list         -> enumerate optical drives
 *
 * IMPORTANT: this backend never uses system(), popen() or a shell. Commands
 * are launched with posix_spawnp() from an argv array, so paths containing
 * quotes, spaces or shell metacharacters are passed through verbatim and
 * cancellation can signal the exact child process. Replacing these tools
 * with DiscRecording.framework is tracked as a separate task.
 * ------------------------------------------------------------------------- */
#ifdef __APPLE__

#include <unistd.h>
#include <signal.h>
#include <spawn.h>
#include <poll.h>
#include <errno.h>
#include <sys/wait.h>
#include <crt_externs.h>   /* _NSGetEnviron() */

namespace {

/* Tracks the child of the current native command so Burn_Cancel() can
 * interrupt it immediately via SIGTERM rather than waiting for pclose(). */
struct MacChildContext {
    std::atomic<pid_t> pid{0};
};

static void MacCancelHook(void* context) {
    MacChildContext* child = static_cast<MacChildContext*>(context);
    pid_t pid = child->pid.load();
    if (pid > 0) {
        kill(pid, SIGTERM);
    }
}

/* Run a command WITHOUT a shell. Returns the child's exit code, or -1 if it
 * could not be started / was cancelled / was killed by a signal.
 *
 * Using posix_spawnp() instead of popen() matters for three reasons:
 *   - no shell means no command injection through a path or volume label;
 *   - fork() is unsafe in a multithreaded process (this library uses worker
 *     threads), while posix_spawn is explicitly safe;
 *   - we get the child's pid, which makes cancellation possible.            */
static int RunCommand(const std::vector<std::string>& args,
                      std::string* output,
                      MacChildContext* child = nullptr) {
    if (args.empty()) {
        return -1;
    }

    int pipeFd[2];
    if (pipe(pipeFd) != 0) {
        return -1;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, pipeFd[0]);
    posix_spawn_file_actions_adddup2(&actions, pipeFd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipeFd[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipeFd[1]);

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid = 0;
    int spawnResult = posix_spawnp(&pid, args[0].c_str(), &actions, nullptr,
                                   argv.data(), *_NSGetEnviron());
    posix_spawn_file_actions_destroy(&actions);
    close(pipeFd[1]);

    if (spawnResult != 0) {
        close(pipeFd[0]);
        errno = spawnResult;
        return -1;
    }
    if (child) {
        child->pid.store(pid);
    }

    if (output) {
        output->clear();
    }

    bool cancelled = false;
    char buffer[1024];
    for (;;) {
        struct pollfd descriptor;
        descriptor.fd = pipeFd[0];
        descriptor.events = POLLIN;
        descriptor.revents = 0;

        int ready = poll(&descriptor, 1, 100);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) {
            if (IsBurnCancelled()) {
                cancelled = true;
                break;
            }
            continue;
        }
        if (descriptor.revents & (POLLIN | POLLHUP)) {
            ssize_t count = read(pipeFd[0], buffer, sizeof(buffer));
            if (count > 0) {
                if (output && output->size() < 65536) {
                    output->append(buffer, static_cast<size_t>(count));
                }
                continue;
            }
            if (count == 0) break;
            if (errno == EINTR) continue;
            break;
        }
        if (descriptor.revents & (POLLERR | POLLNVAL)) {
            break;
        }
    }
    close(pipeFd[0]);

    if (IsBurnCancelled()) {
        cancelled = true;
    }
    if (cancelled) {
        /* The cancel hook normally did this already; repeat it in case the
         * hook was not registered (e.g. ListDevices). */
        pid_t active = child ? child->pid.load() : static_cast<pid_t>(0);
        if (active > 0) kill(active, SIGTERM);
    }

    int status = 0;
    pid_t waited = -1;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);

    /* Clear before the pid can be reused by an unrelated process. */
    if (child) {
        child->pid.store(0);
    }

    if (cancelled || waited < 0) {
        return -1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

static std::string FailureDetail(const std::string& output, int rc) {
    if (!output.empty()) {
        std::string detail = output.substr(0, 256);
        while (!detail.empty() && (detail.back() == '\n' || detail.back() == '\r')) {
            detail.pop_back();
        }
        return detail;
    }
    return "exit code " + std::to_string(rc);
}

static DeviceInfo MakeDevice(const char* path, const char* name, const char* media,
                             int writable) {
    DeviceInfo info = {};
    info.isWritable = writable;
    /* Media state is not inquired during enumeration: drutil status/discinfo
     * would spin drives up as a side effect of a UI refresh. Assumed blank,
     * documented in the public header. */
    info.isEmpty = 1;
    strncpy(info.devicePath, path, sizeof(info.devicePath) - 1);
    strncpy(info.deviceName, name, sizeof(info.deviceName) - 1);
    strncpy(info.mediaType, media, sizeof(info.mediaType) - 1);
    return info;
}

/* Count the numbered device rows of `drutil list` output, e.g.
 *   Device: Revision:
 *   1:      01.0a
 * One "N:" line is one burning device. RunCommand merges stderr into the
 * pipe, so match the row shape instead of guessing from words like "No". */
static int CountDrutilRows(const std::string& output) {
    int rows = 0;
    size_t pos = 0;
    while (pos < output.size()) {
        size_t eol = output.find('\n', pos);
        if (eol == std::string::npos) eol = output.size();
        size_t s = pos;
        while (s < eol && (output[s] == ' ' || output[s] == '\t' || output[s] == '\r')) ++s;
        size_t d = s;
        while (d < eol && output[d] >= '0' && output[d] <= '9') ++d;
        if (d > s && d < eol && output[d] == ':') ++rows;
        pos = eol + 1;
    }
    return rows;
}

} /* namespace */

class MacPlatform : public IPlatform {
public:
    std::vector<DeviceInfo> ListDevices() override {
        std::vector<DeviceInfo> devices;
        std::string output;
        int rc = RunCommand({"drutil", "list"}, &output);

        if (rc == -1) {
            /* drutil could not be started at all (missing, sandboxed,
             * stripped PATH). That is NOT "no optical drive" - report the
             * truth and leave the list empty instead of inventing a
             * writable "default" device. */
            SetBurnLastError("drutil is not available on this system");
            return devices;
        }

        int rows = CountDrutilRows(output);
        if (rows <= 0) {
            /* drutil list enumerates BURNING devices only; zero rows means
             * no recorder (or none usable). Honest empty list. */
            SetBurnLastError("No burning devices reported by drutil");
            return devices;
        }

        for (int i = 1; i <= rows; ++i) {
            char idx[16];
            snprintf(idx, sizeof(idx), "%d", i);
            char name[64];
            snprintf(name, sizeof(name), "Optical drive #%d (drutil)", i);
            /* drutil's own man page defines `list` as the burning-device
             * list, so a listed device is a recorder: writable = 1. */
            devices.push_back(MakeDevice(idx, name, "CD/DVD", 1));
        }
        return devices;
    }

    bool BurnISO(const std::string& isoPath, const std::string& devicePath,
                 const std::string& discType, int speed) override {
        (void)discType; /* drutil picks the media type from the loaded disc;
                           it has no type-overwrite for `burn`. */

        if (IsBurnCancelled()) {
            SetBurnLastError("Burn cancelled by caller");
            return false;
        }
        NotifyProgressBurn(10, "Preparing to burn with drutil...");

        std::vector<std::string> args = { "drutil", "burn", "-noverify", "-eject" };
        /* drutil -speed takes a true multiplier (unlike IMAPI2/libburn,
         * which count sectors/s and k/s), so no unit conversion here. */
        if (speed > 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", speed);
            args.push_back("-speed");
            args.push_back(buf);
        }
        /* ListDevices reports the drutil row index; select that drive when
         * it is one. drutil only accepts digits, not paths, so plain digits
         * guard. Empty or non-numeric devicePath keeps drutil's default. */
        if (!devicePath.empty() &&
            devicePath.find_first_not_of("0123456789") == std::string::npos) {
            args.push_back("-drive");
            args.push_back(devicePath);
        }
        args.push_back(isoPath);

        MacChildContext child;
        SetBurnCancelHook(MacCancelHook, &child);

        std::string output;
        NotifyProgressBurn(50, "Burning in progress...");
        int rc = RunCommand(args, &output, &child);

        child.pid.store(0);
        ClearBurnCancelHook(MacCancelHook, &child);

        if (IsBurnCancelled()) {
            SetBurnLastError("Burn cancelled by caller");
            return false;
        }
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
        if (IsBurnCancelled()) {
            SetBurnLastError("ISO creation cancelled by caller");
            return false;
        }
        NotifyProgressBurn(10, "Creating ISO image with hdiutil...");

        /* -iso -joliet produces an ISO-9660 + Joliet image; the -o argument
         * must end in .iso, which GenerateTempISOPath() guarantees. */
        std::vector<std::string> args = {
            "hdiutil", "makehybrid", "-iso", "-joliet",
            "-default-volume-name", volumeLabel,
            "-o", isoPath, sourcePath
        };

        MacChildContext child;
        SetBurnCancelHook(MacCancelHook, &child);

        std::string output;
        NotifyProgressBurn(50, "Writing ISO image...");
        int rc = RunCommand(args, &output, &child);

        child.pid.store(0);
        ClearBurnCancelHook(MacCancelHook, &child);

        if (IsBurnCancelled()) {
            remove(isoPath.c_str());
            SetBurnLastError("ISO creation cancelled by caller");
            return false;
        }
        if (rc != 0) {
            /* hdiutil may leave a partial .iso behind; never hand a
             * truncated image to the caller as "failed but usable". */
            remove(isoPath.c_str());
            SetBurnLastError("hdiutil makehybrid failed: " + FailureDetail(output, rc));
            return false;
        }

        NotifyProgressBurn(100, "ISO creation completed");
        return true;
    }

    std::string GetPlatformInfo() override {
        return "macOS (hdiutil / drutil, native spawn, no shell)";
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
