#include "burning_utility.h"
#include "burn_internal.h"
#include "unix_features.h"
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

/* MMC profile numbers of recordable media (mmc5r03c.pdf table 89, mirrored
 * by the burn_disc_get_profile() docs of libburn 1.5.2). */
enum MmcProfile {
    PROFILE_CD_ROM              = 0x08,
    PROFILE_CD_R                = 0x09,
    PROFILE_CD_RW               = 0x0A,
    PROFILE_DVD_ROM             = 0x10,
    PROFILE_DVD_R_SEQ           = 0x11,
    PROFILE_DVD_RAM             = 0x12,
    PROFILE_DVD_RW_RO           = 0x13,
    PROFILE_DVD_R_DLT           = 0x14,
    PROFILE_DVD_PLUS_RW         = 0x1A,
    PROFILE_DVD_PLUS_R          = 0x1B,
    PROFILE_DVD_PLUS_R_DL       = 0x2B,
    PROFILE_BD_ROM              = 0x40,
    PROFILE_BD_R_SEQ            = 0x41,
    PROFILE_BD_RE               = 0x43,
    PROFILE_LIBBURN_STDIO       = 0xFFFF  /* not a MMC profile; file device */
};

/* Write-speed unit for one speed multiplier of the given media family,
 * in k/s (libburn counts "k" as 1000). Values from the libburn 1.5.2
 * burn_drive_set_speed() header doc: 1xCD = 176.4 k/s (round up -> 177),
 * 1xDVD = 1385 k/s, 1xBD = 4496 k/s. */
