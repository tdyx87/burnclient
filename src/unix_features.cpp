/* ---------------------------------------------------------------------------
 * unix_features.cpp - the API v3 surface on libburn + libisofs.
 *
 * Every libburn/libisofs symbol used here was verified against the installed
 * headers (see the notes in unix_platform.cpp about libburn living in
 * namespace burn and libisofs staying global).
 *
 * Deliberate limitations, all reported honestly through Capability():
 *   - UDF: libisofs 1.5.2 has no UDF support whatsoever, so it reports 0.
 *   - Read-back verification reads the device node directly because libburn's
 *     read API exposes no documented output sink.
 * ------------------------------------------------------------------------- */
#include "unix_features.h"

#if !defined(_WIN32) && !defined(__APPLE__)

#include "burn_internal.h"
#include "media_format.h"

#include <libburn/libburn.h>
#include <libisofs/libisofs.h>

#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

using namespace burn;

namespace burnunix {

/* ==========================================================================
 * Small helpers
 * ======================================================================== */
static const unsigned kSector = 2048;      /* cooked data sector */
static const unsigned kAudioSector = 2352; /* CD-DA sector */
static const unsigned kSubcode = 96;       /* raw subcode block per sector */

/* Disc speed in k/s for one speed multiplier. libburn wants k/s. */
static int KsPerOneX(char family) {
    switch (family) {
    case 'C': case 'c': return 177;    /* CD  176.4 -> 177 */
    case 'B': case 'b': return 4496;   /* BD  4495.6 -> 4496 */
    default: return 1385;              /* DVD 1384.8 -> 1385 */
    }
}

static int WriteKs(int multiplier, const std::string& discType) {
    if (multiplier <= 0) return 0;
    const char family = discType.empty() ? 'C' : discType[0];
    return multiplier * KsPerOneX(family);
}

/* Owning drive handle: grabs on open(), releases on destruction. */
class Drive {
public:
    Drive() = default;
    ~Drive() {
        if (info_) {
            if (dev_) burn_drive_release(dev_, 0);
            burn_drive_info_free(info_);
        }
    }
    Drive(const Drive&) = delete;
    Drive& operator=(const Drive&) = delete;

    bool open(const std::string& devicePath) {
        /* burn_drive_scan_and_grab() takes char*, not const char*. */
        std::vector<char> adr(devicePath.begin(), devicePath.end());
        adr.push_back('\0');
        struct burn_drive_info* info = nullptr;
        if (burn_drive_scan_and_grab(&info, adr.data(), 0) <= 0) {
            SetBurnLastError("Cannot open burner '" + devicePath +
                             "'. Check that the device node exists and that you have "
                             "write permission on it (usually group 'cdrom' or root).");
            return false;
        }
        info_ = info;
        dev_ = info[0].drive;
        return dev_ != nullptr;
    }

