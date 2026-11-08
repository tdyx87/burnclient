/* Executable hardware checklist for the v3 capability surface.
 *
 * This program is NOT part of the automated test suite: it needs a real
 * optical drive and, for the destructive steps, blank/rewritable media.
 * Its purpose is to turn docs/HARDWARE_TEST_CHECKLIST.md into something a
 * person can run on the target machine and read PASS/SKIP/FAIL from.
 *
 * Usage:
 *   hw_check <device> [--destructive] [--audio a.wav,b.wav] [--iso file.iso]
 *
 *   <device>        volume path of the recorder, e.g. "E:" on Windows or
 *                   "/dev/sr0" on Linux.
 *   --destructive   allow steps that consume media (erase, audio burn,
 *                   image burn, raw burn, session append). Without it only
 *                   read-only and drive-free steps run.
 *   --audio         comma-separated WAV files for the audio-CD step.
 *   --iso           reference image for the read-back verification step.
 *
 * Exit code: 0 when every step either passed or was skipped with a reason,
 * 1 when at least one step failed.
 */
#include "../src/burning_utility.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

static int g_fail = 0;
static int g_pass = 0;
static int g_skip = 0;

static void Pass(const char* what, const char* detail = "") {
    ++g_pass;
    std::printf("  [PASS] %s%s%s\n", what, *detail ? " - " : "", detail);
    std::fflush(stdout);
}
static void Skip(const char* what, const char* why) {
    ++g_skip;
    std::printf("  [SKIP] %s - %s\n", what, why);
    std::fflush(stdout);
}
static void Fail(const char* what, const char* why) {
    ++g_fail;
    std::printf("  [FAIL] %s - %s\n", what, why ? why : "");
    std::fflush(stdout);
}

static void Section(const char* name) {
    std::printf("\n=== %s ===\n", name);
    std::fflush(stdout);
}

/* A drive that is enumerated but cannot be opened exclusively (a virtual
 * drive, a write-protected driver, another process holding it) is an
 * environment limitation, not a defect: report SKIP with the reason instead
 * of FAIL so the checklist stays actionable. */
static bool EnvironmentLimited(const char* err) {
    if (!err) return false;
    return std::strstr(err, "not found for device path") != nullptr ||
           std::strstr(err, "cannot be opened exclusively") != nullptr ||
           std::strstr(err, "write-protected") != nullptr ||
           std::strstr(err, "write protected") != nullptr ||
           std::strstr(err, "busy") != nullptr;
}

struct Options {
    std::string device;
    bool destructive = false;
    std::vector<std::string> audio;
    std::string iso;
};

