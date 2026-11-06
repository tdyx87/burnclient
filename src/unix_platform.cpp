#include "burning_utility.h"
#include <string>
#include <vector>
#include <memory>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <sys/stat.h>

/* Internal C++ helpers — NOT extern "C" (they take std::string) */
void SetBurnLastError(const std::string& error);
void NotifyProgressBurn(int percent, const char* status);

#ifndef _WIN32

#include <libburn/libburn.h>
#include <libisofs/libisofs.h>

/* libburn and libisofs are pure C libraries — no namespace.
 *
 * Key API signatures (verified from libburn/libisofs headers):
 *
 * libburn:
 *   int  burn_initialize(void);
 *   void burn_finish(void);
 *   int  burn_drive_scan(struct burn_drive_info *drive_infos[], unsigned int *n_drives);
 *        returns: 0=still scanning, >0=done, <0=error
 *   int  burn_drive_scan_and_grab(struct burn_drive_info *drive_infos[], char *adr, int load);
 *   int  burn_drive_add_whitelist(char *device_address);
 *   void burn_drive_info_free(struct burn_drive_info *drive_infos);
 *   void burn_drive_release(struct burn_drive *drive, int eject);
 *   void burn_drive_set_speed(struct burn_drive *drive, int read_speed, int write_speed);
 *   enum burn_disc_status burn_disc_get_status(struct burn_drive *drive);
 *
 * struct burn_source:
 *   int   (*read)(struct burn_source *, unsigned char *buffer, int size);
 *   off_t (*get_size)(struct burn_source *);
 *   void  (*free_data)(struct burn_source *);
 *   void  burn_source_free(struct burn_source *src);
 *
 * libisofs:
 *   int  iso_init(void);
 *   void iso_finish(void);
 *   int  iso_image_new(const char *name, IsoImage **image);
 *   void iso_image_unref(IsoImage *image);
 *   int  iso_image_create_burn_source(IsoImage *image, IsoWriteOpts *opts,
 *                                     struct burn_source **burn_src);
 */

class UnixPlatform : public IPlatform {
public:
    UnixPlatform() {
        if (!burn_initialize()) {
            SetBurnLastError("Failed to initialize libburn");
        }
        if (iso_init() < 0) {
            SetBurnLastError("Failed to initialize libisofs");
        }
    }

    ~UnixPlatform() override {
        iso_finish();
        burn_finish();
    }

    std::vector<DeviceInfo> ListDevices() override {
        std::vector<DeviceInfo> devices;

        struct burn_drive_info *drive_list = nullptr;
        unsigned int drive_count = 0;

        /* burn_drive_scan is asynchronous — must poll until it returns nonzero.
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
        NotifyProgressBurn(10, "Initializing burn process...");

        /* burn_drive_scan_and_grab takes struct burn_drive_info *drive_infos[],
         * which is a pointer-to-pointer: struct burn_drive_info** */
        struct burn_drive_info *drive_info = nullptr;

        /* Add to whitelist first (optional but recommended) */
        burn_drive_add_whitelist(devicePath.c_str());

        if (burn_drive_scan_and_grab(&drive_info, const_cast<char*>(devicePath.c_str()), 0) <= 0) {
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

        struct burn_source *src = burn_file_source_new(isoPath.c_str(), NULL);
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

        /* burn_disc_write returns void — it starts the async write.
         * Poll burn_drive_get_status until the drive goes IDLE.
         * The drive may pass through WRITING, WRITING_LEADIN,
         * WRITING_LEADOUT, WRITING_PREGAP, CLOSING_TRACK, CLOSING_SESSION. */
        struct burn_progress progress;
        enum burn_drive_status drive_status;
        while ((drive_status = burn_drive_get_status(drive, &progress)) != BURN_DRIVE_IDLE) {
            if (drive_status == BURN_DRIVE_WRITING ||
                drive_status == BURN_DRIVE_WRITING_LEADIN ||
                drive_status == BURN_DRIVE_WRITING_LEADOUT) {
                /* struct burn_progress uses .sectors (total) and .sector (current) */
                int percent = progress.sectors > 0
                    ? (int)(progress.sector * 100.0 / progress.sectors)
                    : 50;
                if (percent > 50 && percent < 100) {
                    char buf[128];
                    snprintf(buf, sizeof(buf), "Burning... %d%%", percent);
                    NotifyProgressBurn(percent, buf);
                }
            }
            usleep(500000);
        }

        /* After the drive goes IDLE, check if the burn succeeded by
         * looking at the disc status — if it's FULL, the burn worked. */
        enum burn_disc_status post_status = burn_disc_get_status(drive);
        bool burn_ok = (post_status == BURN_DISC_FULL || post_status == BURN_DISC_APPENDABLE);

        burn_write_opts_free(opts);
        burn_track_free(track);
        burn_session_free(session);
        burn_disc_free(disc);
        burn_drive_release(drive, 0);
        burn_drive_info_free(drive_info);

        if (!burn_ok) {
            SetBurnLastError("Burn failed — disc is not readable after write");
            NotifyProgressBurn(100, "Burn failed");
            return false;
        }

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

        /* iso_image_create_burn_source returns a struct burn_source*.
         * The third parameter is struct burn_source** (NOT IsoBurnSource**).
         * IsoBurnSource does not exist in libisofs — it is a libisoburn type. */
        struct burn_source *burn_src = nullptr;
        ret = iso_image_create_burn_source(image, opts, &burn_src);
        if (ret < 0 || !burn_src) {
            SetBurnLastError("Failed to create burn source");
            iso_write_opts_free(opts);
            iso_image_unref(image);
            fclose(f);
            return false;
        }

        /* struct burn_source fields:
         *   int   (*read)(struct burn_source *, unsigned char *buffer, int size);
         *   off_t (*get_size)(struct burn_source *);
         *   void  (*free_data)(struct burn_source *);
         * There is NO .size field — must call get_size().
         * There is NO .free field — must call burn_source_free(). */
        off_t total = 0;
        if (burn_src->get_size) {
            total = burn_src->get_size(burn_src);
        }

        char buf[65536];
        off_t written = 0;
        while (written < total) {
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
