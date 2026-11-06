#include "burning_utility.h"
#include "burn_internal.h"
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

/* Thread-local error string and defaults. Defaults are read on the thread
 * that calls Burn_Start(), then snapshotted into the job. */
static thread_local std::string g_lastError;
static thread_local std::string g_defaultDevice = "";
static thread_local std::string g_defaultDiscType = "CD-R";
static thread_local int g_defaultSpeed = 4;

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

/* Cross-platform temporary file path generation */
static std::string GenerateTempISOPath() {
    std::random_device rd;
    std::uniform_int_distribution<int> dist(100000, 999999);

#if defined(_WIN32)
    char tempDir[MAX_PATH] = {0};
    GetTempPathA(MAX_PATH, tempDir);
    if (tempDir[0] == '\0') {
        strncpy(tempDir, ".", sizeof(tempDir) - 1);
    }
    char buf[600];
    snprintf(buf, sizeof(buf), "%s\\temp_burn_%d.iso", tempDir, dist(rd));
    return std::string(buf);
#else
    const char* tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = getenv("TEMP");
    if (!tmpdir) tmpdir = getenv("TMP");
    if (!tmpdir) tmpdir = "/tmp";
    char buf[600];
    snprintf(buf, sizeof(buf), "%s/temp_burn_%d.iso", tmpdir, dist(rd));
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
 * BurnJob — internal state machine
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
    NotifyProgressBurn(2, "Validating source...");
    if (IsBurnCancelled()) return BURN_ERROR_CANCELLED;

    if (job.sourcePath.empty() || !PathExists(job.sourcePath.c_str())) {
        SetBurnLastError("Source path not found: " + job.sourcePath);
        return BURN_ERROR_INVALID_PARAM;
    }

    IPlatform* platform = GetPlatform();
    if (!platform) {
        SetBurnLastError("Failed to create platform implementation");
        return BURN_ERROR_UNSUPPORTED_FORMAT;
    }

    if (job.mode == BURN_JOB_MODE_CREATE_IMAGE) {
        if (job.outputPath.empty()) {
            SetBurnLastError("Output ISO path is required");
            return BURN_ERROR_INVALID_PARAM;
        }
        job.SetPhase("creating-image");
        NotifyProgressBurn(5, "Creating ISO image...");
        if (IsBurnCancelled()) return BURN_ERROR_CANCELLED;
        if (!platform->CreateISO(job.sourcePath, job.outputPath, job.volumeLabel)) {
            return BURN_ERROR_ISO_CREATION_FAILED;
        }
        NotifyProgressBurn(100, "ISO creation completed");
        return BURN_SUCCESS;
    }

    if (job.devicePath.empty()) {
        SetBurnLastError("No burning device specified");
        return BURN_ERROR_DEVICE_NOT_FOUND;
    }

    std::string isoPath = job.sourcePath;
    TempIsoGuard temp;
    if (job.mode == BURN_JOB_MODE_DIRECTORY) {
        temp.path = GenerateTempISOPath();
        job.SetProgressRange(0, 40);
        job.SetPhase("creating-image");
        if (!platform->CreateISO(job.sourcePath, temp.path, job.volumeLabel)) {
            return BURN_ERROR_ISO_CREATION_FAILED;
        }
        isoPath = temp.path;
        if (IsBurnCancelled()) return BURN_ERROR_CANCELLED;
        job.SetProgressRange(40, 60);
    }

    job.SetPhase("burning");
    if (IsBurnCancelled()) return BURN_ERROR_CANCELLED;
    bool burned = platform->BurnISO(isoPath, job.devicePath, job.discType, job.speed);
    if (IsBurnCancelled()) return BURN_ERROR_CANCELLED;
    if (!burned) return BURN_ERROR_BURN_FAILED;

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
        fclose(isoFile);
        if (isoSize <= 0) {
            SetBurnLastError("ISO file is empty");
            return BURN_ERROR_ISO_CREATION_FAILED;
        }
        NotifyLegacyProgress(50, "Verifying disc...");
        NotifyLegacyProgress(100, "Verification completed (basic check)");
        /* TODO: byte-level read-back verification (needs platform read support). */
        return BURN_SUCCESS;
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
        if (g_defaultDevice.empty()) {
            devicePath[0] = '\0';
            return BURN_ERROR_DEVICE_NOT_FOUND;
        }
        strncpy(devicePath, g_defaultDevice.c_str(), bufferSize - 1);
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
        /* A string literal — callers must NOT call FreeStringMemory on it. */
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
        if (device) g_defaultDevice = device;
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
    }
}

void SetDefaultDiscType(const char* discType) {
    try {
        if (discType) g_defaultDiscType = discType;
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
    }
}

void SetDefaultSpeed(int speed) {
    try {
        if (speed > 0) g_defaultSpeed = speed;
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
        if (options->size != (int)sizeof(BurnJobOptions)) {
            SetBurnLastError("Burn_Start: set options.size to sizeof(BurnJobOptions)");
            return BURN_INVALID_JOB_HANDLE;
        }
        if (options->mode < BURN_JOB_MODE_IMAGE || options->mode > BURN_JOB_MODE_SELFTEST) {
            SetBurnLastError("Burn_Start: unknown job mode");
            return BURN_INVALID_JOB_HANDLE;
        }
        if (options->mode != BURN_JOB_MODE_SELFTEST &&
            (!options->sourcePath || !options->sourcePath[0])) {
            SetBurnLastError("Burn_Start: sourcePath is required");
            return BURN_INVALID_JOB_HANDLE;
        }
        if (options->mode == BURN_JOB_MODE_CREATE_IMAGE &&
            (!options->outputPath || !options->outputPath[0])) {
            SetBurnLastError("Burn_Start: outputPath is required for CREATE_IMAGE");
            return BURN_INVALID_JOB_HANDLE;
        }

        std::shared_ptr<BurnJob> job = std::make_shared<BurnJob>();
        job->mode = options->mode;
        job->sourcePath = options->sourcePath ? options->sourcePath : "";
        job->outputPath = options->outputPath ? options->outputPath : "";
        job->devicePath = options->devicePath ? options->devicePath : "";
        job->discType = options->discType ? options->discType : "";
        job->volumeLabel = options->volumeLabel ? options->volumeLabel : "";
        if (job->devicePath.empty()) job->devicePath = g_defaultDevice;
        if (job->discType.empty()) job->discType = g_defaultDiscType;
        if (job->volumeLabel.empty()) job->volumeLabel = "DISC";
        job->speed = options->speed > 0 ? options->speed : g_defaultSpeed;
        job->verify = options->verify;
        job->callback = options->progressCallback;
        job->userData = options->userData;
        job->startMs = NowMs();

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
