#include "burning_utility.h"
#include "burn_internal.h"
#include <string>
#include <vector>
#include <memory>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <stdint.h>   /* libisofs.h needs uint32_t/uint8_t in global scope */
/* NOTE: POSIX-only headers must stay inside the platform guard below -
 * this file is also compiled on Windows, where <unistd.h> does not exist. */

/* libburn only supports Linux/FreeBSD/Solaris (no Darwin backend).
 * macOS uses the native hdiutil/drutil backend in mac_platform.cpp. */
#if !defined(_WIN32) && !defined(__APPLE__)

#include <unistd.h>
#include <sys/stat.h>
#include <libburn/libburn.h>
#include <libisofs/libisofs.h>

/* IMPORTANT (this was the root cause of the Linux build failures):
 *
 * libburn.h wraps its ENTIRE API in
 *     namespace burn { extern "C" { ... } }     (BURN_BEGIN_DECLS)
 * when included from C++. libisofs.h instead uses a plain
 *     extern "C" { ... }
 * and stays in the global namespace, where it also forward-declares
 * "struct burn_source;".
 *
 * Consequences for C++ consumers:
 *  1. Every libburn function/enum/struct must be visible through
 *     "using namespace burn;".
 *  2. An unqualified "struct burn_source" is AMBIGUOUS between
 *     burn::burn_source (full definition) and ::burn_source
 *     (libisofs forward declaration). Always qualify it explicitly.
 *  3. iso_image_create_burn_source() takes ::burn_source**, while
 *     burn_file_source_new()/burn_track_set_source()/burn_source_free()
 *     use burn::burn_source*. Both describe the same C layout, so a
 *     reinterpret_cast is used at that one boundary.
 */
/* Key API signatures (verified against upstream libburn.h / libisofs.h):
 *
 *   int  burn_drive_scan(struct burn_drive_info *[], unsigned int *);
 *   int  burn_drive_scan_and_grab(struct burn_drive_info *[], char *adr, int load);
 *   int  burn_drive_add_whitelist(char *device_address);   <-- NOT const
 *   void burn_disc_write(struct burn_write_opts *, struct burn_disc *);
 *   enum burn_write_types burn_write_opts_auto_write_type(..., char reasons[], int);
 *   int  iso_image_create_burn_source(IsoImage *, IsoWriteOpts *,
 *                                     struct burn_source **);
 *
 * struct burn_source has .read/.get_size/.free_data/.version/.read_xt/.cancel
 * struct burn_progress has .sectors (total) and .sector (current).
 */

using namespace burn;

/* Native cancellation hook: burn_drive_cancel() requests an abort of the
 * asynchronous burn_disc_write(). The polling loop below keeps waiting until
 * the drive returns to IDLE before any libburn object is freed. */
struct UnixCancelContext {
    std::mutex mutex;
    struct burn_drive* drive = nullptr;
};

static void UnixCancelHook(void* context) {
    UnixCancelContext* ctx = static_cast<UnixCancelContext*>(context);
    std::lock_guard<std::mutex> lock(ctx->mutex);
    if (ctx->drive) {
        burn_drive_cancel(ctx->drive);
    }
}

namespace {

DeviceInfo MakeFallbackDevice() {
    DeviceInfo info = {};
    info.isWritable = 1;
    info.isEmpty = 1;
    strncpy(info.mediaType, "CD/DVD", sizeof(info.mediaType) - 1);
    strncpy(info.devicePath, "/dev/sr0", sizeof(info.devicePath) - 1);
    strncpy(info.deviceName, "Default Optical Drive", sizeof(info.deviceName) - 1);
    return info;
}

} /* namespace */

class UnixPlatform : public IPlatform {
public:
    UnixPlatform() {
        /* burn_initialize() returns 1 on success; iso_init() returns >= 0.
         * CI machines / containers usually have no optical drives and may
         * not permit libburn to initialize - never call into the library
         * when it failed to start. */
        burnReady_ = (burn_initialize() != 0);
        if (!burnReady_) {
            SetBurnLastError("Failed to initialize libburn");
        }
        isoReady_ = (iso_init() >= 0);
        if (!isoReady_) {
            SetBurnLastError("Failed to initialize libisofs");
        }
    }

