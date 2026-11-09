#include "burning_utility.h"
#include "burn_internal.h"
#include "media_format.h"
#include <string>
#include <vector>
#include <memory>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <random>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <unordered_map>
#include <exception>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

/* The last-error string is per-thread (every exported function returns a
 * pointer to a thread_local buffer - see GetLastErrorMessage). The default
 * device / disc type / speed, however, are PROCESS-WIDE and guarded by a
 * mutex: configuring them once on the startup thread must be visible to a
 * worker thread that later calls Burn_Start() with a NULL device/discType.
 * (Previously these were thread_local, so a background thread silently saw
 * "no default device" and rejected the job - fixed here.) */
static thread_local std::string g_lastError;

static std::mutex g_configMutex;
static std::string g_defaultDevice = "";
static std::string g_defaultDiscType = "CD-R";
static int g_defaultSpeed = 4;

/* Copy the three process-wide defaults under the config mutex. Callers that
 * must observe them together (Burn_Start) use this instead of reading the
 * globals field-by-field. */
struct DefaultConfig {
    std::string device;
    std::string discType;
    int speed;
};
static DefaultConfig SnapshotDefaults() {
    std::lock_guard<std::mutex> lk(g_configMutex);
    return DefaultConfig{ g_defaultDevice, g_defaultDiscType, g_defaultSpeed };
}

/* The global progress callback is process-wide (jobs run on worker threads,
 * so a thread_local callback would never be visible to them). */
static std::atomic<BurnProgressCallback> g_progressCallback{nullptr};
static std::atomic<void*> g_progressUserData{nullptr};

/* One platform instance per thread (COM apartments and libburn are both
 * thread-affine). The pointer is deliberately never destroyed: running
 * ~IPlatform from a thread_local destructor during CRT/DLL teardown would
 * call CoUninitialize() / burn_finish() from DLL_PROCESS_DETACH, after those
 * subsystems may already be gone. The OS reclaims one object per thread. */
static IPlatform* GetPlatform() {
    static thread_local IPlatform* s_instance = CreatePlatformImpl().release();
    return s_instance;
}

void SetBurnLastError(const std::string& error) {
    g_lastError = error;
}

/* Cross-platform temporary file path generation.
 * The name embeds the process id, a per-process atomic counter and random
 * digits: two concurrent DIRECTORY jobs must never land on the same path,
 * or one job's TempIsoGuard deletes the other's in-flight image. */
static std::string GenerateTempISOPath() {
    std::random_device rd;
    std::uniform_int_distribution<int> dist(100000, 999999);
    static std::atomic<unsigned> s_seq{0};
    const unsigned seq = s_seq.fetch_add(1);

#if defined(_WIN32)
    char tempDir[MAX_PATH] = {0};
    GetTempPathA(MAX_PATH, tempDir);
    if (tempDir[0] == '\0') {
        strncpy(tempDir, ".", sizeof(tempDir) - 1);
    }
    char buf[600];
    snprintf(buf, sizeof(buf), "%s\\temp_burn_%lu_%u_%d.iso",
             tempDir, (unsigned long)GetCurrentProcessId(), seq, dist(rd));
    return std::string(buf);
#else
    const char* tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = getenv("TEMP");
    if (!tmpdir) tmpdir = getenv("TMP");
    if (!tmpdir) tmpdir = "/tmp";
    char buf[600];
    snprintf(buf, sizeof(buf), "%s/temp_burn_%lu_%u_%d.iso",
             tmpdir, (unsigned long)getpid(), seq, dist(rd));
    return std::string(buf);
#endif
}