    struct burn_drive* dev() const { return dev_; }
    struct burn_drive_info* info() const { return info_; }

private:
    struct burn_drive_info* info_ = nullptr;
    struct burn_drive* dev_ = nullptr;
};

/* Cancellation: burn_drive_cancel() aborts the asynchronous write. */
struct CancelCtx {
    std::mutex mutex;
    struct burn_drive* drive = nullptr;
};

static void CancelHook(void* context) {
    CancelCtx* ctx = static_cast<CancelCtx*>(context);
    std::lock_guard<std::mutex> lock(ctx->mutex);
    if (ctx->drive) burn_drive_cancel(ctx->drive);
}

class CancelArm {
public:
    explicit CancelArm(CancelCtx& ctx) : ctx_(&ctx) { SetBurnCancelHook(CancelHook, ctx_); }
    ~CancelArm() {
        {
            std::lock_guard<std::mutex> lock(ctx_->mutex);
            ctx_->drive = nullptr;
        }
        ClearBurnCancelHook(CancelHook, ctx_);
    }
    CancelArm(const CancelArm&) = delete;
    CancelArm& operator=(const CancelArm&) = delete;
    void bind(struct burn_drive* d, const char* label) {
        {
            std::lock_guard<std::mutex> lock(ctx_->mutex);
            ctx_->drive = d;
        }
        label_ = label;
    }
private:
    CancelCtx* ctx_;
    const char* label_ = "Burning";
};

/* Poll the asynchronous operation until the drive is idle again. */
static void PumpDrive(struct burn_drive* drive, const char* label, bool allowCancel) {
    bool cancelSent = false;
    struct burn_progress progress;
    for (;;) {
        std::memset(&progress, 0, sizeof(progress));
        const enum burn_drive_status st = burn_drive_get_status(drive, &progress);
        if (st == BURN_DRIVE_IDLE) break;

        if (allowCancel && IsBurnCancelled() && !cancelSent) {
            cancelSent = true;
            burn_drive_cancel(drive);
            NotifyProgressBurn(0, "Cancelling...");
        }
        if (!cancelSent && progress.sectors > 0) {
            int percent = (int)(progress.sector * 100.0 / progress.sectors);
            if (percent < 0) percent = 0;
            if (percent > 99) percent = 99;
            char msg[96];
            std::snprintf(msg, sizeof(msg), "%s... %d%%", label, percent);
            NotifyProgressBurn(percent, msg);
        }
        usleep(100000);
    }
}

static const char* DiscStatusName(enum burn_disc_status s) {
    switch (s) {
    case BURN_DISC_UNREADY: return "unready";
    case BURN_DISC_BLANK: return "blank";
    case BURN_DISC_EMPTY: return "no disc";
    case BURN_DISC_APPENDABLE: return "appendable";
    case BURN_DISC_FULL: return "full/closed";
    case BURN_DISC_UNSUITABLE: return "unsuitable";
    default: return "unknown";
    }
}

/* ==========================================================================
 * Capability table. UDF is 0 on purpose: libisofs cannot write UDF.
 * ======================================================================== */
int Capability(int capability) {
    switch (capability) {
    case BURN_CAP_IMAGE_BURN:      return 1;
    case BURN_CAP_CREATE_IMAGE:    return 1;
    case BURN_CAP_AUDIO_TAO:       return 1;
    case BURN_CAP_ERASE:           return 1;
    case BURN_CAP_MULTISESSION:    return 1;
    case BURN_CAP_UDF:             return 0;   /* libisofs has no UDF support */
    case BURN_CAP_BOOT_IMAGE:      return 1;
    case BURN_CAP_READBACK_VERIFY: return 1;
    case BURN_CAP_RAW_BURN:        return 1;
    case BURN_CAP_RAW_IMAGE:       return 1;
    case BURN_CAP_TRAY_CONTROL:    return 1;
    case BURN_CAP_DRIVE_INFO:      return 1;
    default:                       return 0;
    }
}

/* ==========================================================================
 * Multisession inquiry
 * ======================================================================== */
long long NextSessionStartBlock(const std::string& devicePath) {
    Drive drive;
    if (!drive.open(devicePath)) return -1;
    struct burn_drive* d = drive.dev();

    /* Wait for the drive to finish whatever it was doing. */
    PumpDrive(d, "Reading disc state", false);

    const enum burn_disc_status status = burn_disc_get_status(d);
    if (status == BURN_DISC_EMPTY || status == BURN_DISC_UNREADY) {
        SetBurnLastError("No readable disc in " + devicePath +
                         " (status: " + DiscStatusName(status) + ")");
        return -1;
    }
    if (status == BURN_DISC_BLANK) {
        return 0;   /* the pending session would be the first one */
    }
    if (status != BURN_DISC_APPENDABLE) {
        SetBurnLastError("Disc in " + devicePath + " is " + DiscStatusName(status) +
                         ": no further session can be added");
        return -1;
    }

    int lba = 0, nwa = 0;
    if (burn_disc_track_lba_nwa(d, nullptr, 0, &lba, &nwa) <= 0) {
        int msc1 = 0;
        if (burn_disc_get_msc1(d, &msc1) <= 0) {
            SetBurnLastError("Failed to obtain the next writable address of the disc in " +
                             devicePath);
            return -1;
        }
        return msc1;
    }
    return nwa > 0 ? (long long)nwa : -1;
}

/* ==========================================================================
 * Drive information
 * ======================================================================== */
std::string DriveInformation(const std::string& devicePath) {
    std::string out;
    Drive drive;
    if (!drive.open(devicePath)) return out;
    struct burn_drive* d = drive.dev();
    struct burn_drive_info* info = drive.info();

    PumpDrive(d, "Reading drive status", false);

    out += "devicePath: " + devicePath + "\n";
    out += "backend:    libburn/libisofs\n";
    out += std::string("vendor:     ") + info[0].vendor + "\n";
    out += std::string("product:    ") + info[0].product + "\n";
    out += std::string("revision:   ") + info[0].revision + "\n";

    out += "writeSupport:";
    if (info[0].write_cdr) out += " CD-R";
    if (info[0].write_cdrw) out += " CD-RW";
    if (info[0].write_dvdr) out += " DVD-R";
    if (info[0].write_dvdram) out += " DVD-RAM";
    out += "\n";
    out += "readSupport:";
    if (info[0].read_cdr) out += " CD-R";
    if (info[0].read_cdrw) out += " CD-RW";
    if (info[0].read_dvdr) out += " DVD-R";
    if (info[0].read_dvdrom) out += " DVD-ROM";
    if (info[0].read_dvdram) out += " DVD-RAM";
    out += "\n";

    out += std::string("discStatus: ") + DiscStatusName(burn_disc_get_status(d)) + "\n";

    int capacity = 0;
    if (burn_get_read_capacity(d, &capacity, 0) > 0) {
        char b[128];
        std::snprintf(b, sizeof(b), "readCapacity: %d sectors (%.1f MB)\n", capacity,
                      (double)capacity * kSector / (1024.0 * 1024.0));
        out += b;
    }

    int erasable = burn_disc_erasable(d);
    out += std::string("erasable:   ") + (erasable > 0 ? "yes" : "no") + "\n";
    return out;
}

/* ==========================================================================
 * Tray control
 * ======================================================================== */
static int TrayCommand(const char* name, const std::string& devicePath, bool eject) {
    /* libburn's tray control is tied to grabbing the drive: grab with load=1
     * closes the tray, and release with eject=1 opens it. */
    std::vector<char> adr(devicePath.begin(), devicePath.end());
    adr.push_back('\0');
    struct burn_drive_info* info = nullptr;
    if (burn_drive_scan_and_grab(&info, adr.data(), 1) <= 0) {
        SetBurnLastError(std::string(name) + ": cannot open '" + devicePath + "'");
        return BURN_ERROR_DEVICE_NOT_FOUND;
    }
    struct burn_drive* d = info[0].drive;
    PumpDrive(d, "Tray", false);
    burn_drive_release(d, eject ? 1 : 0);
    burn_drive_info_free(info);
    return BURN_SUCCESS;
}

int EjectTray(const std::string& devicePath) {
    return TrayCommand("EjectTray", devicePath, true);
}

int CloseTray(const std::string& devicePath) {
    return TrayCommand("CloseTray", devicePath, false);
}

/* ==========================================================================
 * Erase / format
 * ======================================================================== */
bool EraseDisc(const std::string& devicePath, bool full) {
    NotifyProgressBurn(5, "Preparing erase...");
    Drive drive;
    if (!drive.open(devicePath)) return false;
    struct burn_drive* d = drive.dev();

    PumpDrive(d, "Reading disc state", false);
    const enum burn_disc_status status = burn_disc_get_status(d);
    if (status == BURN_DISC_EMPTY) {
        SetBurnLastError("No disc in " + devicePath);
        return false;
    }
    if (burn_disc_erasable(d) <= 0) {
        SetBurnLastError("Disc in " + devicePath + " is not erasable (" +
                         DiscStatusName(status) +
                         "). Only CD-RW, DVD-RW, DVD+RW, DVD-RAM and BD-RE can be erased.");
        return false;
    }

    NotifyProgressBurn(20, full ? "Erasing (full)..." : "Erasing (fast)...");
    /* burn_disc_erase() is asynchronous; `fast` nonzero blanks only the
     * headers, zero erases the whole disc. */
    burn_disc_erase(d, full ? 0 : 1);
    PumpDrive(d, "Erasing", false);

    if (burn_disc_get_status(d) == BURN_DISC_UNSUITABLE) {
        SetBurnLastError("Erase failed: the drive reports unsuitable media");
        return false;
    }
    NotifyProgressBurn(100, "Erase completed");
    return true;
}

/* ==========================================================================
 * Read-back verification: read the device node directly.
 *
 * Recommends a data session: sectors are compared as 2048-byte cooked blocks,
 * which is exactly what an ISO image contains. The session base is found by
 * matching the image's own primary volume descriptor, so an appended session
 * is located correctly too.
 * ======================================================================== */
bool VerifyWrite(const std::string& devicePath, const std::string& expectedImagePath,
                 int level) {
    const int fd = ::open(devicePath.c_str(), O_RDONLY);
    if (fd < 0) {
        SetBurnLastError("VerifyWrite: cannot open '" + devicePath +
                         "' for reading (permission or missing device)");
        return false;
    }

    FILE* img = std::fopen(expectedImagePath.c_str(), "rb");
    if (!img) {
        ::close(fd);
        SetBurnLastError("VerifyWrite: cannot open the reference image: " + expectedImagePath);
        return false;
    }
    std::fseek(img, 0, SEEK_END);
    const long long imageSize = std::ftell(img);
    if (imageSize < (long long)(34 * kSector)) {
        std::fclose(img);
        ::close(fd);
        SetBurnLastError("VerifyWrite: the reference image is too small to contain a "
                         "primary volume descriptor");
        return false;
    }
    const unsigned long long totalSectors = (unsigned long long)imageSize / kSector;

    std::vector<unsigned char> imagePvd(kSector);
    std::fseek(img, 16 * (long)kSector, SEEK_SET);
    if (std::fread(imagePvd.data(), 1, kSector, img) != kSector ||
        imagePvd[0] != 1 || std::memcmp(imagePvd.data() + 1, "CD001", 5) != 0) {
        std::fclose(img);
        ::close(fd);
        SetBurnLastError("VerifyWrite: the reference image has no primary volume "
                         "descriptor at its LBA 16 - is it an ISO 9660 image?");
        return false;
    }

    std::vector<unsigned char> buf(kSector);
    unsigned long long baseLba = 0;
    bool baseFound = false;
    for (unsigned long long cand = 16; cand <= 16 + 512 && !baseFound; ++cand) {
        if (IsBurnCancelled()) {
            std::fclose(img);
            ::close(fd);
            SetBurnLastError("Verification cancelled by caller");
            return false;
        }
        const off_t off = (off_t)cand * kSector;
        if (::pread(fd, buf.data(), kSector, off) != (ssize_t)kSector) continue;
        if (std::memcmp(buf.data(), imagePvd.data(), kSector) == 0) {
            baseLba = cand - 16;
            baseFound = true;
        }
    }
    if (!baseFound) {
        std::fclose(img);
        ::close(fd);
        SetBurnLastError("VerifyWrite: no session on the disc in " + devicePath +
                         " matches the reference image");
        return false;
    }

    std::vector<unsigned long long> targets;
    if (level == BURN_VERIFY_QUICK) {
        targets.push_back(0);
        targets.push_back(16);
        for (int i = 0; i < 64; ++i)
            targets.push_back((unsigned long long)((double)totalSectors * i / 64.0));
        if (totalSectors > 1) targets.push_back(totalSectors - 1);
    } else {
        targets.reserve((size_t)totalSectors);
        for (unsigned long long s = 0; s < totalSectors; ++s) targets.push_back(s);
    }

    std::vector<unsigned char> imageSector(kSector);
    unsigned long long compared = 0, mismatched = 0, firstMismatch = 0;
    bool ioFailed = false, cancelled = false;
    for (size_t i = 0; i < targets.size(); ++i) {
        if (IsBurnCancelled()) { cancelled = true; break; }
        const unsigned long long sector = targets[i];
        if (sector >= totalSectors) continue;
        std::fseek(img, (long)(sector * kSector), SEEK_SET);
        if (std::fread(imageSector.data(), 1, kSector, img) != kSector) {
            ioFailed = true;
            break;
        }
        const off_t off = (off_t)(baseLba + sector) * kSector;
        if (::pread(fd, buf.data(), kSector, off) != (ssize_t)kSector) {
            SetBurnLastError("VerifyWrite: read error at disc LBA " +
                             std::to_string(baseLba + sector));
            ioFailed = true;
            break;
        }
        if (std::memcmp(buf.data(), imageSector.data(), kSector) != 0) {
            if (mismatched == 0) firstMismatch = sector;
            ++mismatched;
            if (level == BURN_VERIFY_QUICK) break;
        }
        ++compared;
        if ((i % 16) == 0 || i + 1 == targets.size()) {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "Compared %llu of %zu sectors",
                          (unsigned long long)compared, targets.size());
            NotifyProgressBurn(5 + (int)((90 * (i + 1)) / targets.size()), msg);
        }
    }
    std::fclose(img);
    ::close(fd);