static int KsPerOneX(char family) {
    switch (family) {
    case 'C': return 177;
    case 'D': return 1385;
    case 'B': return 4496;
    default:  return 1385;
    }
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
            /* No fabricated /dev/sr0: when libburn cannot start there is
             * provably nothing to report, and inventing a writable device
             * makes integration tests burn air instead of skipping. */
            SetBurnLastError("Failed to initialize libburn");
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

        if (scan_ret < 0) {
            SetBurnLastError("Drive scan failed");
            if (drive_list) {
                burn_drive_info_free(drive_list);
            }
            return devices;
        }
        if (drive_count == 0) {
            SetBurnLastError("No drives found");
            if (drive_list) {
                burn_drive_info_free(drive_list);
            }
            return devices;
        }

        for (unsigned int i = 0; i < drive_count; ++i) {
            DeviceInfo info = {};

            /* Honest writability from the capability bits burn_drive_scan()
             * filled during enumeration. This needs no grab (which would
             * spin drives up and (O_EXCL) lock them out of other programs
             * during a UI refresh). Documented blind spot of the bitfields:
             * drives that ONLY write DVD-RW/DVD+R/DVD+RW/BD media do not set
             * any of these bits and are reported read-only here; their true
             * capability list is only available after a grab via
             * burn_drive_get_all_profiles(). Chosen trade-off: never
             * over-claim. */
            info.isWritable = (drive_list[i].write_cdr
                               || drive_list[i].write_cdrw
                               || drive_list[i].write_dvdr
                               || drive_list[i].write_dvdram) ? 1 : 0;

            /* Media state is not inquired during enumeration (a grab would
             * be needed to query the loaded disc; drive spin-up as a side
             * effect of a list refresh is not acceptable). Report unknown as
             * "assumed blank" like the other backends, documented in the
             * public header. */
            info.isEmpty = 1;

            if (drive_list[i].write_cdr || drive_list[i].write_cdrw) {
                strncpy(info.mediaType, "CD-R/CD-RW", sizeof(info.mediaType) - 1);
            } else if (info.isWritable) {
                strncpy(info.mediaType, "DVD-R/DVD-RAM", sizeof(info.mediaType) - 1);
            } else {
                strncpy(info.mediaType, "CD/DVD (read-only)", sizeof(info.mediaType) - 1);
            }

            /* Use burn_drive_d_get_adr() instead of deprecated .location field */
            char adr[BURN_DRIVE_ADR_LEN] = {0};
            if (burn_drive_d_get_adr(drive_list[i].drive, adr) > 0 && adr[0]) {
                /* BURN_DRIVE_ADR_LEN (1024) is larger than devicePath (512):
                 * clamp the precision so the compiler and reality agree. */
                snprintf(info.devicePath, sizeof(info.devicePath), "%.511s", adr);
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

        /* burn_drive_set_speed() counts k/s (k = 1000), NOT speed multipliers.
         * 1x is media-family specific: CD 176.4 k/s (rounded up per upstream
         * rule), DVD 1385 k/s, BD 4496 k/s. The old code passed speed*1000,
         * which asked for e.g. 4000 k/s = ~22x CD. Read speed is now left
         * at 0 = drive maximum; the old call also clamped reads to the
         * write speed. */
        if (speed > 0) {
            char family = 'D';
            int pno = 0;
            char pname[80] = {0};
            if (burn_disc_get_profile(drive, &pno, pname) == 1) {
                if (pno == PROFILE_CD_R || pno == PROFILE_CD_RW) family = 'C';
                else if (pno == PROFILE_BD_R_SEQ || pno == PROFILE_BD_RE) family = 'B';
            } else if (!discType.empty()) {
                /* No media answer yet: fall back to the requested type text. */
                char c = discType[0];
                if (c == 'C' || c == 'c') family = 'C';
                else if (c == 'B' || c == 'b') family = 'B';
            }
            long long ks = (long long)speed * KsPerOneX(family);
            int maxKs = burn_drive_get_write_speed(drive); /* k/s, <=0 unknown */
            if (maxKs > 0 && ks > maxKs) ks = maxKs;
            if (ks < 1) ks = 1;
            burn_drive_set_speed(drive, 0, (int)ks);
        }

        /* burn_disc_get_status() may need more than one call to settle
         * (documented; BURN_DISC_UNREADY = "status not yet known"). The old
         * single call falsely failed with "not blank or appendable" on slow
         * drives. */
        enum burn_disc_status status = BURN_DISC_UNREADY;
        for (int attempt = 0; attempt < 40 && !IsBurnCancelled(); ++attempt) {
            status = burn_disc_get_status(drive);
            if (status != BURN_DISC_UNREADY) break;
            usleep(250000); /* 250ms, up to 10s total */
        }
        if (IsBurnCancelled()) {
            SetBurnLastError("Burn cancelled by caller");
            burn_drive_release(drive, 0);
            burn_drive_info_free(drive_info);
            return false;
        }
        if (status == BURN_DISC_EMPTY) {
            SetBurnLastError("No disc in drive: " + devicePath);
            burn_drive_release(drive, 0);
            burn_drive_info_free(drive_info);
            return false;
        }
        if (status != BURN_DISC_BLANK && status != BURN_DISC_APPENDABLE) {
            SetBurnLastError(status == BURN_DISC_UNREADY
                             ? "Disc status could not be determined"
                             : "Disc is not blank or appendable");
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
         * recent asynchronous burn_disc_write() (1 = ok, 0 = failed).
         * Decide success BEFORE honouring a late cancellation: a cancel
         * that lands after the drive finished writing must not report a
         * good disc as merely "cancelled", or the caller skips its verify
         * step on a disc that is actually complete. */
        bool wroteOk = (burn_drive_wrote_well(drive) > 0);
        if (cancelled && wroteOk) {
            /* The write completed before the abort took effect. */
            cancelled = false;
        }
        bool burn_ok = !cancelled && wroteOk;

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
        bool writeError = false;
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
            /* written must advance by what fwrite REALLY stored, else the
             * `written < total` guard below cannot see a full disk: the
             * source read succeeds, fread-like progress looks perfect, and
             * only the output write fails. */
            size_t stored = fwrite(buf, 1, (size_t)r, f);
            if (stored != (size_t)r) {
                writeError = true;
                break;
            }
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

        /* fclose() flushes the stdio buffer; a buffered tail that fails to
         * reach the disk only reports its error HERE, never on fwrite. */
        int closeErr = (fclose(f) != 0);
        iso_image_unref(image);

        if (cancelled) {
            remove(isoPath.c_str());
            SetBurnLastError("ISO creation cancelled by caller");
            return false;
        }
        if (writeError || closeErr || written < total) {
            remove(isoPath.c_str());
            SetBurnLastError(written < total && !writeError && !closeErr
                             ? "ISO image was truncated while writing"
                             : "ISO write failed (disk full or I/O error)");
            return false;
        }

        NotifyProgressBurn(100, "ISO creation completed");
        return true;
    }

    std::string GetPlatformInfo() override {
        return "Linux/Unix (libburn/libisofs)";
    }

    /* ---- API version 3 capabilities, implemented in unix_features.cpp ---- */

    int Capability(int capability) override { return burnunix::Capability(capability); }

    long long NextSessionStartBlock(const std::string& devicePath) override {
        return burnunix::NextSessionStartBlock(devicePath);
    }

    bool CreateImage(const BurnImageSpec& spec) override {
        return burnunix::CreateImage(spec);
    }

    bool BurnImageFile(const std::string& imagePath, const std::string& devicePath,
                       const std::string& discType, int speed, int verifyLevel,
                       bool finalize, unsigned long long sessionStartBlock) override {
        return burnunix::BurnImageFile(imagePath, devicePath, discType, speed,
                                       verifyLevel, finalize, sessionStartBlock);
    }

    bool BurnAudioTracks(const std::vector<BurnAudioTrackSpec>& tracks,
                         const std::string& devicePath, int speed,
                         int verifyLevel, bool finalizeDisc) override {
        return burnunix::BurnAudioTracks(tracks, devicePath, speed, verifyLevel, finalizeDisc);
    }

    bool EraseDisc(const std::string& devicePath, bool full) override {
        return burnunix::EraseDisc(devicePath, full);
    }

    bool CreateRawImage(const std::vector<BurnAudioTrackSpec>& tracks,
                        const std::string& outputPath, const std::string& mcn,
                        int startingTrackNumber) override {
        return burnunix::CreateRawImage(tracks, outputPath, mcn, startingTrackNumber);
    }

    bool BurnRawImage(const std::string& imagePath, const std::string& devicePath,
                      int speed, int verifyLevel) override {
        return burnunix::BurnRawImage(imagePath, devicePath, speed, verifyLevel);
    }

    bool VerifyWrite(const std::string& devicePath, const std::string& expectedImagePath,
                     int level) override {
        return burnunix::VerifyWrite(devicePath, expectedImagePath, level);
    }

    int EjectTray(const std::string& devicePath) override {
        return burnunix::EjectTray(devicePath);
    }

    int CloseTray(const std::string& devicePath) override {
        return burnunix::CloseTray(devicePath);
    }

    std::string DriveInformation(const std::string& devicePath) override {
        return burnunix::DriveInformation(devicePath);
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