    ~UnixPlatform() override {
        if (isoReady_) {
            iso_finish();
        }
        if (burnReady_) {
            burn_finish();
        }
    }

private:
    bool burnReady_ = false;
    bool isoReady_ = false;

public:
    std::vector<DeviceInfo> ListDevices() override {
        std::vector<DeviceInfo> devices;

        if (!burnReady_) {
            devices.push_back(MakeFallbackDevice());
            return devices;
        }

        struct burn_drive_info *drive_list = nullptr;
        unsigned int drive_count = 0;

        /* burn_drive_scan is asynchronous - must poll until it returns nonzero.
         * 0 = still scanning, >0 = done, <0 = error */
        int scan_ret;
        while ((scan_ret = burn_drive_scan(&drive_list, &drive_count)) == 0) {
            usleep(100000); /* 100ms poll */
        }

        if (scan_ret < 0 || drive_count == 0) {
            SetBurnLastError("No drives found");
            if (drive_list) {
                burn_drive_info_free(drive_list);
            }
            /* Provide a sensible fallback so callers still get a device path */
            devices.push_back(MakeFallbackDevice());
            return devices;
        }

        for (unsigned int i = 0; i < drive_count; ++i) {
            DeviceInfo info = {};
            info.isWritable = 1;
            info.isEmpty = 1;
            strncpy(info.mediaType, "CD/DVD", sizeof(info.mediaType) - 1);

            /* Use burn_drive_d_get_adr() instead of deprecated .location field */
            char adr[BURN_DRIVE_ADR_LEN] = {0};
            if (burn_drive_d_get_adr(drive_list[i].drive, adr) > 0 && adr[0]) {
                snprintf(info.devicePath, sizeof(info.devicePath), "%s", adr);
            } else {
                /* Fallback to deprecated location field */
                snprintf(info.devicePath, sizeof(info.devicePath), "%s", drive_list[i].location);
            }

            snprintf(info.deviceName, sizeof(info.deviceName), "%.16s %.8s",
                     drive_list[i].product, drive_list[i].vendor);
            devices.push_back(info);
        }

        burn_drive_info_free(drive_list);
        return devices;
    }