    if (cancelled) {
        SetBurnLastError("Verification cancelled by caller");
        return false;
    }
    if (ioFailed) return false;
    if (mismatched) {
        SetBurnLastError("Verification FAILED: " + std::to_string(mismatched) + " of " +
                         std::to_string(compared) + " sectors differ, first at image sector " +
                         std::to_string(firstMismatch) + " (disc LBA " +
                         std::to_string(baseLba + firstMismatch) + ")");
        return false;
    }
    char ok[128];
    std::snprintf(ok, sizeof(ok), "Verification passed: %llu sectors compared byte-exact",
                  (unsigned long long)compared);
    SetBurnLastError(ok);
    NotifyProgressBurn(100, "Verification completed");
    return true;
}

/* ==========================================================================
 * Image creation with libisofs: ISO level, Joliet, El Torito, continuation
 * ======================================================================== */
static std::string BaseName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool CreateImage(const BurnImageSpec& spec) {
    if (IsBurnCancelled()) {
        SetBurnLastError("ISO creation cancelled by caller");
        return false;
    }
    if (spec.fileSystems & BURN_FS_UDF) {
        /* Guard even though Capability() already reports 0: a caller that
         * ignores the capability table still gets a precise reason. */
        SetBurnLastError("UDF images are not supported on Linux: libisofs has no UDF "
                         "writer. Create an ISO9660+Joliet image instead.");
        return false;
    }

    NotifyProgressBurn(10, "Creating ISO image...");
    const std::string label = spec.volumeLabel.empty() ? std::string("DISC") : spec.volumeLabel;

    IsoImage* image = nullptr;
    if (iso_image_new(label.c_str(), &image) < 0 || !image) {
        SetBurnLastError("iso_image_new() failed");
        return false;
    }
    /* RAII for the image reference. */
    struct ImageGuard {
        IsoImage* img;
        ~ImageGuard() { if (img) iso_image_unref(img); }
    } guard{image};

    iso_image_set_volume_id(image, label.c_str());

    IsoDir* root = iso_image_get_root(image);
    if (!root || iso_tree_add_dir_rec(image, root, spec.sourcePath.c_str()) < 0) {
        SetBurnLastError("Failed to add source directory to the image: " + spec.sourcePath);
        return false;
    }

    /* El Torito needs the load clip to be a file *inside* the image, so it is
     * added to the root and then referenced by its in-image path. */
    if (!spec.bootImagePath.empty()) {
        IsoNode* node = nullptr;
        if (iso_tree_add_node(image, root, spec.bootImagePath.c_str(), &node) < 0 || !node) {
            SetBurnLastError("Failed to add the boot image to the ISO tree: " +
                             spec.bootImagePath);
            return false;
        }
        enum eltorito_boot_media_type media = ELTORITO_NO_EMUL;
        switch (spec.bootEmulation) {
        case BURN_BOOT_EMU_FLOPPY_12:
        case BURN_BOOT_EMU_FLOPPY_144:
        case BURN_BOOT_EMU_FLOPPY_288: media = ELTORITO_FLOPPY_EMUL; break;
        case BURN_BOOT_EMU_HARDDISK:   media = ELTORITO_HARD_DISC_EMUL; break;
        default:                       media = ELTORITO_NO_EMUL; break;
        }
        const std::string inImage = "/" + BaseName(spec.bootImagePath);
        ElToritoBootImage* boot = nullptr;
        if (iso_image_set_boot_image(image, inImage.c_str(), media, "/BOOT.CAT", &boot) < 0) {
            SetBurnLastError("iso_image_set_boot_image() failed for " + inImage);
            return false;
        }
        if (boot) {
            uint8_t platform = 0;   /* 0x00 = 80x86 */
            switch (spec.bootPlatform) {
            case BURN_BOOT_PLATFORM_PPC: platform = 0x01; break;
            case BURN_BOOT_PLATFORM_MAC: platform = 0x02; break;
            case BURN_BOOT_PLATFORM_EFI: platform = 0xEF; break;
            default:                     platform = 0x00; break;
            }
            if (el_torito_set_boot_platform_id(boot, platform) < 0) {
                SetBurnLastError("Failed to set the El Torito platform id");
                return false;
            }
        }
    }

    IsoWriteOpts* opts = nullptr;
    if (iso_write_opts_new(&opts, 0) < 0 || !opts) {
        SetBurnLastError("Failed to create ISO write options");
        return false;
    }
    struct OptsGuard {
        IsoWriteOpts* o;
        ~OptsGuard() { if (o) iso_write_opts_free(o); }
    } optsGuard{opts};

    const int level = (spec.interchangeLevel >= 1 && spec.interchangeLevel <= 3)
                          ? spec.interchangeLevel : 3;
    iso_write_opts_set_iso_level(opts, level);
    iso_write_opts_set_rockridge(opts, 1);
    iso_write_opts_set_joliet(opts, (spec.fileSystems & BURN_FS_JOLIET) ? 1 : 0);

    /* Continuation session: the image must know it starts at this LBA so all
     * internal references stay correct. */
    if (spec.sessionStartBlock != 0) {
        iso_write_opts_set_ms_block(opts, (uint32_t)spec.sessionStartBlock);
    }

    FILE* f = std::fopen(spec.outputPath.c_str(), "wb");
    if (!f) {
        SetBurnLastError("Cannot open output file: " + spec.outputPath);
        return false;
    }

    ::burn_source* iso_src = nullptr;
    if (iso_image_create_burn_source(image, opts, &iso_src) < 0 || !iso_src) {
        std::fclose(f);
        SetBurnLastError("iso_image_create_burn_source() failed");
        return false;
    }
    burn::burn_source* src = reinterpret_cast<burn::burn_source*>(iso_src);

    /* burn_source exposes read()/get_size() function pointers; there is no
     * .size field and no .free field (see unix_platform.cpp). */
    off_t total = 0;
    if (src->get_size) total = src->get_size(src);

    std::vector<char> buf(65536);
    off_t written = 0;
    bool cancelled = false, writeError = false, readError = false;
    while (written < total) {
        if (IsBurnCancelled()) { cancelled = true; break; }
        const int toRead = (int)((total - written > (off_t)buf.size())
                                     ? (off_t)buf.size() : (total - written));
        int r = -1;
        if (src->read) r = src->read(src, (unsigned char*)buf.data(), toRead);
        else if (src->version >= 1 && src->read_xt)
            r = src->read_xt(src, (unsigned char*)buf.data(), toRead);
        if (r <= 0) { readError = true; break; }
        if (std::fwrite(buf.data(), 1, (size_t)r, f) != (size_t)r) {
            writeError = true;
            break;
        }
        written += r;
        if (total > 0) {
            int percent = (int)(written * 100 / total);
            if (percent > 99) percent = 99;
            char msg[64];
            std::snprintf(msg, sizeof(msg), "Writing ISO... %d%%", percent);
            NotifyProgressBurn(10 + percent * 80 / 100, msg);
        }
    }
    burn_source_free(src);
    const int closeErr = (std::fclose(f) != 0);

    if (cancelled || writeError || readError || closeErr) {
        std::remove(spec.outputPath.c_str());
        if (cancelled) SetBurnLastError("ISO creation cancelled by caller");
        else if (writeError || closeErr)
            SetBurnLastError("ISO write failed (disk full or I/O error)");
        else SetBurnLastError("Failed to read the ISO image stream");
        return false;
    }
    NotifyProgressBurn(100, "ISO creation completed");
    return true;
}