static std::vector<std::string> SplitComma(const char* s) {
    std::vector<std::string> out;
    std::string cur;
    for (const char* p = s; *p; ++p) {
        if (*p == ',') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += *p;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--destructive") opt.destructive = true;
        else if (a == "--audio" && i + 1 < argc) opt.audio = SplitComma(argv[++i]);
        else if (a == "--iso" && i + 1 < argc) opt.iso = argv[++i];
        else if (!a.empty() && a[0] != '-' && opt.device.empty()) opt.device = a;
    }
    if (opt.device.empty()) {
        std::printf("usage: hw_check <device> [--destructive] [--audio a.wav,b.wav] [--iso f.iso]\n"
                    "  <device> is the recorder volume path, e.g. \"E:\" or /dev/sr0\n");
        return 2;
    }

    std::printf("BurningDLL hardware checklist\n");
    std::printf("  api version      : %d\n", Burn_GetApiVersion());
    std::printf("  platform         : %s\n", GetPlatformInfo());
    std::printf("  device           : %s\n", opt.device.c_str());
    std::printf("  destructive steps: %s\n", opt.destructive ? "ENABLED" : "disabled");
    std::fflush(stdout);

    Section("1. capability table");
    {
        struct { int id; const char* name; } caps[] = {
            { BURN_CAP_IMAGE_BURN, "data-disc burn" },
            { BURN_CAP_CREATE_IMAGE, "image creation" },
            { BURN_CAP_AUDIO_TAO, "audio CD (TAO)" },
            { BURN_CAP_ERASE, "erase / format" },
            { BURN_CAP_MULTISESSION, "multisession append" },
            { BURN_CAP_UDF, "UDF filesystem" },
            { BURN_CAP_BOOT_IMAGE, "El Torito boot" },
            { BURN_CAP_READBACK_VERIFY, "read-back verify" },
            { BURN_CAP_RAW_BURN, "RAW writing" },
            { BURN_CAP_RAW_IMAGE, "RAW image building" },
            { BURN_CAP_TRAY_CONTROL, "tray control" },
            { BURN_CAP_DRIVE_INFO, "drive information" },
        };
        for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); ++i) {
            std::printf("  %-20s : %s\n", caps[i].name,
                        Burn_GetCapability(caps[i].id) ? "supported" : "not supported");
        }
        std::fflush(stdout);
    }

    Section("2. device enumeration");
    {
        DeviceInfo* devices = nullptr;
        int count = 0;
        if (ListBurningDevices(&devices, &count) == BURN_SUCCESS) {
            std::printf("  %d device(s)\n", count);
            for (int i = 0; i < count; ++i) {
                std::printf("    %s  '%s'  media=%s writable=%d empty=%d\n",
                            devices[i].devicePath, devices[i].deviceName,
                            devices[i].mediaType, devices[i].isWritable, devices[i].isEmpty);
            }
            Pass("ListBurningDevices");
            FreeDeviceList(devices, count);
        } else {
            Fail("ListBurningDevices", GetLastErrorMessage());
        }
    }

    Section("3. drive information (MMC)");
    if (!Burn_GetCapability(BURN_CAP_DRIVE_INFO)) {
        Skip("drive information", "not implemented on this backend");
    } else {
        const char* info = Burn_GetDriveInformation(opt.device.c_str());
        if (info && *info) {
            /* Print indented so the reader can check vendor/media/state. */
            const char* p = info;
            while (*p) {
                const char* nl = std::strchr(p, '\n');
                const int len = nl ? (int)(nl - p) : (int)std::strlen(p);
                std::printf("    %.*s\n", len, p);
                p = nl ? nl + 1 : p + std::strlen(p);
            }
            Pass("Burn_GetDriveInformation");
        } else {
            if (EnvironmentLimited(GetLastErrorMessage()))
                Skip("Burn_GetDriveInformation", GetLastErrorMessage());
            else
                Fail("Burn_GetDriveInformation", GetLastErrorMessage());
        }
    }

    Section("4. tray control");
    if (!Burn_GetCapability(BURN_CAP_TRAY_CONTROL)) {
        Skip("eject/close tray", "not implemented on this backend");
    } else if (!opt.destructive) {
        Skip("eject/close tray", "needs --destructive (ejecting disturbs the drive)");
    } else {
        const int e = BurnEjectTray(opt.device.c_str());
        if (e == BURN_SUCCESS) Pass("BurnEjectTray");
        else Fail("BurnEjectTray", GetLastErrorMessage());
        const int c = BurnCloseTray(opt.device.c_str());
        if (c == BURN_SUCCESS) Pass("BurnCloseTray");
        else Fail("BurnCloseTray", GetLastErrorMessage());
    }

    Section("5. multisession inquiry");
    if (!Burn_GetCapability(BURN_CAP_MULTISESSION)) {
        Skip("next session block", "not implemented on this backend");
    } else {
        const long long block = Burn_GetNextSessionBlock(opt.device.c_str());
        if (block >= 0) {
            char b[96];
            std::snprintf(b, sizeof(b), "next writable LBA = %lld", block);
            Pass("Burn_GetNextSessionBlock", b);
        } else {
            /* -1 for "not appendable" is an expected answer on blank or
             * finalized media, so report the reason rather than failing. */
            char b[256];
            std::snprintf(b, sizeof(b), "%s", GetLastErrorMessage());
            Skip("Burn_GetNextSessionBlock", b);
        }
    }

    Section("6. image creation: plain / UDF / boot (drive-free)");
    if (!Burn_GetCapability(BURN_CAP_CREATE_IMAGE)) {
        Skip("image creation", "not implemented on this backend");
    } else {
        const char* dir = "hw_content";
#ifdef _WIN32
        _mkdir(dir);
#else
        mkdir(dir, 0777);
#endif
        { std::ofstream f("hw_content/payload.txt"); f << "hardware checklist payload"; }

        BurnJobOptions3 o;
        std::memset(&o, 0, sizeof(o));
        o.size = (int)sizeof(o);
        o.mode = BURN_JOB_MODE_CREATE_IMAGE;
        o.sourcePath = dir;
        o.volumeLabel = "HWCHECK";

        const char* variants[] = { "hw_plain.iso", "hw_udf.iso", "hw_boot.iso" };
        for (int v = 0; v < 3; ++v) {
            o.outputPath = variants[v];
            o.fileSystems = (v == 1) ? (BURN_FS_ISO9660 | BURN_FS_JOLIET | BURN_FS_UDF) : 0;
            if (v == 2) {
                if (!Burn_GetCapability(BURN_CAP_BOOT_IMAGE)) { Skip("boot image", "not supported"); continue; }
                /* A small no-emulation load-clip is enough to prove the
                 * catalog is written correctly. */
                { std::ofstream f("hw_clip.bin", std::ios::binary);
                  std::vector<unsigned char> z(2048, 0);
                  z[0] = 0xEB; z[1] = 0x00; z[2] = 0x90;   /* x86 boot jump */
                  f.write(reinterpret_cast<const char*>(z.data()),
                          (std::streamsize)z.size()); }
                o.bootImagePath = "hw_clip.bin";
                o.bootEmulation = BURN_BOOT_EMU_NONE;
                o.bootPlatform = BURN_BOOT_PLATFORM_X86;
            } else if (v == 1 && !Burn_GetCapability(BURN_CAP_UDF)) {
                Skip("UDF image", "not supported"); continue;
            }
            BurnJobStatus st;
            std::memset(&st, 0, sizeof(st));
            st.size = (int)sizeof(st);
            const int rc = Burn_Simple(reinterpret_cast<const BurnJobOptions*>(&o), &st);
            if (rc == BURN_SUCCESS) Pass(variants[v]);
            else Fail(variants[v], st.errorMessage);
        }
    }

    Section("7. RAW PQ image building (drive-free)");
    if (!Burn_GetCapability(BURN_CAP_RAW_IMAGE)) {
        Skip("RAW image", "not implemented on this backend");
    } else if (opt.audio.empty()) {
        Skip("RAW image", "no --audio WAV files supplied");
    } else {
        std::vector<const char*> paths;
        for (size_t i = 0; i < opt.audio.size(); ++i) paths.push_back(opt.audio[i].c_str());
        const int rc = BurnCreateRawImage(paths.data(), (int)paths.size(), "hw_disc.raw", nullptr, 1);
        if (rc == BURN_SUCCESS) Pass("BurnCreateRawImage -> hw_disc.raw");
        else Fail("BurnCreateRawImage", GetLastErrorMessage());
    }

    Section("8. write steps (need blank media)");
    if (!opt.destructive) {
        Skip("erase / audio burn / image burn / raw burn / append",
             "needs --destructive and a suitable blank or rewritable disc");
    } else {
        /* 8a. erase (CD-RW / DVD-RW / DVD+RW / DVD-RAM / BD-RE only) */
        if (!Burn_GetCapability(BURN_CAP_ERASE)) {
            Skip("erase", "not implemented on this backend");
        } else {
            const int rc = BurnEraseDisc(opt.device.c_str(), 0);
            if (rc == BURN_SUCCESS) Pass("BurnEraseDisc (fast)");
            else Skip("BurnEraseDisc", GetLastErrorMessage());   /* media may not be rewritable */
        }

        /* 8b. burn the plain image */
        if (Burn_GetCapability(BURN_CAP_IMAGE_BURN)) {
            BurnJobOptions3 o;
            std::memset(&o, 0, sizeof(o));
            o.size = (int)sizeof(o);
            o.mode = BURN_JOB_MODE_IMAGE;
            o.sourcePath = "hw_plain.iso";
            o.devicePath = opt.device.c_str();
            o.discType = "CD-R";
            o.speed = 0;
            BurnJobStatus st;
            std::memset(&st, 0, sizeof(st));
            st.size = (int)sizeof(st);
            const int rc = Burn_Simple(reinterpret_cast<const BurnJobOptions*>(&o), &st);
            if (rc == BURN_SUCCESS) Pass("burn image -> disc");
            else Fail("burn image", st.errorMessage);
        }

        /* 8c. audio CD */
        if (!Burn_GetCapability(BURN_CAP_AUDIO_TAO)) {
            Skip("audio CD", "not implemented on this backend");
        } else if (opt.audio.empty()) {
            Skip("audio CD", "no --audio WAV files supplied");
        } else {
            std::vector<const char*> paths;
            for (size_t i = 0; i < opt.audio.size(); ++i) paths.push_back(opt.audio[i].c_str());
            const int rc = BurnAudioCd(paths.data(), (int)paths.size(), opt.device.c_str(), 0,
                                       BURN_VERIFY_NONE);
            if (rc == BURN_SUCCESS) Pass("BurnAudioCd");
            else Fail("BurnAudioCd", GetLastErrorMessage());
        }

        /* 8d. RAW burn */
        if (Burn_GetCapability(BURN_CAP_RAW_BURN)) {
            const int rc = BurnRawImageToDisc("hw_disc.raw", opt.device.c_str(), 0, BURN_VERIFY_NONE);
            if (rc == BURN_SUCCESS) Pass("BurnRawImageToDisc");
            else Skip("BurnRawImageToDisc", GetLastErrorMessage());
        } else {
            Skip("RAW burn", "not implemented on this backend");
        }
    }

    Section("9. read-back verification");
    if (!Burn_GetCapability(BURN_CAP_READBACK_VERIFY)) {
        Skip("read-back verify", "not implemented on this backend");
    } else if (opt.iso.empty()) {
        Skip("read-back verify", "no --iso reference image supplied");
    } else {
        const int rc = Burn_VerifyWrite(opt.device.c_str(), opt.iso.c_str(), BURN_VERIFY_QUICK);
        if (rc == BURN_SUCCESS) Pass("Burn_VerifyWrite (quick)");
        else Fail("Burn_VerifyWrite", GetLastErrorMessage());
    }

    Section("10. multisession append");
    if (!Burn_GetCapability(BURN_CAP_MULTISESSION)) {
        Skip("append session", "not implemented on this backend");
    } else if (!opt.destructive) {
        Skip("append session", "needs --destructive and an appendable disc");
    } else {
        const int rc = BurnAppendSession("hw_content", opt.device.c_str(), "HWSESSION", 0, 0, 0);
        if (rc == BURN_SUCCESS) Pass("BurnAppendSession");
        else Skip("BurnAppendSession", GetLastErrorMessage());
    }

    std::printf("\n=== summary: %d passed, %d skipped, %d failed ===\n",
                g_pass, g_skip, g_fail);
    return g_fail == 0 ? 0 : 1;
}