    bool BurnISO(const std::string& isoPath, const std::string& devicePath,
                 const std::string& discType, int speed) override {
        if (!burnReady_) {
            SetBurnLastError("libburn is not initialized");
            return false;
        }
        if (IsBurnCancelled()) {
            SetBurnLastError("Burn cancelled by caller");
            return false;
        }
        NotifyProgressBurn(10, "Initializing burn process...");

        /* burn_drive_scan_and_grab takes struct burn_drive_info *drive_infos[],
         * which is a pointer-to-pointer: struct burn_drive_info** */
        struct burn_drive_info *drive_info = nullptr;

        /* burn_drive_scan_and_grab() takes char* (NOT const char*), so keep
         * a mutable NUL-terminated copy. Note: it calls
         * burn_drive_clear_whitelist() + burn_drive_add_whitelist(adr)
         * internally, so calling burn_drive_add_whitelist() here is
         * redundant - that is why the whitelist call was removed. */
        std::vector<char> deviceAdr(devicePath.begin(), devicePath.end());
        deviceAdr.push_back('\0');

        if (burn_drive_scan_and_grab(&drive_info, deviceAdr.data(), 0) <= 0) {
            SetBurnLastError("Failed to open device: " + devicePath);
            return false;
        }

        /* After scan_and_grab, the drive is accessed via drive_info[0].drive */
        struct burn_drive *drive = drive_info[0].drive;

        NotifyProgressBurn(30, "Preparing disc...");

        if (speed > 0) {
            burn_drive_set_speed(drive, speed * 1000, speed * 1000);
        }

        enum burn_disc_status status = burn_disc_get_status(drive);
        if (status != BURN_DISC_BLANK && status != BURN_DISC_APPENDABLE) {
            SetBurnLastError("Disc is not blank or appendable");
            burn_drive_release(drive, 0);
            burn_drive_info_free(drive_info);
            return false;
        }

        NotifyProgressBurn(50, "Burning in progress...");

        struct burn_disc *disc = burn_disc_create();
        struct burn_session *session = burn_session_create();
        burn_disc_add_session(disc, session, BURN_POS_END);

        /* Qualified: unqualified burn_source is ambiguous in C++ */
        burn::burn_source *src = burn_file_source_new(isoPath.c_str(), NULL);
        if (!src) {
            SetBurnLastError("Failed to create burn source from ISO");
            burn_session_free(session);
            burn_disc_free(disc);
            burn_drive_release(drive, 0);
            burn_drive_info_free(drive_info);
            return false;
        }
        struct burn_track *track = burn_track_create();
        burn_track_set_source(track, src);
        burn_session_add_track(session, track, BURN_POS_END);

        struct burn_write_opts *opts = burn_write_opts_new(drive);
        burn_write_opts_set_perform_opc(opts, 0);

        /* burn_write_opts_auto_write_type returns enum burn_write_types.
         * Third parameter is a char* buffer for rejection reasons (BURN_REASONS_LEN=4096).
         * Returns BURN_WRITE_NONE on failure. */
        char reasons[4096] = {0};
        enum burn_write_types write_type;
        write_type = burn_write_opts_auto_write_type(opts, disc, reasons, 0);
        if (write_type == BURN_WRITE_NONE) {
            SetBurnLastError("Failed to determine write type");
            burn_write_opts_free(opts);
            burn_track_free(track);
            burn_session_free(session);
            burn_disc_free(disc);
            burn_drive_release(drive, 0);
            burn_drive_info_free(drive_info);
            return false;
        }

        /* burn_write_opts_auto_write_type() chooses AND applies the write
         * type; upstream requires it immediately before burn_disc_write().
         * This call was missing entirely in an earlier revision - the old
         * code polled a drive that was never told to write, so Linux burns
         * were a silent no-op. */
        UnixCancelContext cancelCtx;
        {
            std::lock_guard<std::mutex> lock(cancelCtx.mutex);
            cancelCtx.drive = drive;
        }
        SetBurnCancelHook(UnixCancelHook, &cancelCtx);

        bool cancelled = IsBurnCancelled();
        if (!cancelled) {
            /* burn_disc_write() returns void and starts the asynchronous
             * write. Progress is polled through burn_drive_get_status(). */
            burn_disc_write(opts, disc);

            struct burn_progress progress;
            enum burn_drive_status drive_status;
            while ((drive_status = burn_drive_get_status(drive, &progress)) != BURN_DRIVE_IDLE) {
                if (IsBurnCancelled() && !cancelled) {
                    cancelled = true;
                    burn_drive_cancel(drive);
                    NotifyProgressBurn(0, "Cancelling burn...");
                }
                if (!cancelled &&
                    (drive_status == BURN_DRIVE_WRITING ||
                     drive_status == BURN_DRIVE_WRITING_LEADIN ||
                     drive_status == BURN_DRIVE_WRITING_LEADOUT ||
                     drive_status == BURN_DRIVE_WRITING_PREGAP ||
                     drive_status == BURN_DRIVE_CLOSING_TRACK ||
                     drive_status == BURN_DRIVE_CLOSING_SESSION)) {
                    /* struct burn_progress uses .sectors (total) and .sector
                     * (current). Report the real 0..99% instead of faking 50%. */
                    int percent = progress.sectors > 0
                        ? (int)(progress.sector * 100.0 / progress.sectors)
                        : 0;
                    if (percent < 0) percent = 0;
                    if (percent > 99) percent = 99;
                    char buf[128];
                    snprintf(buf, sizeof(buf), "Burning... %d%%", percent);
                    NotifyProgressBurn(percent, buf);
                }
                usleep(100000);
            }
        }

        /* Neutralise the hook before clearing it, so a late Burn_Cancel()
         * cannot call burn_drive_cancel() on a released drive. */
        {
            std::lock_guard<std::mutex> lock(cancelCtx.mutex);
            cancelCtx.drive = nullptr;
        }
        ClearBurnCancelHook(UnixCancelHook, &cancelCtx);

        /* burn_drive_wrote_well() is the authoritative result of the most
         * recent asynchronous burn_disc_write() (1 = ok, 0 = failed). */
        bool burn_ok = !cancelled && (burn_drive_wrote_well(drive) > 0);

        burn_write_opts_free(opts);
        burn_track_free(track);
        burn_session_free(session);
        burn_disc_free(disc);
        burn_drive_release(drive, 0);
        burn_drive_info_free(drive_info);

        if (!burn_ok) {
            if (cancelled) {
                SetBurnLastError("Burn cancelled by caller");
                NotifyProgressBurn(0, "Burn cancelled");
            } else {
                SetBurnLastError("Burn failed - disc is not readable after write");
                NotifyProgressBurn(100, "Burn failed");
            }
            return false;
        }

        NotifyProgressBurn(100, "Burn completed");
        return true;
    }