/* ==========================================================================
 * Writing an image, with multi-session control
 * ======================================================================== */
bool BurnImageFile(const std::string& imagePath, const std::string& devicePath,
                   const std::string& discType, int speed, int verifyLevel,
                   bool finalize, unsigned long long sessionStartBlock) {
    NotifyProgressBurn(10, "Initializing burn process...");
    if (IsBurnCancelled()) {
        SetBurnLastError("Burn cancelled by caller");
        return false;
    }

    Drive drive;
    if (!drive.open(devicePath)) return false;
    struct burn_drive* d = drive.dev();
    PumpDrive(d, "Reading disc state", false);

    const enum burn_disc_status status = burn_disc_get_status(d);
    if (status == BURN_DISC_EMPTY) {
        SetBurnLastError("No disc in " + devicePath);
        return false;
    }
    if (sessionStartBlock != 0 && status != BURN_DISC_APPENDABLE) {
        SetBurnLastError("A continuation session was requested but the disc in " +
                         devicePath + " is " + DiscStatusName(status));
        return false;
    }
    if (status == BURN_DISC_FULL) {
        SetBurnLastError("Disc in " + devicePath + " is already closed; "
                         "no session can be written");
        return false;
    }

    /* libburn wants k/s, not a multiplier. Read speed stays 0 (maximum). */
    if (speed > 0) {
        burn_drive_set_speed(d, 0, WriteKs(speed, discType));
    }

    NotifyProgressBurn(30, "Preparing disc...");
    struct burn_disc* disc = burn_disc_create();
    struct burn_session* session = burn_session_create();
    burn_disc_add_session(disc, session, BURN_POS_END);

    burn::burn_source* src = burn_file_source_new(imagePath.c_str(), nullptr);
    if (!src) {
        SetBurnLastError("Failed to create a burn source from " + imagePath);
        burn_session_free(session);
        burn_disc_free(disc);
        return false;
    }
    struct burn_track* track = burn_track_create();
    burn_track_set_source(track, src);
    burn_session_add_track(session, track, BURN_POS_END);

    struct burn_write_opts* opts = burn_write_opts_new(d);
    burn_write_opts_set_perform_opc(opts, 0);

    char reasons[4096] = {0};
    if (burn_write_opts_auto_write_type(opts, disc, reasons, 0) == BURN_WRITE_NONE) {
        SetBurnLastError(std::string("Failed to determine a write type") +
                         (reasons[0] ? std::string(": ") + reasons : std::string()));
        burn_write_opts_free(opts);
        burn_track_free(track);
        burn_session_free(session);
        burn_disc_free(disc);
        return false;
    }

    /* multi = 1 keeps the media appendable, which is the opposite of
     * "finalize". libburn's default is to close the session. */
    burn_write_opts_set_multi(opts, finalize ? 0 : 1);

    NotifyProgressBurn(50, "Burning in progress...");
    CancelCtx ctx;
    CancelArm arm(ctx);
    arm.bind(d, "Burning");
    burn_disc_write(opts, disc);
    PumpDrive(d, "Burning", true);

    const bool wroteOk = (burn_drive_wrote_well(d) > 0);
    bool cancelled = IsBurnCancelled();
    if (cancelled && wroteOk) cancelled = false;   /* finished before the abort */

    burn_write_opts_free(opts);
    burn_track_free(track);
    burn_session_free(session);
    burn_disc_free(disc);

    if (cancelled) {
        SetBurnLastError("Burn cancelled by caller");
        return false;
    }
    if (!wroteOk) {
        SetBurnLastError("Burn failed: the drive reported a write error for " + imagePath);
        return false;
    }
    NotifyProgressBurn(90, "Burn completed");

    /* FULL verification is our own read-back; libburn has no verify API. */
    if (verifyLevel == BURN_VERIFY_FULL) {
        if (!VerifyWrite(devicePath, imagePath, BURN_VERIFY_FULL)) return false;
    } else if (verifyLevel == BURN_VERIFY_QUICK) {
        /* Not supported by the drive: do the sampled comparison instead so
         * the request is honoured rather than silently ignored. */
        if (!VerifyWrite(devicePath, imagePath, BURN_VERIFY_QUICK)) return false;
    }
    NotifyProgressBurn(100, "Burn completed");
    return true;
}

