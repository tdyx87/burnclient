#include "burning_utility.h"
#include <string>
#include <vector>
#include <memory>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>

extern "C" void SetBurnLastError(const std::string& error);
extern "C" void NotifyProgressBurn(int percent, const char* status);

#ifndef _WIN32

#include <libburn/libburn.h>
#include <libisofs/libisofs.h>

/* libburn and libisofs are pure C libraries — no namespace */

class UnixPlatform : public IPlatform {
public:
    UnixPlatform() {
        if (!burn_initialize()) {
            SetBurnLastError("Failed to initialize libburn");
        }
        iso_init();
    }
    ~UnixPlatform() override {
        iso_finish();
        burn_finish();
    }

    std::vector<DeviceInfo> ListDevices() override {
        std::vector<DeviceInfo> devices;

        struct burn_drive_info *drive_list = nullptr;
        unsigned int drive_count = 0;

        /* burn_drive_scan is async — wait for it to complete */
        while (burn_drive_scan(&drive_list, &drive_count) == 0) {
            usleep(100000); /* 100ms poll */
        }

        if (drive_count == 0) {
            SetBurnLastError("No drives found");
            /* Provide a sensible fallback so callers still get a device path */
            DeviceInfo info = {};
            info.isWritable = 1;
            info.isEmpty = 1;
            strncpy(info.mediaType, "CD/DVD", sizeof(info.mediaType) - 1);
            strncpy(info.devicePath, "/dev/sr0", sizeof(info.devicePath) - 1);
            strncpy(info.deviceName, "Default Optical Drive", sizeof(info.deviceName) - 1);
            devices.push_back(info);
            return devices;
        }

        for (unsigned int i = 0; i < drive_count; ++i) {
            DeviceInfo info = {};
            info.isWritable = 1;
            info.isEmpty = 1;
            strncpy(info.mediaType, "CD/DVD", sizeof(info.mediaType) - 1);
            snprintf(info.devicePath, sizeof(info.devicePath), "%s", drive_list[i].location);
            snprintf(info.deviceName, sizeof(info.deviceName), "Optical Drive %u", i);
            devices.push_back(info);
        }

        burn_drive_info_free(drive_list);
        return devices;
    }

    bool BurnISO(const std::string& isoPath, const std::string& devicePath,
                 const std::string& discType, int speed) override {
        NotifyProgressBurn(10, "Initializing burn process...");

        struct burn_drive *drive = nullptr;
        if (burn_drive_add_whitelist(devicePath.c_str()) <= 0) {
            SetBurnLastError("Failed to add device to whitelist");
            return false;
        }

        if (burn_drive_scan_and_grab(&drive, devicePath.c_str(), 0) <= 0) {
            SetBurnLastError("Failed to open device: " + devicePath);
            return false;
        }

        NotifyProgressBurn(30, "Preparing disc...");

        if (speed > 0) {
            burn_drive_set_speed(drive, speed * 1000, speed * 1000);
        }

        enum burn_disc_status status = burn_disc_get_status(drive);
        if (status != BURN_DISC_BLANK && status != BURN_DISC_APPENDABLE) {
            SetBurnLastError("Disc is not blank or appendable");
            burn_drive_release(drive, 0);
            return false;
        }

        NotifyProgressBurn(50, "Burning in progress...");

        struct burn_disc *disc = burn_disc_create();
        struct burn_session *session = burn_session_create();
        burn_disc_add_session(disc, session, BURN_POS_END);

        struct burn_source *src = burn_file_source_new(isoPath.c_str(), NULL);
        if (!src) {
            SetBurnLastError("Failed to create burn source from ISO");
            burn_session_free(session);
            burn_disc_free(disc);
            burn_drive_release(drive, 0);
            return false;
        }
        struct burn_track *track = burn_track_create();
        burn_track_set_source(track, src);
        burn_session_add_track(session, track, BURN_POS_END);

        struct burn_write_opts *opts = burn_write_opts_new(drive);
        burn_write_opts_set_perform_opc(opts, 0);
        burn_write_opts_auto_write_type(opts, disc, BURN_WRITE_SAO, 0);

        if (burn_disc_write(opts, disc) <= 0) {
            SetBurnLastError("Burn write failed");
            burn_write_opts_free(opts);
            burn_track_free(track);
            burn_session_free(session);
            burn_disc_free(disc);
            burn_drive_release(drive, 0);
            return false;
        }

        struct burn_progress progress;
        while (burn_drive_get_status(drive, &progress) == BURN_DRIVE_WRITING) {
            int percent = progress.sectors_total > 0
                ? (int)(progress.sectors_written * 100.0 / progress.sectors_total)
                : 50;
            if (percent > 50 && percent < 100) {
                char buf[128];
                snprintf(buf, sizeof(buf), "Burning... %d%%", percent);
                NotifyProgressBurn(percent, buf);
            }
            usleep(500000);
        }

        burn_write_opts_free(opts);
        burn_track_free(track);
        burn_session_free(session);
        burn_disc_free(disc);
        burn_drive_release(drive, 0);

        NotifyProgressBurn(100, "Burn completed");
        return true;
    }

    bool CreateISO(const std::string& sourcePath, const std::string& isoPath,
                   const std::string& volumeLabel) override {
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

        /* Single opts instance — no duplicate creation */
        IsoWriteOpts *opts = nullptr;
        iso_write_opts_new(&opts, 0);
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

        /* libisofs: iso_image_create_burn_source returns a burn_source via
           IsoBurnSource** which is actually struct burn_source** in libisofs.
           We cast accordingly. */
        struct burn_source *burn_src = nullptr;
        ret = iso_image_create_burn_source(image, opts, (IsoBurnSource**)&burn_src);
        if (ret < 0 || !burn_src) {
            SetBurnLastError("Failed to create burn source");
            iso_write_opts_free(opts);
            iso_image_unref(image);
            fclose(f);
            return false;
        }

        char buf[65536];
        off_t total = burn_src->size;
        off_t written = 0;
        while (written < total) {
            size_t to_read = (size_t)((total - written > (off_t)sizeof(buf))
                                      ? sizeof(buf) : total - written);
            int r = burn_src->read(burn_src, buf, to_read);
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

        if (burn_src->free) {
            burn_src->free(burn_src);
        }
        iso_write_opts_free(opts);
        fclose(f);
        iso_image_unref(image);

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

#endif /* _WIN32 */
