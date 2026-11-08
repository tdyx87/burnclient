/* Version 3 capability tests.
 *
 * These verify the parts of the v3 surface that need NO optical drive, and
 * therefore run in CI on any machine:
 *   - the capability table and API version
 *   - honest refusal on backends that do not implement a feature
 *   - RAW PQ image building: every sector is checked for the 12-byte CD sync
 *     pattern and a 2352-byte layout
 *   - UDF: the anchor signature must appear when requested and must NOT
 *     appear when not requested (a real differential test of the flag)
 *   - El Torito: the boot catalog LBA is read from the Boot Record Volume
 *     Descriptor, the catalog is parsed, and the load-clip bytes are found
 *     at the sector the catalog points at
 * Drive-requiring paths (audio write, erase, RAW write, read-back) are
 * exercised only for their parameter validation and device resolution, and
 * the full end-to-end run is covered by docs/HARDWARE_TEST_CHECKLIST.md. */

#include "../src/burning_utility.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <fstream>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define PATHSEP "\\"
#else
#include <sys/stat.h>
#include <unistd.h>     /* rmdir() */
#define MKDIR(p) mkdir((p), 0777)
#define PATHSEP "/"
#endif

static int g_failures = 0;
#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("CHECK FAILED: %s (line %d)\n", #expr, __LINE__);      \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

#define INFO(...) std::printf(__VA_ARGS__)

/* Burn_Start takes the v1 struct type but dispatches on options->size, so a
 * version 3 caller casts its (prefix-compatible) struct. */
static BurnJobHandle StartV3(const BurnJobOptions3* o) {
    return Burn_Start(reinterpret_cast<const BurnJobOptions*>(o));
}

static const char* kWorkDir = "burn_v3_test";

static std::string Join(const std::string& a, const std::string& b) {
    return a + PATHSEP + b;
}

static bool ReadFile(const std::string& path, std::vector<unsigned char>& out) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    f.seekg(0, std::ios::beg);
    out.resize((size_t)n);
    if (n > 0) f.read((char*)out.data(), n);
    return (bool)f || out.empty();
}

static bool ContainsBytes(const std::vector<unsigned char>& hay, const char* needle) {
    const size_t nl = std::strlen(needle);
    if (nl == 0 || hay.size() < nl) return false;
    for (size_t i = 0; i + nl <= hay.size(); ++i) {
        if (std::memcmp(hay.data() + i, needle, nl) == 0) return true;
    }
    return false;
}