/* ==========================================================================
 * Audio CD (track-at-once)
 *
 * libburn requires raw 44.1 kHz / 16 bit / stereo little-endian PCM with no
 * header, so each WAV's data chunk is copied into a temporary headerless file
 * that libburn can consume as a track source.
 * ======================================================================== */
class TempPcm {
public:
    ~TempPcm() { if (!path_.empty()) ::unlink(path_.c_str()); }
    bool create(const burn_media::AudioPayload& p, const std::string& wavPath) {
        char tmpl[] = "/tmp/burnclient_pcm_XXXXXX";
        const int fd = ::mkstemp(tmpl);
        if (fd < 0) {
            SetBurnLastError("Cannot create a temporary file for the audio track");
            return false;
        }
        path_ = tmpl;

        FILE* in = std::fopen(wavPath.c_str(), "rb");
        if (!in) {
            SetBurnLastError("Cannot open audio file: " + wavPath);
            return false;
        }
        const unsigned long long bytes = p.sectorAlignedBytes();
        if (std::fseek(in, (long)p.dataOffset, SEEK_SET) != 0) {
            std::fclose(in);
            SetBurnLastError("Cannot seek to the audio data in " + wavPath);
            return false;
        }
        std::vector<char> buf(65536);
        unsigned long long left = bytes;
        while (left > 0) {
            const size_t want = (size_t)(left > buf.size() ? buf.size() : left);
            const size_t got = std::fread(buf.data(), 1, want, in);
            if (got == 0) {
                /* Pad the tail with silence so the track ends on a sector
                 * boundary instead of failing late in the burn. */
                std::memset(buf.data(), 0, want);
                if (::write(fd, buf.data(), want) != (ssize_t)want) {
                    std::fclose(in);
                    SetBurnLastError("Failed to write the temporary PCM file");
                    return false;
                }
                left -= want;
                continue;
            }
            if (::write(fd, buf.data(), got) != (ssize_t)got) {
                std::fclose(in);
                SetBurnLastError("Failed to write the temporary PCM file");
                return false;
            }
            left -= got;
        }
        std::fclose(in);
        ::close(fd);
        return true;
    }
    const std::string& path() const { return path_; }

private:
    std::string path_;
};