    bool CreateISO(const std::string& sourcePath, const std::string& isoPath,
                   const std::string& volumeLabel) override {
        if (!isoReady_) {
            SetBurnLastError("libisofs is not initialized");
            return false;
        }
        if (IsBurnCancelled()) {
            SetBurnLastError("ISO creation cancelled by caller");
            return false;
        }
        NotifyProgressBurn(10, "Creating ISO image...");

        IsoImage *image = nullptr;
        int ret = iso_image_new(volumeLabel.c_str(), &image);
        if (ret < 0) {
            SetBurnLastError("Failed to create ISO image");
            return false;
        }

        iso_image_set_volume_id(image, volumeLabel.c_str());

        IsoDir *root = iso_image_get_root(image);
        ret = iso_tree_add_dir_rec(image, root, sourcePath.c_str());
        if (ret < 0) {
            SetBurnLastError("Failed to add source directory to ISO");
            iso_image_unref(image);
            return false;
        }

        NotifyProgressBurn(50, "Writing ISO file...");

        /* Single opts instance */
        IsoWriteOpts *opts = nullptr;
        ret = iso_write_opts_new(&opts, 0);
        if (ret < 0 || !opts) {
            SetBurnLastError("Failed to create write options");
            iso_image_unref(image);
            return false;
        }
        iso_write_opts_set_iso_level(opts, 3);
        iso_write_opts_set_rockridge(opts, 1);
        iso_write_opts_set_joliet(opts, 1);

        FILE *f = fopen(isoPath.c_str(), "wb");
        if (!f) {
            SetBurnLastError("Cannot open output file: " + isoPath);
            iso_write_opts_free(opts);
            iso_image_unref(image);
            return false;
        }

        /* iso_image_create_burn_source() is declared in the global namespace
         * and takes ::burn_source** (libisofs.h forward declaration).
         * libburn's full struct is burn::burn_source. Both describe the
         * exact same C layout, so cast once at this boundary. */
        ::burn_source *iso_src = nullptr;
        ret = iso_image_create_burn_source(image, opts, &iso_src);
        if (ret < 0 || !iso_src) {
            SetBurnLastError("Failed to create burn source");
            iso_write_opts_free(opts);
            iso_image_unref(image);
            fclose(f);
            return false;
        }

        burn::burn_source *burn_src =
            reinterpret_cast<burn::burn_source *>(iso_src);

        /* burn_source fields:
         *   int   (*read)(struct burn_source *, unsigned char *buffer, int size);
         *   off_t (*get_size)(struct burn_source *);
         *   void  (*free_data)(struct burn_source *);
         *   int   version; int (*read_xt)(...); int (*cancel)(...);
         * There is NO .size field - must call get_size().
         * There is NO .free field - must call burn_source_free(). */
        off_t total = 0;
        if (burn_src->get_size) {
            total = burn_src->get_size(burn_src);
        }

        char buf[65536];
        off_t written = 0;
        bool cancelled = false;
        while (written < total) {
            if (IsBurnCancelled()) {
                cancelled = true;
                break;
            }
            int to_read = (int)((total - written > (off_t)sizeof(buf))
                                ? sizeof(buf) : total - written);
            int r = -1;
            if (burn_src->read) {
                r = burn_src->read(burn_src, (unsigned char*)buf, to_read);
            } else if (burn_src->version >= 1 && burn_src->read_xt) {
                r = burn_src->read_xt(burn_src, (unsigned char*)buf, to_read);
            }
            if (r <= 0) break;
            fwrite(buf, 1, (size_t)r, f);
            written += r;
            int pct = total > 0 ? (int)(written * 100 / total) : 100;
            if (pct > 50 && pct < 100) {
                char msg[64];
                snprintf(msg, sizeof(msg), "Writing... %d%%", pct);
                NotifyProgressBurn(pct, msg);
            }
        }

        /* Use burn_source_free() which handles refcounting and calls free_data */
        burn_source_free(burn_src);
        iso_write_opts_free(opts);
        fclose(f);
        iso_image_unref(image);

        if (cancelled) {
            remove(isoPath.c_str());
            SetBurnLastError("ISO creation cancelled by caller");
            return false;
        }
        if (written < total) {
            remove(isoPath.c_str());
            SetBurnLastError("ISO image was truncated while writing");
            return false;
        }

        NotifyProgressBurn(100, "ISO creation completed");
        return true;
    }

    std::string GetPlatformInfo() override {
        return "Linux/Unix (libburn/libisofs)";
    }

    bool CheckToolAvailability(const std::string& toolName) override {
        /* Check common bin directories for an executable */
        const char* dirs[] = {
            "/usr/bin/", "/usr/local/bin/", "/opt/local/bin/", "/bin/", nullptr
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
    return std::make_unique<UnixPlatform>();
}

#endif /* !_WIN32 && !__APPLE__ */
