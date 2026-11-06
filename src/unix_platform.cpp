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

        if (burn_drive_scan(&drive_list, &drive_count) <= 0) {
            SetBurnLastError("No drives found");
            DeviceInfo info = {};
            info.isWritable = 1;
            info.isEmpty = 1;
            strcpy(info.mediaType, "CD/DVD");
            strcpy(info.devicePath, "/dev/sr0");
            strcpy(info.deviceName, "Default Optical Drive");
            devices.push_back(info);
            return devices;
        }

        for (unsigned int i = 0; i < drive_count; ++i) {
            DeviceInfo info = {};
            info.isWritable = 1;
            info.isEmpty = 1;
            strcpy(info.mediaType, "CD/DVD");
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

        struct burn_track *track = burn_track_create();
        burn_track_set_source_iso(track, (char*)isoPath.c_str(), (off_t)0);
        burn_session_add_track(session, track, BURN_POS_END);

        struct burn_write_opts *opts = burn_write_opts_new(drive);
        burn_write_opts_set_perform_opc(opts, 0);
        burn_write_opts_auto_write_type(opts, disc, BURN_WRITE_SAO, 0);

        if (burn_disc_write(opts, disc) <= 0) {
            SetBurnLastError("Burn write failed");
            burn_write_opts_free(opts);
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

        IsoNode *root = iso_image_get_root(image);
        ret = iso_tree_add_dir_rec(image, root, sourcePath.c_str());
        if (ret < 0) {
            SetBurnLastError("Failed to add source directory to ISO");
            iso_image_unref(image);
            return false;
        }

        NotifyProgressBurn(50, "Writing ISO file...");

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

        IsoDataSource *src = nullptr;
        iso_image_create_data_source(image, &src);
        if (src) {
            int64_t total = iso_image_get_size(image);
            int64_t written = 0;
            char buf[65536];
            while (written < total) {
                int64_t to_read = (total - written > (int64_t)sizeof(buf)) ? (int64_t)sizeof(buf) : (total - written);
                (void)to_read;
                int64_t r = src->read_block(src, (uint32_t)(written / 2048), buf);
                if (r < 0) break;
                fwrite(buf, 1, 2048, f);
                written += 2048;
                int pct = total > 0 ? (int)(written * 100 / total) : 100;
                if (pct > 50 && pct < 100) {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "Writing... %d%%", pct);
                    NotifyProgressBurn(pct, msg);
                }
            }
            src->free_data(src);
        }

        fclose(f);
        iso_write_opts_free(opts);
        iso_image_unref(image);

        NotifyProgressBurn(100, "ISO creation completed");
        return true;
    }

    std::string GetPlatformInfo() override {
        return "Linux/Unix (libburn/libisofs)";
    }

    bool CheckToolAvailability(const std::string& toolName) override {
        return true;
    }
};

std::unique_ptr<IPlatform> CreatePlatformImpl() {
    return std::make_unique<UnixPlatform>();
}

#endif