bool BurnAudioTracks(const std::vector<BurnAudioTrackSpec>& tracks,
                     const std::string& devicePath, int speed,
                     int verifyLevel, bool finalizeDisc) {
    if (tracks.empty() || tracks.size() > 99) {
        SetBurnLastError("An audio CD holds between 1 and 99 tracks");
        return false;
    }
    if (verifyLevel != BURN_VERIFY_NONE) {
        SetBurnLastError("Verification is not available on the audio track-at-once path; "
                         "use a data disc with Burn_VerifyWrite() if required");
    }

    NotifyProgressBurn(5, "Preparing audio session...");
    Drive drive;
    if (!drive.open(devicePath)) return false;
    struct burn_drive* d = drive.dev();
    PumpDrive(d, "Reading disc state", false);

    if (burn_disc_get_status(d) == BURN_DISC_EMPTY) {
        SetBurnLastError("No disc in " + devicePath);
        return false;
    }
    if (speed > 0) burn_drive_set_speed(d, 0, WriteKs(speed, "CD-R"));

    /* Materialise every track as headerless PCM before touching the drive. */
    std::vector<burn_media::AudioPayload> payloads(tracks.size());
    std::vector<TempPcm> pcmFiles(tracks.size());
    for (size_t i = 0; i < tracks.size(); ++i) {
        payloads[i] = burn_media::Probe(tracks[i].audioPath);
        if (!payloads[i].ok) {
            SetBurnLastError(payloads[i].error.empty()
                                 ? "Unreadable audio track: " + tracks[i].audioPath
                                 : payloads[i].error);
            return false;
        }
        if (!pcmFiles[i].create(payloads[i], tracks[i].audioPath)) return false;
    }

    struct burn_disc* disc = burn_disc_create();
    struct burn_session* session = burn_session_create();
    burn_disc_add_session(disc, session, BURN_POS_END);

    std::vector<struct burn_track*> trackObjs;
    std::vector<burn::burn_source*> sources;
    for (size_t i = 0; i < tracks.size(); ++i) {
        struct burn_track* track = burn_track_create();
        /* BURN_AUDIO marks the source as 2352-byte CD-DA frames. */
        int mode = BURN_AUDIO;
        if (tracks[i].preemphasis) mode |= BURN_PREEMPHASIS;
        burn_track_define_data(track, 0, 0, 1, mode);

        if (!tracks[i].isrc.empty()) {
            char isrc[13] = {0};
            std::snprintf(isrc, sizeof(isrc), "%s", tracks[i].isrc.c_str());
            burn_track_set_isrc_string(track, isrc, 0);
        }

        burn::burn_source* src = burn_file_source_new(pcmFiles[i].path().c_str(), nullptr);
        if (!src) {
            SetBurnLastError("Failed to create a burn source for track " +
                             std::to_string((int)i + 1));
            for (size_t k = 0; k < trackObjs.size(); ++k) burn_track_free(trackObjs[k]);
            burn_session_free(session);
            burn_disc_free(disc);
            return false;
        }
        burn_track_set_source(track, src);
        sources.push_back(src);
        burn_session_add_track(session, track, BURN_POS_END);
        trackObjs.push_back(track);
    }

    struct burn_write_opts* opts = burn_write_opts_new(d);
    burn_write_opts_set_perform_opc(opts, 0);
    char reasons[4096] = {0};
    enum burn_write_types wt = burn_write_opts_auto_write_type(opts, disc, reasons, 0);
    if (wt == BURN_WRITE_NONE) {
        SetBurnLastError(std::string("Failed to determine an audio write type") +
                         (reasons[0] ? std::string(": ") + reasons : std::string()));
        burn_write_opts_free(opts);
        for (size_t k = 0; k < trackObjs.size(); ++k) burn_track_free(trackObjs[k]);
        burn_session_free(session);
        burn_disc_free(disc);
        return false;
    }
    burn_write_opts_set_multi(opts, finalizeDisc ? 0 : 1);

    NotifyProgressBurn(30, "Writing audio tracks...");
    CancelCtx ctx;
    CancelArm arm(ctx);
    arm.bind(d, "Writing audio");
    burn_disc_write(opts, disc);
    PumpDrive(d, "Writing audio", true);

    const bool wroteOk = (burn_drive_wrote_well(d) > 0);
    bool cancelled = IsBurnCancelled();
    if (cancelled && wroteOk) cancelled = false;

    burn_write_opts_free(opts);
    for (size_t k = 0; k < trackObjs.size(); ++k) burn_track_free(trackObjs[k]);
    for (size_t k = 0; k < sources.size(); ++k) burn_source_free(sources[k]);
    burn_session_free(session);
    burn_disc_free(disc);

    if (cancelled) {
        SetBurnLastError("Audio session cancelled by caller");
        return false;
    }
    if (!wroteOk) {
        SetBurnLastError("Audio burn failed: the drive reported a write error. A CD-R "
                         "cannot be reused after a failed audio session.");
        return false;
    }
    NotifyProgressBurn(100, "Audio session completed");
    return true;
}

/* ==========================================================================
 * RAW image (RAW96R: 2352 bytes of audio + 96 bytes of interleaved subcode)
 *
 * libisofs cannot produce this, so the Q subchannel is generated here:
 *   control(4) address(4) track(8 BCD) index(8 BCD)
 *   relative M/S/F (24 BCD) reserved(8) absolute M/S/F (24 BCD) CRC-16(16)
 * The 96-byte subcode block interleaves the eight channels bit-wise: bit 0 of
 * every subcode byte is P, bit 1 is Q, and so on up to W in bit 7.
 * ======================================================================== */