/* Cross-platform path / file existence check */
static bool PathExists(const char* path) {
    if (!path) return false;
#if defined(_WIN32)
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

/* Resolve a (possibly relative) filesystem path to an absolute one.
 *
 * Motivation: libisofs' iso_tree_add_dir_rec() rejects relative source paths
 * while IMAPI2's AddTree() accepts them, so the same CreateISO() call only
 * worked with an absolute path on Linux. Normalising to absolute at the API
 * boundary makes the behaviour identical on every backend. On failure we
 * return the input unchanged so the backend produces its own precise error. */
static std::string MakeAbsolutePath(const std::string& path) {
    if (path.empty()) return path;
#if defined(_WIN32)
    char buf[4096];
    DWORD n = GetFullPathNameA(path.c_str(), sizeof(buf), buf, nullptr);
    if (n > 0 && n < sizeof(buf)) return std::string(buf);
    return path;
#else
    if (path[0] == '/') return path;
    char buf[4096];
    if (getcwd(buf, sizeof(buf))) {
        std::string base(buf);
        if (!base.empty() && base.back() == '/') return base + path;
        return base + "/" + path;
    }
    return path;
#endif
}

static void CopyToBuffer(char* dst, size_t capacity, const std::string& src) {
    if (!dst || capacity == 0) return;
    size_t len = src.size() < capacity - 1 ? src.size() : capacity - 1;
    std::memcpy(dst, src.data(), len);
    dst[len] = '\0';
}

static unsigned long long NowMs() {
    return (unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

/* ---------------------------------------------------------------------------
 * BurnJob - internal state machine
 *
 * PENDING -> RUNNING -> COMPLETED | FAILED | CANCELLED
 *
 * Everything mutable is guarded by `mutex`. Caller callbacks are ALWAYS
 * invoked outside that mutex so they may safely call back into the API.
 * Jobs live in a shared_ptr registry keyed by an integer handle, so an
 * invalid/released handle can never dereference freed memory.
 * ------------------------------------------------------------------------- */
struct BurnJob {
    /* Immutable snapshot of the request (all strings copied by Burn_Start). */
    int mode = BURN_JOB_MODE_IMAGE;
    std::string sourcePath;
    std::string outputPath;
    std::string devicePath;
    std::string discType;
    std::string volumeLabel;
    int speed = 4;
    int verify = 0;
    BurnProgressCallback callback = nullptr;
    void* userData = nullptr;
    unsigned long long id = 0;
    unsigned long long startMs = 0;

    /* ---- API version 3 request fields ---- */
    int verifyLevel = BURN_VERIFY_NONE;
    int leaveOpen = 0;
    int fileSystems = 0;
    int interchangeLevel = 0;
    std::string bootImagePath;
    int bootEmulation = BURN_BOOT_EMU_NONE;
    int bootPlatform = BURN_BOOT_PLATFORM_X86;
    int udfRevision = 0;
    unsigned long long sessionStartBlock = 0;
    int eraseFull = 0;
    std::vector<BurnAudioTrackSpec> audioTracks;
    std::string mediaCatalogNumber;
    int startingTrackNumber = 1;

    mutable std::mutex mutex;
    std::condition_variable cv;
    std::condition_variable hookCv;
    std::thread worker;

    int state = BURN_JOB_PENDING;
    int percent = 0;
    int errorCode = BURN_SUCCESS;
    int progressBase = 0;
    int progressSpan = 100;
    bool finished = false;
    unsigned long long elapsedMs = 0;
    std::string phase;
    std::string message;
    std::string errorMessage;

    std::atomic<bool> cancelRequested{false};

    /* Native cancellation hook, set by the backend around a blocking call. */
    BurnCancelHook cancelHook = nullptr;
    void* cancelHookContext = nullptr;
    int hookInFlight = 0;

    void SetPhase(const char* value) {
        std::lock_guard<std::mutex> lk(mutex);
        phase = value ? value : "";
    }

    void SetProgressRange(int base, int span) {
        std::lock_guard<std::mutex> lk(mutex);
        progressBase = base;
        progressSpan = span > 0 ? span : 1;
    }

    /* Map a backend's 0..100 into this phase's slice and keep it monotonic. */
    void Report(int rawPercent, const char* status) {
        int mapped;
        {
            std::lock_guard<std::mutex> lk(mutex);
            if (rawPercent < 0) rawPercent = 0;
            if (rawPercent > 100) rawPercent = 100;
            mapped = progressBase + (rawPercent * progressSpan) / 100;
            if (mapped > 100) mapped = 100;
            if (mapped > percent) percent = mapped;
            message = status ? status : "";
        }
        if (callback) {
            /* User code may throw or re-enter the DLL. Never let that unwind
             * into a COM/libburn C frame or an exported C function. */
            try {
                callback(mapped, status ? status : "", userData);
            } catch (...) {
            }
        }
    }
};

static std::mutex g_jobsMutex;
static std::unordered_map<unsigned long long, std::shared_ptr<BurnJob>> g_jobs;
static std::atomic<unsigned long long> g_nextJobId{1};
static std::atomic<unsigned long long> g_simpleActiveHandle{BURN_INVALID_JOB_HANDLE};

static std::mutex g_activeJobMutex;
static std::shared_ptr<BurnJob> g_activeJob;
/* Prefer the calling thread's own job; fall back to the process-wide active
 * job only for progress events delivered on a foreign thread (IMAPI2's COM
 * event sink can fire from an RPC thread). */
static thread_local std::shared_ptr<BurnJob> g_threadJob;

static std::shared_ptr<BurnJob> GetActiveBurnJob() {
    std::lock_guard<std::mutex> lk(g_activeJobMutex);
    return g_activeJob;
}

static std::shared_ptr<BurnJob> GetCurrentBurnJob() {
    return g_threadJob ? g_threadJob : GetActiveBurnJob();
}

static std::shared_ptr<BurnJob> LookupJob(BurnJobHandle handle) {
    if (handle == BURN_INVALID_JOB_HANDLE) return nullptr;
    std::lock_guard<std::mutex> lk(g_jobsMutex);
    auto it = g_jobs.find(handle);
    return it == g_jobs.end() ? nullptr : it->second;
}

void NotifyProgressBurn(int percent, const char* status) {
    std::shared_ptr<BurnJob> job = GetCurrentBurnJob();
    if (job) {
        job->Report(percent, status);
    }
    NotifyLegacyProgress(percent, status);
}

void NotifyLegacyProgress(int percent, const char* status) {
    /* Legacy global callback. Also runs when no job is active (VerifyDisc). */
    BurnProgressCallback cb = g_progressCallback.load();
    if (cb) {
        try {
            cb(percent, status, g_progressUserData.load());
        } catch (...) {
        }
    }
}

bool IsBurnCancelled() {
    std::shared_ptr<BurnJob> job = GetCurrentBurnJob();
    if (!job) return false;
    return job->cancelRequested.load();
}

void SetBurnCancelHook(BurnCancelHook hook, void* context) {
    std::shared_ptr<BurnJob> job = GetCurrentBurnJob();
    if (!job) return;
    std::lock_guard<std::mutex> lk(job->mutex);
    job->cancelHook = hook;
    job->cancelHookContext = context;
}

void ClearBurnCancelHook(BurnCancelHook hook, void* context) {
    std::shared_ptr<BurnJob> job = GetCurrentBurnJob();
    if (!job) return;
    std::unique_lock<std::mutex> lk(job->mutex);
    if (job->cancelHook == hook && job->cancelHookContext == context) {
        job->cancelHook = nullptr;
        job->cancelHookContext = nullptr;
    }
    /* Wait for an in-flight Burn_Cancel() hook to return, so the backend can
     * safely destroy `context` as soon as this function returns. */
    job->hookCv.wait(lk, [&job] { return job->hookInFlight == 0; });
}

/* Invoke the native cancel hook outside the job mutex (the hook may block
 * briefly), while guaranteeing the context stays alive. */
static void FireCancelHook(const std::shared_ptr<BurnJob>& job) {
    BurnCancelHook hook = nullptr;
    void* context = nullptr;
    {
        std::lock_guard<std::mutex> lk(job->mutex);
        hook = job->cancelHook;
        context = job->cancelHookContext;
        if (hook) ++job->hookInFlight;
    }
    if (!hook) return;
    try {
        hook(context);
    } catch (...) {
    }
    {
        std::lock_guard<std::mutex> lk(job->mutex);
        --job->hookInFlight;
    }
    job->hookCv.notify_all();
}

/* ---------------------------------------------------------------------------
 * Job mode helpers (v3)
 * ------------------------------------------------------------------------- */
namespace {

bool ModeNeeds(int mode) { return mode >= BURN_JOB_MODE_IMAGE && mode <= BURN_JOB_MODE_BURN_RAW; }

int CapabilityForMode(int mode) {
    switch (mode) {
    case BURN_JOB_MODE_IMAGE:        return BURN_CAP_IMAGE_BURN;
    case BURN_JOB_MODE_DIRECTORY:    return BURN_CAP_CREATE_IMAGE;
    case BURN_JOB_MODE_CREATE_IMAGE: return BURN_CAP_CREATE_IMAGE;
    case BURN_JOB_MODE_AUDIO_TAO:    return BURN_CAP_AUDIO_TAO;
    case BURN_JOB_MODE_ERASE:        return BURN_CAP_ERASE;
    case BURN_JOB_MODE_APPEND:       return BURN_CAP_MULTISESSION;
    case BURN_JOB_MODE_VERIFY:       return BURN_CAP_READBACK_VERIFY;
    case BURN_JOB_MODE_CREATE_RAW:   return BURN_CAP_RAW_IMAGE;
    case BURN_JOB_MODE_BURN_RAW:     return BURN_CAP_RAW_BURN;
    default:                         return -1;
    }
}

/* true when the mode must have a source path to be meaningful. */
bool ModeNeedsSource(int mode) {
    switch (mode) {
    case BURN_JOB_MODE_IMAGE: case BURN_JOB_MODE_DIRECTORY:
    case BURN_JOB_MODE_CREATE_IMAGE: case BURN_JOB_MODE_APPEND:
    case BURN_JOB_MODE_VERIFY: case BURN_JOB_MODE_BURN_RAW:
        return true;
    default:
        return false;
    }
}

bool ModeNeedsOutput(int mode) {
    return mode == BURN_JOB_MODE_CREATE_IMAGE || mode == BURN_JOB_MODE_CREATE_RAW;
}

bool ModeNeedsAudioTracks(int mode) {
    return mode == BURN_JOB_MODE_AUDIO_TAO || mode == BURN_JOB_MODE_CREATE_RAW;
}

/* Copy the caller's fixed-size track array into owned std::strings. The
 * header promises that Burn_Start() copies every string, so the job must
 * not keep the caller's BurnAudioTrackC* alive. */
std::vector<BurnAudioTrackSpec> CopyAudioTracks(const BurnAudioTrackC* src, int count) {
    std::vector<BurnAudioTrackSpec> out;
    if (!src || count <= 0) return out;
    out.reserve((size_t)count);
    for (int i = 0; i < count; ++i) {
        BurnAudioTrackSpec t;
        t.audioPath = src[i].audioPath;
        t.isrc = src[i].isrc;
        t.preemphasis = src[i].preemphasis;
        out.push_back(t);
    }
    return out;
}

/* True when the path names a directory (the append/image decision hinges
 * on this, so it must not be guessed from a file size). */
static bool IsDirectoryPath(const std::string& path) {
#if defined(_WIN32)
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

/* Project the job request onto a backend image spec. */
BurnImageSpec MakeImageSpec(const BurnJob& job) {
    BurnImageSpec spec;
    spec.sourcePath = job.sourcePath;
    spec.outputPath = job.outputPath;
    spec.volumeLabel = job.volumeLabel;
    spec.fileSystems = job.fileSystems;
    spec.interchangeLevel = job.interchangeLevel;
    spec.bootImagePath = job.bootImagePath;
    spec.bootEmulation = job.bootEmulation;
    spec.bootPlatform = job.bootPlatform;
    spec.udfRevision = job.udfRevision;
    spec.sessionStartBlock = job.sessionStartBlock;
    spec.devicePath = job.devicePath;
    return spec;
}

} /* namespace */

/* Remove the temporary ISO even when the worker body throws. */
struct TempIsoGuard {
    std::string path;
    ~TempIsoGuard() {
        if (!path.empty()) std::remove(path.c_str());
    }
};

static int RunJobBody(BurnJob& job) {
    if (IsBurnCancelled()) return BURN_ERROR_CANCELLED;

    /* Diagnostic mode: a long-running, cooperative, hardware-free job. It
     * lets consumers (and the test suite) exercise Start/GetStatus/Cancel/
     * Wait/Release without an optical drive. */
    if (job.mode == BURN_JOB_MODE_SELFTEST) {
        job.SetPhase("selftest");
        for (int i = 0; i <= 100; ++i) {
            if (IsBurnCancelled()) return BURN_ERROR_CANCELLED;
            char msg[64];
            snprintf(msg, sizeof(msg), "Diagnostic job... %d%%", i);
            NotifyProgressBurn(i, msg);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return BURN_SUCCESS;
    }

    job.SetPhase("validating");
    NotifyProgressBurn(2, "Validating request...");
    if (IsBurnCancelled()) return BURN_ERROR_CANCELLED;

    IPlatform* platform = GetPlatform();
    if (!platform) {
        SetBurnLastError("Failed to create platform implementation");
        return BURN_ERROR_UNSUPPORTED_FORMAT;
    }

    /* Ask the backend before touching any media: a job started in an
     * extended mode on a platform that cannot do it should fail here, not
     * half-way through. Baseline modes (0..3) are not gated because the
     * default Capability() answers 0 for everything, including the
     * features every backend does implement. */
    if (job.mode >= BURN_JOB_MODE_AUDIO_TAO && job.mode <= BURN_JOB_MODE_BURN_RAW) {
        if (!platform->Capability(CapabilityForMode(job.mode))) {
            SetBurnLastError("This job mode is not implemented by the platform backend");
            return BURN_ERROR_UNSUPPORTED_FORMAT;
        }
    }
    /* Options that need a capability of their own even inside a baseline
     * mode: a plain CREATE_IMAGE works everywhere, asking for UDF or a
     * boot catalog does not. */
    if (!job.bootImagePath.empty() && !platform->Capability(BURN_CAP_BOOT_IMAGE)) {
        SetBurnLastError("El Torito boot images are not implemented by the platform backend");
        return BURN_ERROR_UNSUPPORTED_FORMAT;
    }
    if ((job.fileSystems & BURN_FS_UDF) && !platform->Capability(BURN_CAP_UDF)) {
        SetBurnLastError("UDF images are not implemented by the platform backend");
        return BURN_ERROR_UNSUPPORTED_FORMAT;
    }
    if (job.verifyLevel == BURN_VERIFY_FULL && !platform->Capability(BURN_CAP_READBACK_VERIFY)) {
        SetBurnLastError("Byte-level read-back verification is not implemented by the "
                         "platform backend");
        return BURN_ERROR_UNSUPPORTED_FORMAT;
    }

    if (ModeNeedsSource(job.mode) &&
        (job.sourcePath.empty() || !PathExists(job.sourcePath.c_str()))) {
        SetBurnLastError("Source path not found: " + job.sourcePath);
        return BURN_ERROR_INVALID_PARAM;
    }

    /* Normalise to absolute so every backend sees the same form. libisofs
     * rejects relative source paths while IMAPI2 accepts them, which made an
     * identical CreateISO() call succeed on Windows and fail on Linux.
     * The output path is normalised too (it normally does not exist yet). */
    if (!job.sourcePath.empty()) job.sourcePath = MakeAbsolutePath(job.sourcePath);
    if (!job.outputPath.empty()) job.outputPath = MakeAbsolutePath(job.outputPath);
    if (!job.bootImagePath.empty()) job.bootImagePath = MakeAbsolutePath(job.bootImagePath);

    if (ModeNeedsAudioTracks(job.mode)) {
        for (size_t i = 0; i < job.audioTracks.size(); ++i) {
            BurnAudioTrackSpec& t = job.audioTracks[i];
            t.audioPath = MakeAbsolutePath(t.audioPath);
            if (!PathExists(t.audioPath.c_str())) {
                SetBurnLastError("Audio track " + std::to_string((int)i + 1) +
                                 " not found: " + t.audioPath);
                return BURN_ERROR_INVALID_PARAM;
            }
            /* Reject a bad geometry up front: writing it would waste a
             * blank disc on audio that plays at the wrong speed. */
            burn_media::AudioPayload p = burn_media::Probe(t.audioPath);
            if (!p.ok) {
                SetBurnLastError(p.error.empty() ? "Unreadable audio track: " + t.audioPath
                                                 : p.error);
                return BURN_ERROR_INVALID_PARAM;
            }
        }
    }

    if (job.mode != BURN_JOB_MODE_CREATE_IMAGE &&
        job.mode != BURN_JOB_MODE_CREATE_RAW &&
        job.devicePath.empty()) {
        SetBurnLastError("No burning device specified");
        return BURN_ERROR_DEVICE_NOT_FOUND;
    }

    if (job.mode == BURN_JOB_MODE_CREATE_IMAGE) {
        if (job.outputPath.empty()) {
            SetBurnLastError("Output ISO path is required");
            return BURN_ERROR_INVALID_PARAM;
        }
        job.SetPhase("creating-image");
        NotifyProgressBurn(5, "Creating image...");
        if (IsBurnCancelled()) return BURN_ERROR_CANCELLED;
        BurnImageSpec spec = MakeImageSpec(job);
        if (!platform->CreateImage(spec)) {
            return BURN_ERROR_ISO_CREATION_FAILED;
        }
        NotifyProgressBurn(100, "Image creation completed");
        return BURN_SUCCESS;
    }

    if (job.mode == BURN_JOB_MODE_CREATE_RAW) {
        if (job.outputPath.empty()) {
            SetBurnLastError("Output RAW image path is required");
            return BURN_ERROR_INVALID_PARAM;
        }
        job.SetPhase("creating-raw-image");
        NotifyProgressBurn(5, "Building RAW PQ image...");
        if (IsBurnCancelled()) return BURN_ERROR_CANCELLED;
        if (!platform->CreateRawImage(job.audioTracks, job.outputPath,
                                      job.mediaCatalogNumber,
                                      job.startingTrackNumber)) {
            return BURN_ERROR_ISO_CREATION_FAILED;
        }
        NotifyProgressBurn(100, "RAW image creation completed");
        return BURN_SUCCESS;
    }

    if (job.mode == BURN_JOB_MODE_ERASE) {
        job.SetPhase("erasing");
        NotifyProgressBurn(5, "Erasing / formatting media...");
        if (IsBurnCancelled()) return BURN_ERROR_CANCELLED;
        if (!platform->EraseDisc(job.devicePath, job.eraseFull != 0)) {
            return BURN_ERROR_BURN_FAILED;
        }
        NotifyProgressBurn(100, "Erase completed");
        return BURN_SUCCESS;
    }

    if (job.mode == BURN_JOB_MODE_AUDIO_TAO) {
        job.SetPhase("burning-audio");
        NotifyProgressBurn(3, "Preparing audio session...");
        if (IsBurnCancelled()) return BURN_ERROR_CANCELLED;
        if (!platform->BurnAudioTracks(job.audioTracks, job.devicePath, job.speed,
                                       job.verifyLevel, job.leaveOpen == 0)) {
            if (IsBurnCancelled()) return BURN_ERROR_CANCELLED;
            return BURN_ERROR_BURN_FAILED;
        }
        NotifyProgressBurn(100, "Audio session completed");
        return BURN_SUCCESS;
    }

    if (job.mode == BURN_JOB_MODE_VERIFY) {
        job.SetPhase("verifying");
        NotifyProgressBurn(3, "Reading back written data...");
        if (IsBurnCancelled()) return BURN_ERROR_CANCELLED;
        int level = job.verifyLevel == BURN_VERIFY_NONE ? BURN_VERIFY_FULL : job.verifyLevel;
        if (!platform->VerifyWrite(job.devicePath, job.sourcePath, level)) {
            if (IsBurnCancelled()) return BURN_ERROR_CANCELLED;
            /* Last-error carries the sector-accurate reason. */
            return BURN_ERROR_BURN_FAILED;
        }
        NotifyProgressBurn(100, "Verification completed");
        return BURN_SUCCESS;
    }

    std::string isoPath = job.sourcePath;
    TempIsoGuard temp;
    if (job.mode == BURN_JOB_MODE_DIRECTORY) {
        temp.path = GenerateTempISOPath();
        job.SetProgressRange(0, 40);
        job.SetPhase("creating-image");
        if (IsBurnCancelled()) return BURN_ERROR_CANCELLED;
        BurnImageSpec spec = MakeImageSpec(job);
        spec.outputPath = temp.path;
        if (!platform->CreateImage(spec)) {
            return BURN_ERROR_ISO_CREATION_FAILED;
        }
        isoPath = temp.path;
        if (IsBurnCancelled()) return BURN_ERROR_CANCELLED;
        job.SetProgressRange(40, 60);
    }

    job.SetPhase("burning");
    if (IsBurnCancelled()) return BURN_ERROR_CANCELLED;

    bool burned;
    if (job.mode == BURN_JOB_MODE_BURN_RAW) {
        burned = platform->BurnRawImage(isoPath, job.devicePath, job.speed,
                                       job.verifyLevel);
    } else {
        /* IMAGE / DIRECTORY / APPEND share one entry point that also carries
         * the v3 write options. A first-session closed write without
         * verification is exactly the legacy BurnISO(). */
        long long sbbSigned = (long long)job.sessionStartBlock;
        if (job.mode == BURN_JOB_MODE_APPEND && sbbSigned == 0) {
            /* Not supplied: ask the drive where the next session starts. */
            sbbSigned = platform->NextSessionStartBlock(job.devicePath);
            if (sbbSigned < 0) {
                if (sbbSigned == -2) {
                    SetBurnLastError("This platform backend cannot inquire session addresses");
                    return BURN_ERROR_UNSUPPORTED_FORMAT;
                }
                SetBurnLastError("Append requires an appendable disc; the drive reported "
                                 "no free session (blank media takes mode DIRECTORY instead)");
                return BURN_ERROR_BURN_FAILED;
            }
        }
        const unsigned long long sbb = sbbSigned > 0 ? (unsigned long long)sbbSigned : 0ULL;

        if (job.mode == BURN_JOB_MODE_APPEND && IsDirectoryPath(job.sourcePath)) {
            /* A directory source means the caller wants the continuation
             * session's image built for that LBA first. */
            temp.path = GenerateTempISOPath();
            job.SetPhase("creating-image");
            BurnImageSpec spec = MakeImageSpec(job);
            spec.outputPath = temp.path;
            spec.sessionStartBlock = sbb;
            if (!platform->CreateImage(spec)) {
                return BURN_ERROR_ISO_CREATION_FAILED;
            }
            isoPath = temp.path;
            if (IsBurnCancelled()) return BURN_ERROR_CANCELLED;
            job.SetPhase("burning");
        }
        burned = platform->BurnImageFile(isoPath, job.devicePath, job.discType,
                                         job.speed, job.verifyLevel,
                                         job.leaveOpen == 0, sbb);
    }
    /* Result first, late cancellation second: a cancel that lands while the
     * drive is finishing its last sectors must not downgrade a completely
     * written disc to CANCELLED - the caller would skip verifying good
     * media. The backends apply the same rule at their own level. */
    if (!burned) {
        if (IsBurnCancelled()) return BURN_ERROR_CANCELLED;
        return BURN_ERROR_BURN_FAILED;
    }

    NotifyProgressBurn(100, "Burn completed");
    return BURN_SUCCESS;
}

static void JobThread(std::shared_ptr<BurnJob> job) {
    g_threadJob = job;
    {
        std::lock_guard<std::mutex> lk(job->mutex);
        job->state = BURN_JOB_RUNNING;
        job->phase = "starting";
        job->message = "Job started";
    }
    {
        std::lock_guard<std::mutex> lk(g_activeJobMutex);
        g_activeJob = job;
    }

    int code = BURN_SUCCESS;
    std::string error;
    try {
        code = RunJobBody(*job);
    } catch (const std::exception& e) {
        code = BURN_ERROR_EXCEPTION;
        error = std::string("Internal exception: ") + e.what();
    } catch (...) {
        code = BURN_ERROR_EXCEPTION;
        error = "Internal exception (unknown type)";
    }

    if (code != BURN_SUCCESS && error.empty()) {
        /* Platform backends report details through the calling thread's
         * thread_local last-error buffer; this IS that thread. */
        const char* last = GetLastErrorMessage();
        if (last && *last) error = last;
    }
    if (code == BURN_ERROR_CANCELLED && error.empty()) {
        error = "Burn cancelled by caller";
    }
    if (error.empty() && code != BURN_SUCCESS) {
        error = "Burn job failed";
    }

    {
        std::lock_guard<std::mutex> lk(job->mutex);
        job->errorCode = code;
        job->elapsedMs = NowMs() - job->startMs;
        job->finished = true;
        switch (code) {
        case BURN_SUCCESS:
            job->state = BURN_JOB_COMPLETED;
            job->phase = "completed";
            job->message = "Completed";
            job->percent = 100;
            break;
        case BURN_ERROR_CANCELLED:
            job->state = BURN_JOB_CANCELLED;
            job->phase = "cancelled";
            job->message = "Cancelled by caller";
            job->errorMessage = error;
            break;
        default:
            job->state = BURN_JOB_FAILED;
            job->phase = "failed";
            job->message = error;
            job->errorMessage = error;
            break;
        }
    }
    job->cv.notify_all();

    {
        std::lock_guard<std::mutex> lk(g_activeJobMutex);
        if (g_activeJob.get() == job.get()) g_activeJob.reset();
    }
    g_threadJob.reset();
}

/* ---------------------------------------------------------------------------
 * C API. Every exported function is wrapped in try/catch so no C++ exception
 * can ever unwind through the C ABI, a COM frame, or a libburn C frame.
 * ------------------------------------------------------------------------- */
extern "C" {

const char* GetLastErrorMessage() {
    try {
        static thread_local std::string result;
        result = g_lastError;
        return result.c_str();
    } catch (const std::exception& e) {
        SetBurnLastError(std::string("Internal exception: ") + e.what());
        return "Internal exception";
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return "Internal exception";
    }
}

void ClearLastError() {
    try {
        g_lastError.clear();
    } catch (...) {
        return;
    }
}

void FreeStringMemory(void* ptr) {
    try {
        free(ptr);
    } catch (...) {
        return;
    }
}

void SetProgressCallback(BurnProgressCallback callback, void* userData) {
    try {
        g_progressCallback.store(callback);
        g_progressUserData.store(userData);
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return;
    }
}

/* ---- Legacy synchronous API: now a thin compatibility layer over jobs ---- */

int CreateISOToDisc(const char* isoPath, const char* devicePath, const char* discType, int speed) {
    try {
        if (!isoPath || !devicePath) {
            SetBurnLastError("Invalid parameters");
            return BURN_ERROR_INVALID_PARAM;
        }
        BurnJobOptions options;
        std::memset(&options, 0, sizeof(options));
        options.size = (int)sizeof(options);
        options.mode = BURN_JOB_MODE_IMAGE;
        options.sourcePath = isoPath;
        options.devicePath = devicePath;
        options.discType = discType;
        options.speed = speed;
        return Burn_Simple(&options, nullptr);
    } catch (const std::exception& e) {
        SetBurnLastError(std::string("Internal exception: ") + e.what());
        return BURN_ERROR_EXCEPTION;
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return BURN_ERROR_EXCEPTION;
    }
}

int BurnDirectoryToDisc(const char* sourceDir, const char* devicePath, const char* discType, int speed) {
    try {
        if (!sourceDir || !devicePath) {
            SetBurnLastError("Invalid parameters");
            return BURN_ERROR_INVALID_PARAM;
        }
        BurnJobOptions options;
        std::memset(&options, 0, sizeof(options));
        options.size = (int)sizeof(options);
        options.mode = BURN_JOB_MODE_DIRECTORY;
        options.sourcePath = sourceDir;
        options.devicePath = devicePath;
        options.discType = discType;
        options.volumeLabel = "BURN";
        options.speed = speed;
        return Burn_Simple(&options, nullptr);
    } catch (const std::exception& e) {
        SetBurnLastError(std::string("Internal exception: ") + e.what());
        return BURN_ERROR_EXCEPTION;
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return BURN_ERROR_EXCEPTION;
    }
}

int CreateISOImage(const char* sourcePath, const char* isoPath, const char* volumeLabel) {
    try {
        if (!sourcePath || !isoPath) {
            SetBurnLastError("Invalid parameters");
            return BURN_ERROR_INVALID_PARAM;
        }
        BurnJobOptions options;
        std::memset(&options, 0, sizeof(options));
        options.size = (int)sizeof(options);
        options.mode = BURN_JOB_MODE_CREATE_IMAGE;
        options.sourcePath = sourcePath;
        options.outputPath = isoPath;
        options.volumeLabel = volumeLabel;
        return Burn_Simple(&options, nullptr);
    } catch (const std::exception& e) {
        SetBurnLastError(std::string("Internal exception: ") + e.what());
        return BURN_ERROR_EXCEPTION;
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return BURN_ERROR_EXCEPTION;
    }
}

int VerifyDisc(const char* devicePath, const char* isoPath) {
    try {
        if (!devicePath || !isoPath) {
            SetBurnLastError("Invalid parameters");
            return BURN_ERROR_INVALID_PARAM;
        }
        if (!PathExists(isoPath)) {
            SetBurnLastError("ISO file not found for verification");
            return BURN_ERROR_INVALID_PARAM;
        }
        FILE* isoFile = fopen(isoPath, "rb");
        if (!isoFile) {
            SetBurnLastError("Cannot open ISO file for verification");
            return BURN_ERROR_INVALID_PARAM;
        }
        fseek(isoFile, 0, SEEK_END);
        long isoSize = ftell(isoFile);
        /* Pre-flight check of the image we were asked to compare against. */
        char probe[4];
        fseek(isoFile, 0, SEEK_SET);
        size_t got = fread(probe, 1, sizeof(probe), isoFile);
        fclose(isoFile);
        if (isoSize <= 0) {
            SetBurnLastError("ISO file is empty");
            return BURN_ERROR_ISO_CREATION_FAILED;
        }
        NotifyLegacyProgress(50, "Validating image file...");

        /* Honest return: no byte-level read-back was performed.
         *
         * IMAPI2 has no disc-read interface at all, and libburn's read APIs
         * are not wired into this library, so there is no backend that can
         * read a track back off the medium. Returning BURN_SUCCESS here used
         * to report "verified" after only having stat()ed the ISO file,
         * which is indistinguishable from a real verification to callers
         * checking for 0. Callers that need integrity assurance must compare
         * a hash they read themselves (for example by mounting the disc). */
        (void)got;
        SetBurnLastError(
            "Disc read-back verification is not implemented on this platform; "
            "the ISO file was checked but the disc contents were not read");
        return BURN_ERROR_UNSUPPORTED_FORMAT;
    } catch (const std::exception& e) {
        SetBurnLastError(std::string("Internal exception: ") + e.what());
        return BURN_ERROR_EXCEPTION;
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return BURN_ERROR_EXCEPTION;
    }
}

int ListBurningDevices(DeviceInfo** devices, int* deviceCount) {
    try {
        if (!devices || !deviceCount) {
            SetBurnLastError("Invalid parameters");
            return BURN_ERROR_INVALID_PARAM;
        }
        IPlatform* platform = GetPlatform();
        if (!platform) {
            SetBurnLastError("Failed to create platform implementation");
            return BURN_ERROR_UNSUPPORTED_FORMAT;
        }
        std::vector<DeviceInfo> deviceList = platform->ListDevices();
        *deviceCount = (int)deviceList.size();
        if (*deviceCount == 0) {
            *devices = nullptr;
            return BURN_SUCCESS;
        }
        *devices = (DeviceInfo*)malloc(sizeof(DeviceInfo) * (*deviceCount));
        if (!*devices) {
            SetBurnLastError("Memory allocation failed");
            return BURN_ERROR_NO_SPACE;
        }
        for (int i = 0; i < *deviceCount; ++i) {
            (*devices)[i] = deviceList[i];
        }
        return BURN_SUCCESS;
    } catch (const std::exception& e) {
        SetBurnLastError(std::string("Internal exception: ") + e.what());
        return BURN_ERROR_EXCEPTION;
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return BURN_ERROR_EXCEPTION;
    }
}

void FreeDeviceList(DeviceInfo* devices, int deviceCount) {
    (void)deviceCount;
    try {
        free(devices);
    } catch (...) {
        return;
    }
}

int GetDefaultDevice(char* devicePath, int bufferSize) {
    try {
        if (!devicePath || bufferSize <= 0) {
            return BURN_ERROR_INVALID_PARAM;
        }
        std::string device;
        {
            std::lock_guard<std::mutex> lk(g_configMutex);
            device = g_defaultDevice;
        }
        if (device.empty()) {
            devicePath[0] = '\0';
            return BURN_ERROR_DEVICE_NOT_FOUND;
        }
        strncpy(devicePath, device.c_str(), bufferSize - 1);
        devicePath[bufferSize - 1] = '\0';
        return BURN_SUCCESS;
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return BURN_ERROR_EXCEPTION;
    }
}

const char* GetPlatformInfo() {
    try {
        IPlatform* platform = GetPlatform();
        if (!platform) return "Unknown";
        static thread_local std::string result;
        result = platform->GetPlatformInfo();
        return result.c_str();
    } catch (const std::exception& e) {
        SetBurnLastError(std::string("Internal exception: ") + e.what());
        return "Internal exception";
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return "Internal exception";
    }
}

int CheckToolAvailability(const char* toolName) {
    try {
        if (!toolName) return 0;
        IPlatform* platform = GetPlatform();
        if (!platform) return 0;
        return platform->CheckToolAvailability(toolName) ? 1 : 0;
    } catch (const std::exception& e) {
        SetBurnLastError(std::string("Internal exception: ") + e.what());
        return BURN_ERROR_EXCEPTION;
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return BURN_ERROR_EXCEPTION;
    }
}

const char* GetSupportedFormats() {
    try {
        /* A string literal - callers must NOT call FreeStringMemory on it. */
        return "CD-R, CD-RW, DVD-R, DVD+R, DVD-RW, DVD+RW, DVD-RAM, BD-R, BD-RE";
    } catch (...) {
        return "Internal exception";
    }
}

int ValidateSourcePath(const char* path) {
    try {
        return path && PathExists(path) ? 1 : 0;
    } catch (const std::exception& e) {
        SetBurnLastError(std::string("Internal exception: ") + e.what());
        return BURN_ERROR_EXCEPTION;
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return BURN_ERROR_EXCEPTION;
    }
}

void SetDefaultDevice(const char* device) {
    try {
        if (device) {
            std::lock_guard<std::mutex> lk(g_configMutex);
            g_defaultDevice = device;
        }
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
    }
}

void SetDefaultDiscType(const char* discType) {
    try {
        if (discType) {
            std::lock_guard<std::mutex> lk(g_configMutex);
            g_defaultDiscType = discType;
        }
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
    }
}

void SetDefaultSpeed(int speed) {
    try {
        if (speed > 0) {
            std::lock_guard<std::mutex> lk(g_configMutex);
            g_defaultSpeed = speed;
        }
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
    }
}

/* ---- Handle-based job API ---- */

BurnJobHandle Burn_Start(const BurnJobOptions* options) {
    try {
        if (!options) {
            SetBurnLastError("Burn_Start: options must not be NULL");
            return BURN_INVALID_JOB_HANDLE;
        }
        /* Two struct versions share a prefix: dispatch on `size`. */
        const bool v3 = options->size == (int)sizeof(BurnJobOptions3);
        if (options->size != (int)sizeof(BurnJobOptions) && !v3) {
            SetBurnLastError("Burn_Start: set options.size to sizeof(BurnJobOptions) "
                             "or sizeof(BurnJobOptions3)");
            return BURN_INVALID_JOB_HANDLE;
        }
        const BurnJobOptions3* o3 = v3 ? reinterpret_cast<const BurnJobOptions3*>(options) : nullptr;

        if (!ModeNeeds(options->mode)) {
            SetBurnLastError("Burn_Start: unknown job mode");
            return BURN_INVALID_JOB_HANDLE;
        }
        if (ModeNeedsSource(options->mode) &&
            (!options->sourcePath || !options->sourcePath[0])) {
            SetBurnLastError("Burn_Start: sourcePath is required for this job mode");
            return BURN_INVALID_JOB_HANDLE;
        }
        if (ModeNeedsOutput(options->mode) &&
            (!options->outputPath || !options->outputPath[0])) {
            SetBurnLastError("Burn_Start: outputPath is required for this job mode");
            return BURN_INVALID_JOB_HANDLE;
        }
        if (v3) {
            if (ModeNeedsAudioTracks(options->mode)) {
                if (!o3->audioTracks || o3->audioTrackCount <= 0) {
                    SetBurnLastError("Burn_Start: audioTracks/audioTrackCount are required "
                                     "for this job mode");
                    return BURN_INVALID_JOB_HANDLE;
                }
                if (o3->audioTrackCount > 99) {
                    SetBurnLastError("Burn_Start: a CD holds at most 99 tracks");
                    return BURN_INVALID_JOB_HANDLE;
                }
            }
            if (o3->audioTrackCount > 0 && !ModeNeedsAudioTracks(options->mode)) {
                SetBurnLastError("Burn_Start: audioTracks given for a mode that does not use them");
                return BURN_INVALID_JOB_HANDLE;
            }
            if (o3->interchangeLevel != 0 &&
                (o3->interchangeLevel < 1 || o3->interchangeLevel > 3)) {
                SetBurnLastError("Burn_Start: interchangeLevel must be 1..3 or 0");
                return BURN_INVALID_JOB_HANDLE;
            }
            if (o3->fileSystems & ~(BURN_FS_ISO9660 | BURN_FS_JOLIET | BURN_FS_UDF)) {
                SetBurnLastError("Burn_Start: fileSystems contains unknown BURN_FS_* bits");
                return BURN_INVALID_JOB_HANDLE;
            }
            if (o3->bootEmulation < BURN_BOOT_EMU_NONE || o3->bootEmulation > BURN_BOOT_EMU_HARDDISK) {
                SetBurnLastError("Burn_Start: bootEmulation must be BURN_BOOT_EMU_*");
                return BURN_INVALID_JOB_HANDLE;
            }
            if (o3->verifyLevel < BURN_VERIFY_NONE || o3->verifyLevel > BURN_VERIFY_FULL) {
                SetBurnLastError("Burn_Start: verifyLevel must be BURN_VERIFY_*");
                return BURN_INVALID_JOB_HANDLE;
            }
            if (o3->startingTrackNumber != 0 &&
                (o3->startingTrackNumber < 1 || o3->startingTrackNumber > 99)) {
                SetBurnLastError("Burn_Start: startingTrackNumber must be 1..99 or 0");
                return BURN_INVALID_JOB_HANDLE;
            }
        }

        std::shared_ptr<BurnJob> job = std::make_shared<BurnJob>();
        const DefaultConfig cfg = SnapshotDefaults();
        job->mode = options->mode;
        job->sourcePath = options->sourcePath ? options->sourcePath : "";
        job->outputPath = options->outputPath ? options->outputPath : "";
        job->devicePath = options->devicePath ? options->devicePath : "";
        job->discType = options->discType ? options->discType : "";
        job->volumeLabel = options->volumeLabel ? options->volumeLabel : "";
        if (job->devicePath.empty()) job->devicePath = cfg.device;
        if (job->discType.empty()) job->discType = cfg.discType;
        if (job->volumeLabel.empty()) job->volumeLabel = "DISC";
        job->speed = options->speed > 0 ? options->speed : cfg.speed;
        job->verify = options->verify;
        job->callback = options->progressCallback;
        job->userData = options->userData;
        job->startMs = NowMs();

        if (v3) {
            /* A legacy verify=1 request means "verify", not "level 1"
             * only when verifyLevel was left at zero. */
            job->verifyLevel = o3->verifyLevel;
            if (job->verifyLevel == BURN_VERIFY_NONE && o3->verify > 0) {
                job->verifyLevel = BURN_VERIFY_QUICK;
            }
            job->leaveOpen = o3->leaveOpen ? 1 : 0;
            job->fileSystems = o3->fileSystems;
            job->interchangeLevel = o3->interchangeLevel;
            job->bootImagePath = o3->bootImagePath ? o3->bootImagePath : "";
            job->bootEmulation = o3->bootEmulation;
            job->bootPlatform = o3->bootPlatform;
            job->udfRevision = o3->udfRevision;
            job->sessionStartBlock = o3->sessionStartBlock;
            job->eraseFull = o3->eraseFull ? 1 : 0;
            job->audioTracks = CopyAudioTracks(o3->audioTracks, o3->audioTrackCount);
            job->mediaCatalogNumber = o3->mediaCatalogNumber ? o3->mediaCatalogNumber : "";
            job->startingTrackNumber = o3->startingTrackNumber > 0 ? o3->startingTrackNumber : 1;
        } else if (job->verify > 0) {
            job->verifyLevel = BURN_VERIFY_QUICK;
        }

        unsigned long long id = 0;
        {
            std::lock_guard<std::mutex> lk(g_jobsMutex);
            id = g_nextJobId.fetch_add(1);
            job->id = id;
            g_jobs.emplace(id, job);
        }
        try {
            job->worker = std::thread(JobThread, job);
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(g_jobsMutex);
            g_jobs.erase(id);
            SetBurnLastError(std::string("Failed to start burn worker: ") + e.what());
            return BURN_INVALID_JOB_HANDLE;
        } catch (...) {
            std::lock_guard<std::mutex> lk(g_jobsMutex);
            g_jobs.erase(id);
            SetBurnLastError("Failed to start burn worker thread");
            return BURN_INVALID_JOB_HANDLE;
        }
        return id;
    } catch (const std::exception& e) {
        SetBurnLastError(std::string("Internal exception: ") + e.what());
        return BURN_INVALID_JOB_HANDLE;
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return BURN_INVALID_JOB_HANDLE;
    }
}

int Burn_GetStatus(BurnJobHandle handle, BurnJobStatus* status) {
    try {
        if (!status) {
            SetBurnLastError("Burn_GetStatus: status must not be NULL");
            return BURN_ERROR_INVALID_PARAM;
        }
        if (status->size != (int)sizeof(BurnJobStatus)) {
            SetBurnLastError("Burn_GetStatus: set status.size to sizeof(BurnJobStatus)");
            return BURN_ERROR_INVALID_PARAM;
        }
        std::shared_ptr<BurnJob> job = LookupJob(handle);
        if (!job) return BURN_ERROR_INVALID_HANDLE;

        std::lock_guard<std::mutex> lk(job->mutex);
        status->state = job->state;
        status->percent = job->percent;
        status->errorCode = job->errorCode;
        status->cancelRequested = job->cancelRequested.load() ? 1 : 0;
        status->elapsedMs = job->finished ? job->elapsedMs : NowMs() - job->startMs;
        CopyToBuffer(status->phase, sizeof(status->phase), job->phase);
        CopyToBuffer(status->message, sizeof(status->message), job->message);
        CopyToBuffer(status->errorMessage, sizeof(status->errorMessage), job->errorMessage);
        return BURN_SUCCESS;
    } catch (const std::exception& e) {
        SetBurnLastError(std::string("Internal exception: ") + e.what());
        return BURN_ERROR_EXCEPTION;
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return BURN_ERROR_EXCEPTION;
    }
}

int Burn_Cancel(BurnJobHandle handle) {
    try {
        std::shared_ptr<BurnJob> job = LookupJob(handle);
        if (!job) return BURN_ERROR_INVALID_HANDLE;
        {
            std::lock_guard<std::mutex> lk(job->mutex);
            job->cancelRequested.store(true);
            if (!job->finished) {
                job->phase = "cancelling";
                job->message = "Cancellation requested";
            }
        }
        FireCancelHook(job);
        return BURN_SUCCESS;
    } catch (const std::exception& e) {
        SetBurnLastError(std::string("Internal exception: ") + e.what());
        return BURN_ERROR_EXCEPTION;
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return BURN_ERROR_EXCEPTION;
    }
}

int Burn_Wait(BurnJobHandle handle, int timeoutMs) {
    try {
        std::shared_ptr<BurnJob> job = LookupJob(handle);
        if (!job) return BURN_ERROR_INVALID_HANDLE;
        std::unique_lock<std::mutex> lk(job->mutex);
        if (timeoutMs < 0) {
            job->cv.wait(lk, [&job] { return job->finished; });
            return BURN_SUCCESS;
        }
        if (!job->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                              [&job] { return job->finished; })) {
            return BURN_ERROR_TIMEOUT;
        }
        return BURN_SUCCESS;
    } catch (const std::exception& e) {
        SetBurnLastError(std::string("Internal exception: ") + e.what());
        return BURN_ERROR_EXCEPTION;
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return BURN_ERROR_EXCEPTION;
    }
}

int Burn_Release(BurnJobHandle handle) {
    try {
        std::shared_ptr<BurnJob> job;
        {
            std::lock_guard<std::mutex> lk(g_jobsMutex);
            auto it = g_jobs.find(handle);
            if (it == g_jobs.end()) return BURN_ERROR_INVALID_HANDLE;
            job = it->second;
            g_jobs.erase(it);
        }
        {
            std::lock_guard<std::mutex> lk(job->mutex);
            if (!job->finished) job->cancelRequested.store(true);
        }
        FireCancelHook(job);
        /* The worker owns its own shared_ptr, so detaching keeps the job alive
         * until the thread finishes. Never destroy a joinable std::thread. */
        if (job->worker.joinable()) job->worker.detach();
        return BURN_SUCCESS;
    } catch (const std::exception& e) {
        SetBurnLastError(std::string("Internal exception: ") + e.what());
        return BURN_ERROR_EXCEPTION;
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return BURN_ERROR_EXCEPTION;
    }
}

int Burn_Simple(const BurnJobOptions* options, BurnJobStatus* outStatus) {
    try {
        BurnJobStatus snapshot;
        std::memset(&snapshot, 0, sizeof(snapshot));
        snapshot.size = (int)sizeof(snapshot);

        BurnJobHandle handle = Burn_Start(options);
        if (handle == BURN_INVALID_JOB_HANDLE) {
            snapshot.state = BURN_JOB_FAILED;
            snapshot.errorCode = BURN_ERROR_INVALID_PARAM;
            const char* reason = GetLastErrorMessage();
            CopyToBuffer(snapshot.errorMessage, sizeof(snapshot.errorMessage),
                         reason ? reason : "Burn_Start failed");
            CopyToBuffer(snapshot.message, sizeof(snapshot.message), snapshot.errorMessage);
            if (outStatus) *outStatus = snapshot;
            return snapshot.errorCode;
        }

        g_simpleActiveHandle.store(handle);
        int waitRc = Burn_Wait(handle, -1);
        Burn_GetStatus(handle, &snapshot);
        g_simpleActiveHandle.store(BURN_INVALID_JOB_HANDLE);

        if (waitRc == BURN_ERROR_TIMEOUT) {
            snapshot.errorCode = BURN_ERROR_TIMEOUT;
        }
        Burn_Release(handle);

        if (outStatus) *outStatus = snapshot;
        /* Copy the worker's error onto the calling thread's buffer so the
         * legacy GetLastErrorMessage() keeps working. */
        SetBurnLastError(snapshot.errorMessage);
        return snapshot.errorCode;
    } catch (const std::exception& e) {
        SetBurnLastError(std::string("Internal exception: ") + e.what());
        return BURN_ERROR_EXCEPTION;
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return BURN_ERROR_EXCEPTION;
    }
}

const char* Burn_GetJobErrorMessage(BurnJobHandle handle) {
    try {
        static thread_local std::string result;
        std::shared_ptr<BurnJob> job = LookupJob(handle);
        if (!job) {
            result = "Invalid or released job handle";
            return result.c_str();
        }
        std::lock_guard<std::mutex> lk(job->mutex);
        result = job->errorMessage;
        return result.c_str();
    } catch (...) {
        return "Internal exception";
    }
}

int CancelCurrentBurn(void) {
    try {
        BurnJobHandle handle = g_simpleActiveHandle.load();
        if (handle == BURN_INVALID_JOB_HANDLE) return BURN_SUCCESS;
        return Burn_Cancel(handle);
    } catch (...) {
        return BURN_ERROR_EXCEPTION;
    }
}

int Burn_GetActiveJobCount(void) {
    try {
        std::lock_guard<std::mutex> lk(g_jobsMutex);
        return (int)g_jobs.size();
    } catch (...) {
        return 0;
    }
}

int Burn_GetApiVersion(void) {
    return BURN_API_VERSION;
}

/* ---- API version 3: capability discovery and the extended surface ---- */

int Burn_GetCapability(int capability) {
    try {
        IPlatform* platform = GetPlatform();
        if (!platform) return 0;
        /* The two baseline features are implemented by every backend in
         * this library, so answer them here instead of relying on an
         * override that exists for that purpose. */
        if (capability == BURN_CAP_IMAGE_BURN || capability == BURN_CAP_CREATE_IMAGE) {
            return 1;
        }
        int v = platform->Capability(capability);
        return v ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

long long Burn_GetNextSessionBlock(const char* devicePath) {
    try {
        if (!devicePath || !devicePath[0]) {
            SetBurnLastError("Burn_GetNextSessionBlock: devicePath is required");
            return BURN_ERROR_INVALID_PARAM;
        }
        IPlatform* platform = GetPlatform();
        if (!platform) {
            SetBurnLastError("Failed to create platform implementation");
            return BURN_ERROR_UNSUPPORTED_FORMAT;
        }
        return platform->NextSessionStartBlock(devicePath);
    } catch (const std::exception& e) {
        SetBurnLastError(std::string("Internal exception: ") + e.what());
        return BURN_ERROR_EXCEPTION;
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return BURN_ERROR_EXCEPTION;
    }
}

int BurnEjectTray(const char* devicePath) {
    try {
        if (!devicePath || !devicePath[0]) {
            SetBurnLastError("BurnEjectTray: devicePath is required");
            return BURN_ERROR_INVALID_PARAM;
        }
        IPlatform* platform = GetPlatform();
        if (!platform) return BURN_ERROR_UNSUPPORTED_FORMAT;
        return platform->EjectTray(devicePath);
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return BURN_ERROR_EXCEPTION;
    }
}

int BurnCloseTray(const char* devicePath) {
    try {
        if (!devicePath || !devicePath[0]) {
            SetBurnLastError("BurnCloseTray: devicePath is required");
            return BURN_ERROR_INVALID_PARAM;
        }
        IPlatform* platform = GetPlatform();
        if (!platform) return BURN_ERROR_UNSUPPORTED_FORMAT;
        return platform->CloseTray(devicePath);
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return BURN_ERROR_EXCEPTION;
    }
}

const char* Burn_GetDriveInformation(const char* devicePath) {
    static thread_local std::string result;
    try {
        if (!devicePath || !devicePath[0]) {
            SetBurnLastError("Burn_GetDriveInformation: devicePath is required");
            result.clear();
            return result.c_str();
        }
        IPlatform* platform = GetPlatform();
        if (!platform) {
            result.clear();
            return result.c_str();
        }
        result = platform->DriveInformation(devicePath);
        return result.c_str();
    } catch (...) {
        result.clear();
        return result.c_str();
    }
}

const char* Burn_GetMediaType(const char* devicePath) {
    static thread_local std::string result;
    try {
        result.clear();
        if (!devicePath || !devicePath[0]) {
            SetBurnLastError("Burn_GetMediaType: devicePath is required");
            return result.c_str();
        }
        IPlatform* platform = GetPlatform();
        if (!platform) {
            SetBurnLastError("Burn_GetMediaType: no platform implementation");
            return result.c_str();
        }
        result = platform->MediaType(devicePath);
        return result.c_str();
    } catch (...) {
        SetBurnLastError("Burn_GetMediaType: internal exception");
        result.clear();
        return result.c_str();
    }
}

int Burn_GetMediaTypeCode(const char* devicePath) {
    try {
        if (!devicePath || !devicePath[0]) {
            SetBurnLastError("Burn_GetMediaTypeCode: devicePath is required");
            return BURN_MEDIA_UNKNOWN;
        }
        IPlatform* platform = GetPlatform();
        if (!platform) {
            SetBurnLastError("Burn_GetMediaTypeCode: no platform implementation");
            return BURN_MEDIA_UNKNOWN;
        }
        return platform->MediaTypeCode(devicePath);
    } catch (...) {
        SetBurnLastError("Burn_GetMediaTypeCode: internal exception");
        return BURN_MEDIA_UNKNOWN;
    }
}

int Burn_GetMediaState(const char* devicePath) {
    try {
        if (!devicePath || !devicePath[0]) {
            SetBurnLastError("Burn_GetMediaState: devicePath is required");
            return -1;
        }
        IPlatform* platform = GetPlatform();
        if (!platform) {
            SetBurnLastError("Burn_GetMediaState: no platform implementation");
            return -1;
        }
        return platform->MediaState(devicePath);
    } catch (...) {
        SetBurnLastError("Burn_GetMediaState: internal exception");
        return -1;
    }
}

const char* Burn_GetMediaStateText(const char* devicePath) {
    static thread_local std::string result;
    try {
        result.clear();
        if (!devicePath || !devicePath[0]) {
            SetBurnLastError("Burn_GetMediaStateText: devicePath is required");
            result = "A device path is required";
            return result.c_str();
        }
        IPlatform* platform = GetPlatform();
        if (!platform) {
            SetBurnLastError("Burn_GetMediaStateText: no platform implementation");
            result = "No platform implementation";
            return result.c_str();
        }
        result = platform->MediaStateText(devicePath);
        return result.c_str();
    } catch (...) {
        SetBurnLastError("Burn_GetMediaStateText: internal exception");
        result = "Media readiness query failed with an internal exception";
        return result.c_str();
    }
}

namespace {

/* Shared plumbing for the synchronous v3 convenience wrappers: build a
 * BurnJobOptions3, run it to completion through the job engine (so all of
 * them get the same cancellation, progress and error semantics), and hand
 * back the job's errorCode. */
int RunV3Simple(BurnJobOptions3* o, BurnJobStatus* outStatus) {
    o->size = (int)sizeof(BurnJobOptions3);
    return Burn_Simple(reinterpret_cast<const BurnJobOptions*>(o), outStatus);
}

} /* namespace */

int BurnAudioCd(const char* const* wavPaths, int trackCount,
                const char* devicePath, int speed, int verifyLevel) {
    try {
        if (!devicePath || !devicePath[0]) {
            SetBurnLastError("BurnAudioCd: devicePath is required");
            return BURN_ERROR_INVALID_PARAM;
        }
        if (!wavPaths || trackCount <= 0 || trackCount > 99) {
            SetBurnLastError("BurnAudioCd: wavPaths and a trackCount of 1..99 are required");
            return BURN_ERROR_INVALID_PARAM;
        }
        /* BurnAudioTrackC uses fixed 512-byte slots; reject an over-long
         * path rather than silently truncating to a missing file. */
        std::vector<BurnAudioTrackC> tracks((size_t)trackCount);
        for (int i = 0; i < trackCount; ++i) {
            const char* p = wavPaths[i];
            if (!p || !p[0]) {
                SetBurnLastError("BurnAudioCd: track " + std::to_string(i + 1) +
                                 " has a NULL path");
                return BURN_ERROR_INVALID_PARAM;
            }
            if (std::strlen(p) >= sizeof(tracks[0].audioPath)) {
                SetBurnLastError("BurnAudioCd: track path is longer than 511 characters");
                return BURN_ERROR_INVALID_PARAM;
            }
            std::memcpy(tracks[(size_t)i].audioPath, p, std::strlen(p) + 1);
            tracks[(size_t)i].isrc[0] = '\0';
            tracks[(size_t)i].preemphasis = 0;
        }

        BurnJobOptions3 o;
        std::memset(&o, 0, sizeof(o));
        o.mode = BURN_JOB_MODE_AUDIO_TAO;
        o.devicePath = devicePath;
        o.speed = speed;
        o.verifyLevel = verifyLevel;
        o.audioTracks = tracks.data();
        o.audioTrackCount = trackCount;
        return RunV3Simple(&o, nullptr);
    } catch (const std::exception& e) {
        SetBurnLastError(std::string("Internal exception: ") + e.what());
        return BURN_ERROR_EXCEPTION;
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return BURN_ERROR_EXCEPTION;
    }
}

int BurnEraseDisc(const char* devicePath, int fullErase) {
    try {
        if (!devicePath || !devicePath[0]) {
            SetBurnLastError("BurnEraseDisc: devicePath is required");
            return BURN_ERROR_INVALID_PARAM;
        }
        BurnJobOptions3 o;
        std::memset(&o, 0, sizeof(o));
        o.mode = BURN_JOB_MODE_ERASE;
        o.devicePath = devicePath;
        o.eraseFull = fullErase;
        return RunV3Simple(&o, nullptr);
    } catch (const std::exception& e) {
        SetBurnLastError(std::string("Internal exception: ") + e.what());
        return BURN_ERROR_EXCEPTION;
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return BURN_ERROR_EXCEPTION;
    }
}

int BurnAppendSession(const char* sourceDir, const char* devicePath,
                      const char* volumeLabel, int speed,
                      unsigned long long sessionStartBlock,
                      int leaveOpen) {
    try {
        if (!sourceDir || !sourceDir[0]) {
            SetBurnLastError("BurnAppendSession: sourceDir is required");
            return BURN_ERROR_INVALID_PARAM;
        }
        BurnJobOptions3 o;
        std::memset(&o, 0, sizeof(o));
        o.mode = BURN_JOB_MODE_APPEND;
        o.sourcePath = sourceDir;
        o.devicePath = devicePath;
        o.volumeLabel = volumeLabel;
        o.speed = speed;
        o.sessionStartBlock = sessionStartBlock;
        o.leaveOpen = leaveOpen;
        return RunV3Simple(&o, nullptr);
    } catch (const std::exception& e) {
        SetBurnLastError(std::string("Internal exception: ") + e.what());
        return BURN_ERROR_EXCEPTION;
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return BURN_ERROR_EXCEPTION;
    }
}

/* Byte-level read-back of a written disc against its source image. This is
 * the real verification VerifyDisc() could not offer; check
 * BURN_CAP_READBACK_VERIFY first. level: BURN_VERIFY_QUICK / _FULL. */
int Burn_VerifyWrite(const char* devicePath, const char* expectedImagePath, int level) {
    try {
        if (!devicePath || !devicePath[0] || !expectedImagePath || !expectedImagePath[0]) {
            SetBurnLastError("Burn_VerifyWrite: devicePath and expectedImagePath are required");
            return BURN_ERROR_INVALID_PARAM;
        }
        if (level != BURN_VERIFY_QUICK && level != BURN_VERIFY_FULL) {
            SetBurnLastError("Burn_VerifyWrite: level must be BURN_VERIFY_QUICK or BURN_VERIFY_FULL");
            return BURN_ERROR_INVALID_PARAM;
        }
        BurnJobOptions3 o;
        std::memset(&o, 0, sizeof(o));
        o.mode = BURN_JOB_MODE_VERIFY;
        o.sourcePath = expectedImagePath;
        o.devicePath = devicePath;
        o.verifyLevel = level;
        return RunV3Simple(&o, nullptr);
    } catch (const std::exception& e) {
        SetBurnLastError(std::string("Internal exception: ") + e.what());
        return BURN_ERROR_EXCEPTION;
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return BURN_ERROR_EXCEPTION;
    }
}

/* RAW PQ image building (drive-free) and RAW writing. */
int BurnCreateRawImage(const char* const* wavPaths, int trackCount,
                       const char* outputPath, const char* mediaCatalogNumber,
                       int startingTrackNumber) {
    try {
        if (!wavPaths || trackCount <= 0 || trackCount > 99 ||
            !outputPath || !outputPath[0]) {
            SetBurnLastError("BurnCreateRawImage: wavPaths (1..99) and outputPath are required");
            return BURN_ERROR_INVALID_PARAM;
        }
        std::vector<BurnAudioTrackC> tracks((size_t)trackCount);
        for (int i = 0; i < trackCount; ++i) {
            const char* p = wavPaths[i];
            if (!p || std::strlen(p) >= sizeof(tracks[0].audioPath)) {
                SetBurnLastError("BurnCreateRawImage: invalid track path");
                return BURN_ERROR_INVALID_PARAM;
            }
            std::memcpy(tracks[(size_t)i].audioPath, p, std::strlen(p) + 1);
            tracks[(size_t)i].isrc[0] = '\0';
            tracks[(size_t)i].preemphasis = 0;
        }
        BurnJobOptions3 o;
        std::memset(&o, 0, sizeof(o));
        o.mode = BURN_JOB_MODE_CREATE_RAW;
        o.outputPath = outputPath;
        o.audioTracks = tracks.data();
        o.audioTrackCount = trackCount;
        o.mediaCatalogNumber = mediaCatalogNumber;
        o.startingTrackNumber = startingTrackNumber;
        return RunV3Simple(&o, nullptr);
    } catch (const std::exception& e) {
        SetBurnLastError(std::string("Internal exception: ") + e.what());
        return BURN_ERROR_EXCEPTION;
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return BURN_ERROR_EXCEPTION;
    }
}

int BurnRawImageToDisc(const char* rawImagePath, const char* devicePath,
                       int speed, int verifyLevel) {
    try {
        if (!rawImagePath || !rawImagePath[0]) {
            SetBurnLastError("BurnRawImageToDisc: rawImagePath is required");
            return BURN_ERROR_INVALID_PARAM;
        }
        BurnJobOptions3 o;
        std::memset(&o, 0, sizeof(o));
        o.mode = BURN_JOB_MODE_BURN_RAW;
        o.sourcePath = rawImagePath;
        o.devicePath = devicePath;
        o.speed = speed;
        o.verifyLevel = verifyLevel;
        return RunV3Simple(&o, nullptr);
    } catch (const std::exception& e) {
        SetBurnLastError(std::string("Internal exception: ") + e.what());
        return BURN_ERROR_EXCEPTION;
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return BURN_ERROR_EXCEPTION;
    }
}

} /* extern "C" */

/* ---------------------------------------------------------------------------
 * C++ wrapper class (BurningUtility)
 * ------------------------------------------------------------------------- */
class BurningUtility::Impl {
public:
    Impl() = default;
};

BurningUtility::BurningUtility() : pImpl(new Impl()) {}
BurningUtility::~BurningUtility() { delete pImpl; }

bool BurningUtility::CreateISOToDisc(const std::string& isoPath, const std::string& devicePath,
                                    const std::string& discType, int speed) {
    return ::CreateISOToDisc(isoPath.c_str(), devicePath.c_str(), discType.c_str(), speed) == BURN_SUCCESS;
}

bool BurningUtility::BurnDirectoryToDisc(const std::string& sourceDir, const std::string& devicePath,
                                        const std::string& discType, int speed) {
    return ::BurnDirectoryToDisc(sourceDir.c_str(), devicePath.c_str(), discType.c_str(), speed) == BURN_SUCCESS;
}

bool BurningUtility::CreateISOImage(const std::string& sourcePath, const std::string& isoPath,
                                   const std::string& volumeLabel) {
    return ::CreateISOImage(sourcePath.c_str(), isoPath.c_str(), volumeLabel.c_str()) == BURN_SUCCESS;
}

bool BurningUtility::VerifyDisc(const std::string& devicePath, const std::string& isoPath) {
    return ::VerifyDisc(devicePath.c_str(), isoPath.c_str()) == BURN_SUCCESS;
}

std::vector<DeviceInfo> BurningUtility::ListBurningDevices() {
    DeviceInfo* devices = nullptr;
    int count = 0;
    std::vector<DeviceInfo> result;
    if (::ListBurningDevices(&devices, &count) == BURN_SUCCESS && devices) {
        for (int i = 0; i < count; ++i) result.push_back(devices[i]);
        ::FreeDeviceList(devices, count);
    }
    return result;
}

std::string BurningUtility::GetDefaultDevice() {
    char buffer[512];
    if (::GetDefaultDevice(buffer, sizeof(buffer)) == BURN_SUCCESS) return std::string(buffer);
    return "";
}

std::string BurningUtility::GetPlatformInfo() {
    const char* info = ::GetPlatformInfo();
    return info ? std::string(info) : "Unknown";
}

bool BurningUtility::CheckToolAvailability(const std::string& toolName) {
    return ::CheckToolAvailability(toolName.c_str()) == 1;
}

std::string BurningUtility::GetSupportedFormats() {
    const char* formats = ::GetSupportedFormats();
    return formats ? std::string(formats) : "";
}

bool BurningUtility::ValidateSourcePath(const std::string& path) {
    return ::ValidateSourcePath(path.c_str()) == 1;
}

void BurningUtility::SetDefaultDevice(const std::string& device) { ::SetDefaultDevice(device.c_str()); }
void BurningUtility::SetDefaultDiscType(const std::string& discType) { ::SetDefaultDiscType(discType.c_str()); }
void BurningUtility::SetDefaultSpeed(int speed) { ::SetDefaultSpeed(speed); }
void BurningUtility::SetProgressCallback(BurnProgressCallback callback, void* userData) {
    ::SetProgressCallback(callback, userData);
}
std::string BurningUtility::GetLastErrorMessage() {
    const char* msg = ::GetLastErrorMessage();
    return msg ? std::string(msg) : "";
}
void BurningUtility::ClearLastError() { ::ClearLastError(); }

/* ---------------------------------------------------------------------------
 * C++ RAII wrapper (BurnTask)
 * ------------------------------------------------------------------------- */
BurnTask::BurnTask() : handle_(BURN_INVALID_JOB_HANDLE) {}
BurnTask::BurnTask(const BurnJobOptions& options) : handle_(BURN_INVALID_JOB_HANDLE) { Start(options); }
BurnTask::~BurnTask() { Release(); }

BurnTask::BurnTask(BurnTask&& other) noexcept : handle_(other.handle_) {
    other.handle_ = BURN_INVALID_JOB_HANDLE;
}

BurnTask& BurnTask::operator=(BurnTask&& other) noexcept {
    if (this != &other) {
        Release();
        handle_ = other.handle_;
        other.handle_ = BURN_INVALID_JOB_HANDLE;
    }
    return *this;
}

bool BurnTask::Start(const BurnJobOptions& options) {
    Release();
    handle_ = Burn_Start(&options);
    return handle_ != BURN_INVALID_JOB_HANDLE;
}

bool BurnTask::IsValid() const { return handle_ != BURN_INVALID_JOB_HANDLE; }
BurnJobHandle BurnTask::Handle() const { return handle_; }

bool BurnTask::GetStatus(BurnJobStatus& status) const {
    return ::Burn_GetStatus(handle_, &status) == BURN_SUCCESS;
}

bool BurnTask::Cancel() { return ::Burn_Cancel(handle_) == BURN_SUCCESS; }
bool BurnTask::Wait(int timeoutMs) { return ::Burn_Wait(handle_, timeoutMs) == BURN_SUCCESS; }

bool BurnTask::IsRunning() const {
    BurnJobStatus status;
    std::memset(&status, 0, sizeof(status));
    status.size = (int)sizeof(status);
    if (!GetStatus(status)) return false;
    return status.state == BURN_JOB_PENDING || status.state == BURN_JOB_RUNNING;
}

int BurnTask::Result() const {
    BurnJobStatus status;
    std::memset(&status, 0, sizeof(status));
    status.size = (int)sizeof(status);
    if (!GetStatus(status)) return BURN_ERROR_INVALID_HANDLE;
    if (status.state == BURN_JOB_PENDING || status.state == BURN_JOB_RUNNING) {
        return BURN_ERROR_BUSY;
    }
    return status.errorCode;
}

void BurnTask::Release() {
    if (handle_ != BURN_INVALID_JOB_HANDLE) {
        ::Burn_Release(handle_);
        handle_ = BURN_INVALID_JOB_HANDLE;
    }
}