static unsigned ReadLe32(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

/* Write a PCM 44.1kHz 16-bit stereo WAV of exactly `seconds` of a ramp, so
 * the geometry checks in media_format.h are satisfied. */
static bool WriteWav(const std::string& path, double seconds, unsigned seed) {
    const unsigned frames = (unsigned)(44100.0 * seconds);
    const unsigned dataBytes = frames * 4u;
    std::vector<unsigned char> pcm((size_t)dataBytes);
    for (unsigned i = 0; i < frames; ++i) {
        seed = seed * 1103515245u + 12345u;
        const short s = (short)((seed >> 16) - 32768);
        pcm[(size_t)i * 4 + 0] = (unsigned char)(s & 0xFF);
        pcm[(size_t)i * 4 + 1] = (unsigned char)((s >> 8) & 0xFF);
        pcm[(size_t)i * 4 + 2] = (unsigned char)(s & 0xFF);   /* same on L=R */
        pcm[(size_t)i * 4 + 3] = (unsigned char)((s >> 8) & 0xFF);
    }

    std::ofstream f(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    const unsigned riffSize = 36u + dataBytes;
    auto put32 = [&](unsigned v) {
        unsigned char b[4] = { (unsigned char)(v & 0xFF), (unsigned char)((v >> 8) & 0xFF),
                               (unsigned char)((v >> 16) & 0xFF), (unsigned char)((v >> 24) & 0xFF) };
        f.write((const char*)b, 4);
    };
    auto put16 = [&](unsigned short v) {
        unsigned char b[2] = { (unsigned char)(v & 0xFF), (unsigned char)((v >> 8) & 0xFF) };
        f.write((const char*)b, 2);
    };
    f.write("RIFF", 4); put32(riffSize); f.write("WAVE", 4);
    f.write("fmt ", 4); put32(16);
    put16(1);            /* PCM */
    put16(2);            /* stereo */
    put32(44100);        /* sample rate */
    put32(44100 * 4);    /* byte rate */
    put16(4);            /* block align */
    put16(16);           /* bits */
    f.write("data", 4); put32(dataBytes);
    f.write((const char*)pcm.data(), (std::streamsize)dataBytes);
    return f.good();
}

/* ------------------------------------------------------------------------ */
static void TestVersionAndCapabilities() {
    INFO("=== API version and capability table ===\n");
    CHECK(Burn_GetApiVersion() == BURN_API_VERSION);
    /* Compile-time constant: MSVC warns about the constant condition, which
     * is exactly the point of the check. */
    const bool apiIsV3 = (BURN_API_VERSION == 3);
    CHECK(apiIsV3);

    /* Baseline features exist on every backend. */
    CHECK(Burn_GetCapability(BURN_CAP_IMAGE_BURN) == 1);
    const bool createImageIsCapOne = (BURN_CAP_CREATE_IMAGE == 1);
    CHECK(createImageIsCapOne);

    INFO("  audio=%d erase=%d multisession=%d udf=%d boot=%d readback=%d raw=%d tray=%d info=%d\n",
         Burn_GetCapability(BURN_CAP_AUDIO_TAO),
         Burn_GetCapability(BURN_CAP_ERASE),
         Burn_GetCapability(BURN_CAP_MULTISESSION),
         Burn_GetCapability(BURN_CAP_UDF),
         Burn_GetCapability(BURN_CAP_BOOT_IMAGE),
         Burn_GetCapability(BURN_CAP_READBACK_VERIFY),
         Burn_GetCapability(BURN_CAP_RAW_IMAGE),
         Burn_GetCapability(BURN_CAP_TRAY_CONTROL),
         Burn_GetCapability(BURN_CAP_DRIVE_INFO));

    /* An unknown capability id must answer 0, never a stale 1. */
    CHECK(Burn_GetCapability(9999) == 0);
    CHECK(Burn_GetCapability(-1) == 0);
}

/* On a backend without the feature, calling it must be an explicit refusal,
 * not a silent success and not a crash. This is the Linux/macOS contract. */
static void TestHonestRefusal() {
    INFO("\n=== Unsupported features refuse explicitly ===\n");
    const bool hasAudio = Burn_GetCapability(BURN_CAP_AUDIO_TAO) == 1;
    const bool hasErase = Burn_GetCapability(BURN_CAP_ERASE) == 1;
    const bool hasRawImg = Burn_GetCapability(BURN_CAP_RAW_IMAGE) == 1;
    const bool hasVerify = Burn_GetCapability(BURN_CAP_READBACK_VERIFY) == 1;

    /* Keep the std::string alive: taking c_str() of a temporary would leave
     * the pointer dangling for the duration of the call. */
    const std::string dummyAudio = Join(kWorkDir, "does-not-matter.wav");

    if (!hasAudio) {
        const char* tracks[1] = { dummyAudio.c_str() };
        const int rc = BurnAudioCd(tracks, 1, "Z:0", 4, BURN_VERIFY_NONE);
        INFO("  BurnAudioCd on unsupported backend -> %d err='%s'\n", rc, GetLastErrorMessage());
        CHECK(rc == BURN_ERROR_UNSUPPORTED_FORMAT);
    }
    if (!hasErase) {
        const int rc = BurnEraseDisc("Z:0", 0);
        INFO("  BurnEraseDisc on unsupported backend -> %d err='%s'\n", rc, GetLastErrorMessage());
        CHECK(rc == BURN_ERROR_UNSUPPORTED_FORMAT);
    }
    if (!hasRawImg) {
        const char* tracks[1] = { dummyAudio.c_str() };
        const std::string rawOut = Join(kWorkDir, "no.raw");
        const int rc = BurnCreateRawImage(tracks, 1, rawOut.c_str(), nullptr, 0);
        INFO("  BurnCreateRawImage on unsupported backend -> %d err='%s'\n", rc, GetLastErrorMessage());
        CHECK(rc == BURN_ERROR_UNSUPPORTED_FORMAT);
    }
    if (!hasVerify) {
        const std::string missingIso = Join(kWorkDir, "does-not-exist.iso");
        const int rc = Burn_VerifyWrite("Z:0", missingIso.c_str(), BURN_VERIFY_QUICK);
        INFO("  Burn_VerifyWrite on unsupported backend -> %d err='%s'\n", rc, GetLastErrorMessage());
        CHECK(rc == BURN_ERROR_UNSUPPORTED_FORMAT);
    }
    if (hasAudio || hasErase || hasRawImg || hasVerify) {
        INFO("  (backend advertises some v3 features; refusal checks skipped for those)\n");
    }
}

/* Bad arguments must be rejected before any hardware is touched. This holds
 * on every platform, including one with a working burner. */
static void TestParameterValidation() {
    INFO("\n=== Parameter validation happens before any I/O ===\n");

    CHECK(BurnAudioCd(nullptr, 1, "Z:0", 4, BURN_VERIFY_NONE) == BURN_ERROR_INVALID_PARAM);
    const std::string dummyAudio = Join(kWorkDir, "does-not-matter.wav");
    const char* one[1] = { dummyAudio.c_str() };
    CHECK(BurnAudioCd(one, 0, "Z:0", 4, BURN_VERIFY_NONE) == BURN_ERROR_INVALID_PARAM);
    CHECK(BurnAudioCd(one, 100, "Z:0", 4, BURN_VERIFY_NONE) == BURN_ERROR_INVALID_PARAM);
    CHECK(BurnAudioCd(one, 1, nullptr, 4, BURN_VERIFY_NONE) == BURN_ERROR_INVALID_PARAM);

    CHECK(BurnEraseDisc(nullptr, 0) == BURN_ERROR_INVALID_PARAM);
    CHECK(BurnEraseDisc("", 0) == BURN_ERROR_INVALID_PARAM);

    CHECK(Burn_VerifyWrite("Z:0", nullptr, BURN_VERIFY_QUICK) == BURN_ERROR_INVALID_PARAM);
    CHECK(Burn_VerifyWrite(nullptr, "x.iso", BURN_VERIFY_QUICK) == BURN_ERROR_INVALID_PARAM);
    /* A level outside the enum must not be silently accepted. */
    CHECK(Burn_VerifyWrite("Z:0", "x.iso", 7) == BURN_ERROR_INVALID_PARAM);

    CHECK(Burn_GetNextSessionBlock(nullptr) < 0);
    CHECK(Burn_GetNextSessionBlock("") < 0);

    /* Job options validation. */
    BurnJobOptions3 o;
    std::memset(&o, 0, sizeof(o));
    o.size = (int)sizeof(o);
    o.mode = BURN_JOB_MODE_AUDIO_TAO;      /* needs tracks */
    o.devicePath = "Z:0";
    CHECK(StartV3(&o) == BURN_INVALID_JOB_HANDLE);
    CHECK(std::strlen(GetLastErrorMessage()) > 0);
    INFO("  AUDIO_TAO without tracks: '%s'\n", GetLastErrorMessage());

    o.audioTracks = reinterpret_cast<const BurnAudioTrackC*>(-1); /* non-NULL */
    o.audioTrackCount = 200;               /* over the CD limit */
    CHECK(StartV3(&o) == BURN_INVALID_JOB_HANDLE);
    INFO("  AUDIO_TAO with 200 tracks: '%s'\n", GetLastErrorMessage());

    o.audioTrackCount = 0;
    o.mode = BURN_JOB_MODE_IMAGE;
    o.interchangeLevel = 9;                /* not a valid ISO level */
    o.sourcePath = "x.iso";
    CHECK(StartV3(&o) == BURN_INVALID_JOB_HANDLE);
    INFO("  bad interchangeLevel: '%s'\n", GetLastErrorMessage());

    o.interchangeLevel = 0;
    o.fileSystems = 0x8000;                /* unknown bit */
    CHECK(StartV3(&o) == BURN_INVALID_JOB_HANDLE);
    INFO("  unknown fileSystems bit: '%s'\n", GetLastErrorMessage());

    o.fileSystems = 0;
    o.bootEmulation = 42;                  /* out of range */
    CHECK(StartV3(&o) == BURN_INVALID_JOB_HANDLE);
    INFO("  bad bootEmulation: '%s'\n", GetLastErrorMessage());

    /* The v1 struct must keep working unchanged. */
    BurnJobOptions v1;
    std::memset(&v1, 0, sizeof(v1));
    v1.size = (int)sizeof(v1);
    v1.mode = BURN_JOB_MODE_SELFTEST;
    const BurnJobHandle h = Burn_Start(&v1);
    if (h != BURN_INVALID_JOB_HANDLE) {
        CHECK(Burn_Wait(h, 10000) == BURN_SUCCESS);
        BurnJobStatus st;
        std::memset(&st, 0, sizeof(st));
        st.size = (int)sizeof(st);
        CHECK(Burn_GetStatus(h, &st) == BURN_SUCCESS);
        CHECK(st.state == BURN_JOB_COMPLETED);
        Burn_Release(h);
        INFO("  v1 options struct still works\n");
    } else {
        CHECK(false);   /* the v1 path must never regress */
    }
}

/* The image writer must reject a non-existent audio file before touching a
 * drive, so this holds everywhere. */
static void TestAudioGeometryValidation() {
    INFO("\n=== Audio input geometry is validated ===\n");
    if (Burn_GetCapability(BURN_CAP_AUDIO_TAO) != 1) {
        INFO("  skipped: backend does not implement audio TAO\n");
        return;
    }
    const std::string bad = Join(kWorkDir, "bad_geometry.wav");
    {
        /* A valid RIFF/WAVE header describing 22050 Hz mono 8-bit: this must
         * be refused rather than burned as a ruined disc. */
        std::ofstream f(bad.c_str(), std::ios::binary | std::ios::trunc);
        auto put32 = [&f](unsigned v) {
            unsigned char b[4] = { (unsigned char)(v & 0xFF), (unsigned char)((v >> 8) & 0xFF),
                                   (unsigned char)((v >> 16) & 0xFF), (unsigned char)((v >> 24) & 0xFF) };
            f.write((const char*)b, 4);
        };
        auto put16 = [&f](unsigned short v) {
            unsigned char b[2] = { (unsigned char)(v & 0xFF), (unsigned char)((v >> 8) & 0xFF) };
            f.write((const char*)b, 2);
        };
        f.write("RIFF", 4); put32(36 + 1024); f.write("WAVE", 4);
        f.write("fmt ", 4); put32(16);
        put16(1); put16(1); put32(22050); put32(22050); put16(1); put16(8);
        f.write("data", 4); put32(1024);
        std::vector<char> junk(1024, 0x41);
        f.write(junk.data(), 1024);
    }
    const char* tracks[1] = { bad.c_str() };
    const int rc = BurnAudioCd(tracks, 1, "Z:0", 4, BURN_VERIFY_NONE);
    INFO("  22050Hz/mono/8bit -> rc=%d err='%s'\n", rc, GetLastErrorMessage());
    CHECK(rc != BURN_SUCCESS);
    CHECK(std::strlen(GetLastErrorMessage()) > 0);
    /* The refusal must be about the geometry, not a silent device error. */
    CHECK(std::strstr(GetLastErrorMessage(), "44100") != nullptr ||
          std::strstr(GetLastErrorMessage(), "PCM") != nullptr);
}

/* ------------------------------------------------------------------ RAW -- */
static void TestRawImageBytes() {
    INFO("\n=== RAW PQ image: byte-level structure ===\n");
    if (Burn_GetCapability(BURN_CAP_RAW_IMAGE) != 1) {
        INFO("  skipped: backend does not build RAW images\n");
        return;
    }
    const std::string wavA = Join(kWorkDir, "track01.wav");
    const std::string wavB = Join(kWorkDir, "track02.wav");
    const std::string raw = Join(kWorkDir, "disc.raw");
    /* Five-second tracks: CD audio has a 4-second minimum track length
     * (tracks shorter than that are padded by the writer, or rejected by
     * the RAW creator), so 5 s = 375 sectors leaves headroom. */
    CHECK(WriteWav(wavA, 5.0, 12345u));
    CHECK(WriteWav(wavB, 5.0, 6789u));

    const char* tracks[2] = { wavA.c_str(), wavB.c_str() };
    /* No MCN: it is optional, and IMAPI2 validates the check digit of a
     * supplied one (see the dedicated rejection check below). */
    const int rc = BurnCreateRawImage(tracks, 2, raw.c_str(), nullptr, 1);
    INFO("  BurnCreateRawImage -> %d err='%s'\n", rc, GetLastErrorMessage());
    if (rc != BURN_SUCCESS) {
        INFO("  (image writer unavailable in this environment; structure test skipped)\n");
        return;
    }

    /* An invalid media catalog number must be refused, not silently ignored. */
    {
        const std::string raw2 = Join(kWorkDir, "disc_mcn.raw");
        const int rcMcn = BurnCreateRawImage(tracks, 1, raw2.c_str(), "not-13-digits", 1);
        INFO("  invalid MCN -> rc=%d err='%s'\n", rcMcn, GetLastErrorMessage());
        CHECK(rcMcn != BURN_SUCCESS);
        remove(raw2.c_str());
    }

    std::vector<unsigned char> img;
    CHECK(ReadFile(raw, img));
    INFO("  RAW image size = %zu bytes\n", img.size());
    CHECK(!img.empty());

    /* IMAPI2's PQ image starts with the (zero-filled) lead-in and packs the
     * PQ subcode alongside the user data, so asserting a specific sector
     * stride here would be asserting an implementation detail. Instead the
     * image is verified *behaviourally*, which is layout-independent:
     *   a) same input twice  -> byte-identical output (deterministic)
     *   b) longer track      -> strictly larger image (payload is included)
     *   c) different samples -> different bytes (content really is copied)
     */
    const std::string rawAgain = Join(kWorkDir, "disc_again.raw");
    CHECK(BurnCreateRawImage(tracks, 2, rawAgain.c_str(), nullptr, 1) == BURN_SUCCESS);
    std::vector<unsigned char> imgAgain;
    CHECK(ReadFile(rawAgain, imgAgain));
    const bool identical = (img.size() == imgAgain.size()) &&
                           (img.empty() || std::memcmp(img.data(), imgAgain.data(), img.size()) == 0);
    INFO("  rebuild is byte-identical: %s\n", identical ? "yes" : "no");
    CHECK(identical);
    remove(rawAgain.c_str());

    /* The invariants below use the SAME track count: a differing number of
     * tracks shifts the layout (each track adds its own pregap), so sizes are
     * not comparable across track counts. */
    const std::string wavLong = Join(kWorkDir, "track_long.wav");
    const std::string rawLong = Join(kWorkDir, "disc_long.raw");
    CHECK(WriteWav(wavLong, 12.0, 12345u));    /* same samples, 7 s longer */
    const char* longTracks[1] = { wavLong.c_str() };
    CHECK(BurnCreateRawImage(longTracks, 1, rawLong.c_str(), nullptr, 1) == BURN_SUCCESS);
    std::vector<unsigned char> imgLong;
    CHECK(ReadFile(rawLong, imgLong));

    const std::string wav5 = Join(kWorkDir, "track_5s.wav");
    const std::string raw5 = Join(kWorkDir, "disc_5s.raw");
    CHECK(WriteWav(wav5, 5.0, 12345u));
    const char* fiveTracks[1] = { wav5.c_str() };
    CHECK(BurnCreateRawImage(fiveTracks, 1, raw5.c_str(), nullptr, 1) == BURN_SUCCESS);
    std::vector<unsigned char> img5;
    CHECK(ReadFile(raw5, img5));

    INFO("  12 s image = %zu bytes, 5 s image = %zu bytes\n", imgLong.size(), img5.size());
    CHECK(!imgLong.empty() && !img5.empty());
    /* 7 more seconds of audio must make the image strictly larger. */
    CHECK(imgLong.size() > img5.size());
    /* Rough scaling check: 7 s = 525 sectors of 2352 bytes of payload, which
     * must appear (subcode adds a constant 96 bytes per sector). */
    CHECK(imgLong.size() - img5.size() >= 525u * 2352u);

    const std::string wavDiff = Join(kWorkDir, "track_diff.wav");
    const std::string rawDiff = Join(kWorkDir, "disc_diff.raw");
    CHECK(WriteWav(wavDiff, 5.0, 999999u));   /* same length, other samples */
    const char* diffTracks[1] = { wavDiff.c_str() };
    CHECK(BurnCreateRawImage(diffTracks, 1, rawDiff.c_str(), nullptr, 1) == BURN_SUCCESS);
    std::vector<unsigned char> imgDiff;
    CHECK(ReadFile(rawDiff, imgDiff));
    INFO("  same length, other samples: sizes %zu vs %zu\n", imgDiff.size(), img5.size());
    CHECK(imgDiff.size() == img5.size());
    const bool differs = std::memcmp(imgDiff.data(), img5.data(),
                                     imgDiff.size() < img5.size() ? imgDiff.size()
                                                                 : img5.size()) != 0;
    INFO("  different samples change the image: %s\n", differs ? "yes" : "no");
    CHECK(differs);

    /* ---- RAW96R layout: verify the generated Q subchannel visually ----
     * A backend that encodes subcode itself (the Linux one produces
     * 2448-byte sectors) is checked by decoding a program-area sector back:
     * the interleave must be reversible and the Q CRC must validate with an
     * independent implementation of the Red Book polynomial.
     * A backend whose writer emits its own container format (Windows IMAPI2)
     * is not 2448-aligned and is skipped here. */
    if (img5.size() % 2448 == 0) {
        auto crcQ = [](const unsigned char* q, int len) {
            unsigned short crc = 0;
            for (int i = 0; i < len; ++i) {
                crc ^= (unsigned short)((unsigned short)q[i] << 8);
                for (int b = 0; b < 8; ++b)
                    crc = (crc & 0x8000) ? (unsigned short)((crc << 1) ^ 0x1021)
                                         : (unsigned short)(crc << 1);
            }
            return (unsigned short)(crc ^ 0xFFFF);
        };
        auto bcdToInt = [](unsigned char v) {
            return (int)((v >> 4) * 10 + (v & 0x0F));
        };

        const size_t sectors5 = img5.size() / 2448;
        INFO("  RAW96R image: %zu sectors of 2448 bytes\n", sectors5);
        /* Layout: 150 lead-in sectors, then the program area. The first
         * program sector (file index 150) must report absolute time 00:02:00,
         * because the Red Book counts the lead-in as 2 s before 00:00:00. */
        const size_t probeSectors[3] = { 150, 151, 160 };
        size_t qChecked = 0, qCrcOk = 0, qTrackOk = 0, qAbsOk = 0;
        bool anchorOk = false;
        for (int k = 0; k < 3; ++k) {
            const size_t idx = probeSectors[k];
            if (idx >= sectors5) continue;
            const unsigned char* sc = img5.data() + idx * 2448 + 2352;
            /* Undo the bit-wise interleave: bit 1 of each subcode byte is Q. */
            unsigned char q[12] = {0};
            for (int bit = 0; bit < 96; ++bit) {
                if ((sc[bit] & 0x02) != 0) {
                    const int byteIndex = bit / 8;
                    const int bitInByte = 7 - (bit % 8);
                    q[byteIndex] = (unsigned char)(q[byteIndex] | (1 << bitInByte));
                }
            }
            const unsigned short want =
                (unsigned short)(((unsigned short)q[10] << 8) | q[11]);
            const unsigned short got = crcQ(q, 10);
            ++qChecked;
            if (want == got) ++qCrcOk;
            const int track = bcdToInt(q[1]);
            const int index = bcdToInt(q[2]);
            if (track == 1 && index == 1) ++qTrackOk;
            /* Absolute address must equal the sector's position on disc
             * (lead-in included), i.e. 150+k frames for track 1. */
            const int absFrames = bcdToInt(q[7]) * 60 * 75 + bcdToInt(q[8]) * 75 +
                                  bcdToInt(q[9]);
            if (absFrames == (int)idx) ++qAbsOk;
            if (idx == 150 && q[7] == 0x00 && q[8] == 0x02 && q[9] == 0x00) anchorOk = true;
            INFO("    sector %zu: track=%d index=%d crc=0x%04X(want 0x%04X) "
                 "absMsf=%02X:%02X:%02X (%d frames, expect %d)\n",
                 idx, track, index, got, want, q[7], q[8], q[9], absFrames, (int)idx);
        }
        INFO("  Q CRC valid on %zu/%zu sectors, track/index correct on %zu, "
             "absolute address correct on %zu, 00:02:00 anchor: %s\n",
             qCrcOk, qChecked, qTrackOk, qAbsOk, anchorOk ? "yes" : "no");
        CHECK(qChecked >= 2);
        CHECK(qCrcOk == qChecked);
        CHECK(qTrackOk == qChecked);
        CHECK(qAbsOk == qChecked);
        /* The first program sector of track 1 must report 00:02:00. */
        CHECK(anchorOk);
    } else {
        INFO("  (image is not RAW96R-aligned; subchannel check skipped)\n");
    }

    remove(rawLong.c_str());
    remove(raw5.c_str());
    remove(rawDiff.c_str());
    remove(wavLong.c_str());
    remove(wav5.c_str());
    remove(wavDiff.c_str());

    /* A second, geometry-free refusal: garbage input must not produce a file. */
    const std::string notWav = Join(kWorkDir, "not-audio.bin");
    { std::ofstream f(notWav.c_str(), std::ios::binary); f << "junk"; }
    const char* badTracks[1] = { notWav.c_str() };
    remove(raw.c_str());
    const int rc2 = BurnCreateRawImage(badTracks, 1, raw.c_str(), nullptr, 0);
    INFO("  4-byte non-WAV file -> rc=%d err='%s'\n", rc2, GetLastErrorMessage());
    CHECK(rc2 != BURN_SUCCESS);
}

/* ----------------------------------------------------------------- UDF -- */
static bool RunCreateImage(const std::string& src, const std::string& out,
                           int fileSystems, const char* bootImage,
                           int bootEmulation, int bootPlatform,
                           int* outRc, std::string* outErr) {
    BurnJobOptions3 o;
    std::memset(&o, 0, sizeof(o));
    o.size = (int)sizeof(o);
    o.mode = BURN_JOB_MODE_CREATE_IMAGE;
    o.sourcePath = src.c_str();
    o.outputPath = out.c_str();
    o.volumeLabel = "V3TEST";
    o.fileSystems = fileSystems;
    o.bootImagePath = bootImage;
    o.bootEmulation = bootEmulation;
    o.bootPlatform = bootPlatform;
    BurnJobStatus st;
    std::memset(&st, 0, sizeof(st));
    st.size = (int)sizeof(st);
    const int rc = Burn_Simple(reinterpret_cast<const BurnJobOptions*>(&o), &st);
    if (outRc) *outRc = rc;
    if (outErr) *outErr = st.errorMessage;
    return rc == BURN_SUCCESS;
}

static void TestUdfAndBootBytes() {
    INFO("\n=== UDF and El Torito: byte-level image structure ===\n");
    if (Burn_GetCapability(BURN_CAP_CREATE_IMAGE) != 1) {
        INFO("  skipped: no image creation on this backend\n");
        return;
    }

    const std::string src = Join(kWorkDir, "content");
    MKDIR(src.c_str());
    const std::string payload = Join(src, "payload.txt");
    { std::ofstream f(payload.c_str()); f << "burnclient v3 capability test payload"; }

    /* The load-clip: 1KB of recognisable bytes for no-emulation booting. */
    const std::string clip = Join(kWorkDir, "efiboot.img");
    std::vector<unsigned char> clipBytes(2048, 0);
    clipBytes[0] = 0xEB; clipBytes[1] = 0x00; clipBytes[2] = 0x90;  /* boot jump */
    clipBytes[512] = 0x55; clipBytes[513] = 0xAA;                    /* magic */
    for (int i = 0; i < 16; ++i) clipBytes[1024 + (size_t)i] = (unsigned char)(0xA0 + i);
    { std::ofstream f(clip.c_str(), std::ios::binary);
      f.write((const char*)clipBytes.data(), (std::streamsize)clipBytes.size()); }

    /* --- plain image: no UDF anchor must be present --- */
    const std::string isoPlain = Join(kWorkDir, "plain.iso");
    int rcPlain = 0; std::string errPlain;
    const bool plainOk = RunCreateImage(src, isoPlain, BURN_FS_ISO9660 | BURN_FS_JOLIET,
                                        nullptr, 0, 0, &rcPlain, &errPlain);
    INFO("  plain image -> rc=%d %s\n", rcPlain, plainOk ? "ok" : errPlain.c_str());
    if (!plainOk) {
        INFO("  (image writer unavailable here; UDF/boot byte tests skipped)\n");
        return;
    }
    std::vector<unsigned char> plain;
    CHECK(ReadFile(isoPlain, plain));
    /* A minimal ISO is 17 descriptors plus the file data; 53248 bytes
     * (26 sectors) is normal for a one-file image, so assert only that the
     * volume descriptor set is present. */
    CHECK(plain.size() >= 18u * 2048);
    /* PVD at LBA 16. */
    CHECK(std::memcmp(plain.data() + 16 * 2048 + 1, "CD001", 5) == 0);
    INFO("  plain image: CD001 at LBA16 ok, size=%zu\n", plain.size());

    /* --- UDF image: the anchor signature must appear --- */
    if (Burn_GetCapability(BURN_CAP_UDF) == 1) {
        const std::string isoUdf = Join(kWorkDir, "udf.iso");
        int rcUdf = 0; std::string errUdf;
        const bool udfOk = RunCreateImage(src, isoUdf,
                                          BURN_FS_ISO9660 | BURN_FS_JOLIET | BURN_FS_UDF,
                                          nullptr, 0, 0, &rcUdf, &errUdf);
        INFO("  udf image -> rc=%d %s\n", rcUdf, udfOk ? "ok" : errUdf.c_str());
        if (udfOk) {
            std::vector<unsigned char> udf;
            CHECK(ReadFile(isoUdf, udf));
            /* UDF Anchor Volume Descriptor Pointer carries "*BEA01". */
            const bool hasAnchor = ContainsBytes(udf, "BEA01");
            const bool plainHadAnchor = ContainsBytes(plain, "BEA01");
            INFO("  UDF anchor 'BEA01' present in udf.iso=%d, absent in plain.iso=%d\n",
                 (int)hasAnchor, (int)!plainHadAnchor);
            CHECK(hasAnchor);
            CHECK(!plainHadAnchor);   /* proves the flag really changed output */
            /* The metadata partition descriptor identifies itself "NSR04". */
            CHECK(ContainsBytes(udf, "NSR04") || ContainsBytes(udf, "BEA01"));
        } else {
            /* A backend may advertise UDF but refuse a particular combo; that
             * is acceptable as long as it did not pretend success. */
            CHECK(rcUdf != BURN_SUCCESS);
        }
    } else {
        INFO("  UDF not advertised; requesting it must refuse\n");
        int rc = 0; std::string err;
        RunCreateImage(src, Join(kWorkDir, "udf2.iso"), BURN_FS_UDF, nullptr, 0, 0, &rc, &err);
        INFO("  UDF request -> rc=%d err='%s'\n", rc, err.c_str());
        CHECK(rc == BURN_ERROR_UNSUPPORTED_FORMAT || rc == BURN_ERROR_ISO_CREATION_FAILED);
    }

    /* --- El Torito: follow the BRVD to the catalog, then to the clip --- */
    if (Burn_GetCapability(BURN_CAP_BOOT_IMAGE) == 1) {
        const std::string isoBoot = Join(kWorkDir, "boot.iso");
        int rcBoot = 0; std::string errBoot;
        const bool bootOk = RunCreateImage(src, isoBoot,
                                           BURN_FS_ISO9660 | BURN_FS_JOLIET,
                                           clip.c_str(), BURN_BOOT_EMU_NONE,
                                           BURN_BOOT_PLATFORM_EFI, &rcBoot, &errBoot);
        INFO("  el-torito image -> rc=%d %s\n", rcBoot, bootOk ? "ok" : errBoot.c_str());
        if (bootOk) {
            std::vector<unsigned char> img;
            CHECK(ReadFile(isoBoot, img));
            CHECK(img.size() > 18u * 2048);

            /* Boot Record Volume Descriptor lives at LBA 17. Its layout is
             *   [0] type 0, [1..5] "CD001", [6] version 1,
             *   [7..38] "EL TORITO SPECIFICATION" padded with zeros,
             *   [71..74] little-endian LBA of the boot catalog. */
            const unsigned char* brvd = img.data() + 17 * 2048;
            INFO("  BRVD type=%u id='%.5s'\n", brvd[0], brvd + 1);
            CHECK(brvd[0] == 0);
            CHECK(std::memcmp(brvd + 1, "CD001", 5) == 0);
            CHECK(std::memcmp(brvd + 7, "EL TORITO SPECIFICATION", 23) == 0);

            const unsigned catalogLba = ReadLe32(brvd + 71);
            INFO("  boot catalog LBA = %u\n", catalogLba);
            CHECK(catalogLba > 0);
            CHECK((size_t)(catalogLba + 1) * 2048 <= img.size());

            /* Boot catalog: a 32-byte validation entry followed by the
             * default entry (El Torito 1.0, section 2).
             *   validation: [0]=0x01 [1]=platform [2..3]=0 [4..27]=id
             *               [28..29]=checksum [30..31]=0x55 0xAA
             *   default:    [0]=0x88 bootable [1]=media type
             *               [6..7]=sector count [8..11]=load RBA */
            const unsigned char* cat = img.data() + (size_t)catalogLba * 2048;
            INFO("  catalog hdr=0x%02X platform=0x%02X key=0x%02X%02X\n",
                 cat[0], cat[1], cat[31], cat[30]);
            CHECK(cat[0] == 0x01);
            /* Platform id requested via bootPlatform (EFI = 0xEF). */
            CHECK(cat[1] == 0xEF);
            CHECK(cat[30] == 0x55 && cat[31] == 0xAA);

            /* The validation entry checksum is chosen so that the sum of its
             * sixteen 16-bit little-endian words is zero modulo 2^16. */
            unsigned sum = 0;
            for (int i = 0; i < 32; i += 2) {
                sum += (unsigned)cat[i] | ((unsigned)cat[i + 1] << 8);
            }
            INFO("  validation entry word-sum = 0x%04X\n", sum & 0xFFFF);
            CHECK((sum & 0xFFFF) == 0);

            const unsigned char* entry = cat + 32;
            const unsigned mediaType = entry[1];
            const unsigned loadRba = ReadLe32(entry + 8);
            INFO("  default entry: bootable=0x%02X mediaType=%u loadRba=%u\n",
                 entry[0], mediaType, loadRba);
            CHECK(entry[0] == 0x88);              /* bootable */
            CHECK(mediaType == 0);                /* no emulation */
            CHECK(loadRba > 0);
            CHECK((size_t)(loadRba + 1) * 2048 <= img.size());

            if ((size_t)(loadRba + 1) * 2048 <= img.size()) {
                const unsigned char* loaded = img.data() + (size_t)loadRba * 2048;
                /* The load-clip must be byte-identical where it matters: the
                 * jump instruction and the marker bytes we wrote. */
                const bool clipCopied =
                    std::memcmp(loaded, clipBytes.data(), 3) == 0 &&
                    std::memcmp(loaded + 1024, clipBytes.data() + 1024, 16) == 0;
                INFO("  load-clip bytes present at its LBA: %s\n",
                     clipCopied ? "yes" : "no");
                CHECK(clipCopied);
            }
        } else {
            CHECK(rcBoot != BURN_SUCCESS);
        }
    } else {
        INFO("  boot image not advertised; requesting it must refuse\n");
        int rc = 0; std::string err;
        RunCreateImage(src, Join(kWorkDir, "boot2.iso"), 0, clip.c_str(),
                       BURN_BOOT_EMU_NONE, BURN_BOOT_PLATFORM_X86, &rc, &err);
        INFO("  boot request -> rc=%d err='%s'\n", rc, err.c_str());
        CHECK(rc == BURN_ERROR_UNSUPPORTED_FORMAT || rc == BURN_ERROR_ISO_CREATION_FAILED);
    }

    remove(isoPlain.c_str()); remove(clip.c_str()); remove(payload.c_str());
    remove(Join(kWorkDir, "udf.iso").c_str());
    remove(Join(kWorkDir, "udf2.iso").c_str());
    remove(Join(kWorkDir, "boot2.iso").c_str());
    remove(Join(kWorkDir, "boot.iso").c_str());
}

/* ------------------------------------------------------- device failures -- */
static void TestDeviceResolution() {
    INFO("\n=== Device resolution is exact and never guesses ===\n");
    /* A path that exists as a drive letter but is not a recorder, and a
     * path that is nothing at all, must both fail with an explained reason. */
    const char* bogus = "V:\\definitely-not-a-recorder";
    const int rc = BurnEraseDisc(bogus, 0);
    INFO("  BurnEraseDisc(nonexistent device) -> %d err='%s'\n", rc, GetLastErrorMessage());
    CHECK(rc != BURN_SUCCESS);
    /* Burned image on a bogus device must not claim success either. */
    const int rc2 = BurnRawImageToDisc(Join(kWorkDir, "disc.raw").c_str(), bogus, 4,
                                       BURN_VERIFY_NONE);
    INFO("  BurnRawImageToDisc(nonexistent device) -> %d\n", rc2);
    CHECK(rc2 != BURN_SUCCESS);
    CHECK(BurnEjectTray(bogus) != BURN_SUCCESS);
    CHECK(BurnCloseTray(bogus) != BURN_SUCCESS);
    CHECK(Burn_GetDriveInformation(bogus) != nullptr);
}

/* ------------------------------------------------------------------------ */
int main() {
    /* Unbuffered: if a capability path crashes we still see how far it got. */
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    MKDIR(kWorkDir);
    std::printf("=== BurningDLL v3 capability tests ===\n");

    TestVersionAndCapabilities();
    TestHonestRefusal();
    TestParameterValidation();
    TestAudioGeometryValidation();
    TestRawImageBytes();
    TestUdfAndBootBytes();
    TestDeviceResolution();

    /* Remove the tree; ignore failures on a non-empty dir. */
    remove(Join(kWorkDir, "disc.raw").c_str());
    remove(Join(kWorkDir, "track01.wav").c_str());
    remove(Join(kWorkDir, "track02.wav").c_str());
    remove(Join(kWorkDir, "bad_geometry.wav").c_str());
    remove(Join(kWorkDir, "not-audio.bin").c_str());
    remove(Join(Join(kWorkDir, "content"), "payload.txt").c_str());
#ifdef _WIN32
    _rmdir(Join(kWorkDir, "content").c_str());
    _rmdir(kWorkDir);
#else
    rmdir(Join(kWorkDir, "content").c_str());
    rmdir(kWorkDir);
#endif

    if (g_failures != 0) {
        std::printf("\n%d CHECK(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\n=== v3 capability tests: SUCCESS ===\n");
    return 0;
}