static unsigned short Crc16Q(const unsigned char* q, int len) {
    /* CRC-16-CCITT, polynomial x^16 + x^12 + x^5 + 1, initial value 0,
     * transmitted inverted (Red Book Annex A). */
    unsigned short crc = 0;
    for (int i = 0; i < len; ++i) {
        crc ^= (unsigned short)((unsigned short)q[i] << 8);
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x8000) crc = (unsigned short)((crc << 1) ^ 0x1021);
            else crc = (unsigned short)(crc << 1);
        }
    }
    return (unsigned short)(crc ^ 0xFFFF);
}

static unsigned char ToBcd(int v) {
    if (v < 0) v = 0;
    return (unsigned char)(((v / 10) << 4) | (v % 10));
}

/* Build one 2448-byte RAW96R sector.
 *
 * relSector is the time within the track (0 at the track's index 1) and
 * absSector is the address on disc counted from the start of the lead-in,
 * so the first program-area sector of a disc reports 00:02:00 - the Red Book
 * convention. Using one value for both would put a 2-second error into every
 * Q subchannel. */
static void MakeRawSector(const unsigned char* audio2352, int control, int track,
                          int index, unsigned long long relSector,
                          unsigned long long absSector, int pBit,
                          unsigned char* out2448) {
    std::memcpy(out2448, audio2352, 2352);

    /* Q channel, 96 bits = 12 bytes, MSB first. */
    unsigned char q[12] = {0};
    q[0] = (unsigned char)(((control & 0x0F) << 4) | 0x01);   /* ADR = 1: position */
    q[1] = ToBcd(track);
    q[2] = ToBcd(index);

    /* Relative time within the track. */
    const int rel = (int)relSector;
    q[3] = ToBcd((rel / 75) / 60);
    q[4] = ToBcd((rel / 75) % 60);
    q[5] = ToBcd(rel % 75);
    q[6] = 0;
    /* Absolute address on disc. */
    const int mins = ((int)(absSector / 75)) / 60;
    const int secs = ((int)(absSector / 75)) % 60;
    const int frames = (int)(absSector % 75);
    q[7] = ToBcd(mins); q[8] = ToBcd(secs); q[9] = ToBcd(frames);

    const unsigned short crc = Crc16Q(q, 10);
    q[10] = (unsigned char)((crc >> 8) & 0xFF);
    q[11] = (unsigned char)(crc & 0xFF);

    /* Interleave into the 96-byte subcode block. */
    unsigned char* sc = out2448 + 2352;
    std::memset(sc, 0, 96);
    for (int bit = 0; bit < 96; ++bit) {
        const int byteIndex = bit / 8;
        const int bitInByte = 7 - (bit % 8);
        unsigned char value = 0;
        if ((pBit ? 1 : 0) & (1 << (7 - (bit % 8)))) value |= 0x01;
        if ((q[byteIndex] >> bitInByte) & 0x01) value |= 0x02;
        sc[bit] |= value;
    }
}

bool CreateRawImage(const std::vector<BurnAudioTrackSpec>& tracks,
                    const std::string& outputPath, const std::string& mcn,
                    int startingTrackNumber) {
    if (tracks.empty() || tracks.size() > 99) {
        SetBurnLastError("A RAW CD image holds between 1 and 99 tracks");
        return false;
    }
    if (!mcn.empty()) {
        bool digits = mcn.size() == 13;
        for (size_t i = 0; digits && i < mcn.size(); ++i)
            if (mcn[i] < '0' || mcn[i] > '9') digits = false;
        if (!digits) {
            SetBurnLastError("A media catalog number must be exactly 13 digits");
            return false;
        }
    }
    NotifyProgressBurn(5, "Building RAW image...");

    std::vector<burn_media::AudioPayload> payloads(tracks.size());
    std::vector<std::vector<unsigned char> > pcm(tracks.size());
    for (size_t i = 0; i < tracks.size(); ++i) {
        payloads[i] = burn_media::Probe(tracks[i].audioPath);
        if (!payloads[i].ok) {
            SetBurnLastError(payloads[i].error.empty()
                                 ? "Unreadable audio track: " + tracks[i].audioPath
                                 : payloads[i].error);
            return false;
        }
        const unsigned long long bytes = payloads[i].sectorAlignedBytes();
        pcm[i].assign((size_t)bytes, 0);
        FILE* f = std::fopen(tracks[i].audioPath.c_str(), "rb");
        if (!f) {
            SetBurnLastError("Cannot open audio file: " + tracks[i].audioPath);
            return false;
        }
        std::fseek(f, (long)payloads[i].dataOffset, SEEK_SET);
        const size_t got = std::fread(pcm[i].data(), 1, (size_t)bytes, f);
        std::fclose(f);
        if (got == 0) {
            SetBurnLastError("The audio data chunk is empty in " + tracks[i].audioPath);
            return false;
        }
    }

    FILE* out = std::fopen(outputPath.c_str(), "wb");
    if (!out) {
        SetBurnLastError("Cannot open output file: " + outputPath);
        return false;
    }

    std::vector<unsigned char> audio(kAudioSector, 0);
    std::vector<unsigned char> sector(kAudioSector + kSubcode, 0);
    const int firstTrack = (startingTrackNumber >= 1 && startingTrackNumber <= 99)
                               ? startingTrackNumber : 1;

    /* Standard RAW audio disc layout:
     *   - 150 lead-in sectors (2 s). The lead-in IS the track's index 0
     *     (pregap), so it is not written a second time as a separate
     *     pregap - doing that would shift every program sector by 150
     *     frames and put a 2 s error into every absolute address.
     *   - the program area, whose first sector is LBA 0 = MSF 00:02:00.
     *   - between two tracks, 150 gap sectors that belong to the NEXT
     *     track's index 0.
     *   - 150 lead-out sectors after the last track.
     */
    const unsigned leadIn = 150, gap = 150, leadOut = 150;
    unsigned long long abs = 0;   /* == file index == MSF frames from the
                                   * start of the lead-in */
    bool writeError = false;
    unsigned long long emitted = 0;

    for (unsigned s = 0; s < leadIn && !writeError; ++s) {
        /* P alternates inside the lead-in so the boundary is visible. */
        MakeRawSector(audio.data(), 0, firstTrack, 0, 0, abs, (s % 2) ? 0 : 1,
                       sector.data());
        if (std::fwrite(sector.data(), 1, sector.size(), out) != sector.size())
            writeError = true;
        ++abs; ++emitted;
    }

    for (size_t t = 0; t < tracks.size() && !writeError; ++t) {
        if (IsBurnCancelled()) {
            std::fclose(out);
            std::remove(outputPath.c_str());
            SetBurnLastError("RAW image creation cancelled by caller");
            return false;
        }
        const int trackNo = firstTrack + (int)t;
        const unsigned sectors = (unsigned)(pcm[t].size() / kAudioSector);

        /* Program area: index 1, P = 0, relative time counted from the
         * first program sector of this track. */
        for (unsigned s = 0; s < sectors && !writeError; ++s) {
            MakeRawSector(pcm[t].data() + (size_t)s * kAudioSector, 0, trackNo, 1,
                          (unsigned long long)s, abs, 0, sector.data());
            if (std::fwrite(sector.data(), 1, sector.size(), out) != sector.size())
                writeError = true;
            ++abs; ++emitted;
        }

        if (t + 1 < tracks.size()) {
            /* The gap belongs to the next track's index 0. */
            for (unsigned s = 0; s < gap && !writeError; ++s) {
                MakeRawSector(audio.data(), 0, trackNo + 1, 0, 0, abs,
                              (s % 2) ? 0 : 1, sector.data());
                if (std::fwrite(sector.data(), 1, sector.size(), out) != sector.size())
                    writeError = true;
                ++abs; ++emitted;
            }
        } else {
            /* Lead-out: control 0xAA. */
            for (unsigned s = 0; s < leadOut && !writeError; ++s) {
                MakeRawSector(audio.data(), 0xAA, trackNo, 1,
                              (unsigned long long)(sectors + s), abs, 1, sector.data());
                if (std::fwrite(sector.data(), 1, sector.size(), out) != sector.size())
                    writeError = true;
                ++abs; ++emitted;
            }
        }

        char msg[96];
        std::snprintf(msg, sizeof(msg), "Encoded track %d of %d", (int)t + 1, (int)tracks.size());
        NotifyProgressBurn(10 + (int)(70 * (t + 1) / tracks.size()), msg);
    }

    const int closeErr = (std::fclose(out) != 0);
    if (writeError || closeErr) {
        std::remove(outputPath.c_str());
        SetBurnLastError("RAW image write failed (disk full or I/O error)");
        return false;
    }
    char done[128];
    std::snprintf(done, sizeof(done), "RAW image written: %llu sectors x 2448 bytes",
                  (unsigned long long)emitted);
    SetBurnLastError(done);
    NotifyProgressBurn(100, "RAW image creation completed");
    return true;
}

bool BurnRawImage(const std::string& imagePath, const std::string& devicePath,
                  int speed, int verifyLevel) {
    if (verifyLevel != BURN_VERIFY_NONE) {
        SetBurnLastError("Verification is not available on the RAW write path; "
                         "use Burn_VerifyWrite() on a data disc instead");
    }
    NotifyProgressBurn(5, "Preparing RAW write...");

    Drive drive;
    if (!drive.open(devicePath)) return false;
    struct burn_drive* d = drive.dev();
    PumpDrive(d, "Reading disc state", false);
    if (burn_disc_get_status(d) == BURN_DISC_EMPTY) {
        SetBurnLastError("No disc in " + devicePath);
        return false;
    }
    if (speed > 0) burn_drive_set_speed(d, 0, WriteKs(speed, "CD-R"));

    struct burn_disc* disc = burn_disc_create();
    struct burn_session* session = burn_session_create();
    burn_disc_add_session(disc, session, BURN_POS_END);

    burn::burn_source* src = burn_file_source_new(imagePath.c_str(), nullptr);
    if (!src) {
        SetBurnLastError("Failed to create a burn source from " + imagePath);
        burn_session_free(session);
        burn_disc_free(disc);
        return false;
    }
    struct burn_track* track = burn_track_create();
    /* RAW96R input: 2352 audio bytes plus 96 bytes of raw subcode per sector,
     * which is exactly the layout CreateRawImage() produces. */
    burn_track_define_data(track, 0, 0, 1, BURN_AUDIO | BURN_SUBCODE_R96);
    burn_track_set_source(track, src);
    burn_session_add_track(session, track, BURN_POS_END);

    struct burn_write_opts* opts = burn_write_opts_new(d);
    burn_write_opts_set_perform_opc(opts, 0);
    /* Raw writing must be requested explicitly; auto write type would pick a
     * cooked mode and reject the 2448-byte layout. */
    if (burn_write_opts_set_write_type(opts, BURN_WRITE_RAW, BURN_BLOCK_RAW96R) <= 0) {
        SetBurnLastError("The drive does not support RAW writing with 96-byte subcode");
        burn_write_opts_free(opts);
        burn_track_free(track);
        burn_session_free(session);
        burn_disc_free(disc);
        return false;
    }

    NotifyProgressBurn(20, "Writing RAW data...");
    CancelCtx ctx;
    CancelArm arm(ctx);
    arm.bind(d, "Writing RAW");
    burn_disc_write(opts, disc);
    PumpDrive(d, "Writing RAW", true);

    const bool wroteOk = (burn_drive_wrote_well(d) > 0);
    bool cancelled = IsBurnCancelled();
    if (cancelled && wroteOk) cancelled = false;

    burn_write_opts_free(opts);
    burn_track_free(track);
    burn_source_free(src);
    burn_session_free(session);
    burn_disc_free(disc);

    if (cancelled) {
        SetBurnLastError("RAW burn cancelled by caller");
        return false;
    }
    if (!wroteOk) {
        SetBurnLastError("RAW write failed: the drive reported an error for " + imagePath);
        return false;
    }
    NotifyProgressBurn(100, "Burn completed");
    return true;
}


} /* namespace burnunix */

#endif /* !_WIN32 && !__APPLE__ */
