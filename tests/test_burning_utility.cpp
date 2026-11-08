#include <iostream>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <chrono>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif
#include "../src/burning_utility.h"

/* CHECK() compiles its argument away under NDEBUG, which silently removes
 * any function call placed inside it. CI builds Release, so use a check that
 * ALWAYS evaluates and reports failures. */
static int g_failures = 0;
#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::cerr << "CHECK FAILED: " #expr " (line " << __LINE__ << ")\n"; \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

/* A caller callback that throws must not unwind through the DLL's C ABI. */
static void ThrowingProgressCallback(int, const char*, void*) {
    throw std::runtime_error("simulated caller exception");
}

static BurnJobOptions SelfTestOptions() {
    BurnJobOptions options;
    std::memset(&options, 0, sizeof(options));
    options.size = (int)sizeof(options);
    options.mode = BURN_JOB_MODE_SELFTEST;
    return options;
}

static BurnJobStatus EmptyStatus() {
    BurnJobStatus status;
    std::memset(&status, 0, sizeof(status));
    status.size = (int)sizeof(status);
    return status;
}

static void SleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static bool WaitForState(BurnJobHandle handle, int target, int maxMs) {
    for (int waited = 0; waited < maxMs; waited += 10) {
        BurnJobStatus status = EmptyStatus();
        if (Burn_GetStatus(handle, &status) == BURN_SUCCESS && status.state == target) {
            return true;
        }
        SleepMs(10);
    }
    return false;
}

static void TestLegacySurface() {
    const char* platformInfo = GetPlatformInfo();
    std::cout << "Platform: " << (platformInfo ? platformInfo : "Unknown") << std::endl;
    CHECK(platformInfo != nullptr);

    const char* formats = GetSupportedFormats();
    std::cout << "Supported formats: " << (formats ? formats : "Unknown") << std::endl;
    CHECK(formats != nullptr);

    std::cout << "  growisofs: " << (CheckToolAvailability("growisofs") ? "Available" : "Not Available") << std::endl;
    std::cout << "  mkisofs: " << (CheckToolAvailability("mkisofs") ? "Available" : "Not Available") << std::endl;

    DeviceInfo* devices = nullptr;
    int deviceCount = 0;
    if (ListBurningDevices(&devices, &deviceCount) == BURN_SUCCESS) {
        std::cout << "Device count: " << deviceCount << std::endl;
        if (devices) FreeDeviceList(devices, deviceCount);
    }

    BurningUtility util;
    std::cout << "Platform (C++): " << util.GetPlatformInfo() << std::endl;
    ClearLastError();
    std::cout << "Last error: '" << GetLastErrorMessage() << "'" << std::endl;

    /* Exercise the legacy exception boundary: VerifyDisc() fires a global
     * progress callback which throws here. Since the disc read-back fix,
     * a present-but-unverifiable image returns UNSUPPORTED_FORMAT instead
     * of the misleading SUCCESS; the callback still ran and threw, and the
     * call returning normally proves containment. */
    const char* probeIso = "burn_verify_probe.iso";
    FILE* probe = fopen(probeIso, "wb");
    CHECK(probe != nullptr);
    fwrite("ISO-PROBE", 1, 9, probe);
    fclose(probe);

    SetProgressCallback(ThrowingProgressCallback, nullptr);
    int verifyResult = VerifyDisc("probe-device", probeIso);
    SetProgressCallback(nullptr, nullptr);
    std::cout << "VerifyDisc no-readback honesty: rc=" << verifyResult
              << " err='" << GetLastErrorMessage() << "'" << std::endl;
    CHECK(verifyResult == BURN_ERROR_UNSUPPORTED_FORMAT);
    CHECK(std::strlen(GetLastErrorMessage()) > 0);

    /* A missing image file is still a parameter error, not "unsupported". */
    CHECK(VerifyDisc("probe-device", "burn_definitely_absent.iso")
          == BURN_ERROR_INVALID_PARAM);
    remove(probeIso);
    std::cout << "Throwing legacy callback contained: yes (call returned)" << std::endl;
}

static void TestValidationAndFailure() {
    std::cout << "\n=== Job API: validation and failure ===" << std::endl;
    CHECK(Burn_GetApiVersion() == BURN_API_VERSION);

    CHECK(Burn_Start(nullptr) == BURN_INVALID_JOB_HANDLE);
    CHECK(std::strlen(GetLastErrorMessage()) > 0);

    BurnJobOptions bad = SelfTestOptions();
    bad.size = 1;
    CHECK(Burn_Start(&bad) == BURN_INVALID_JOB_HANDLE);

    bad = SelfTestOptions();
    bad.mode = 99;
    CHECK(Burn_Start(&bad) == BURN_INVALID_JOB_HANDLE);

    BurnJobStatus status = EmptyStatus();
    CHECK(Burn_GetStatus(BURN_INVALID_JOB_HANDLE, &status) == BURN_ERROR_INVALID_HANDLE);
    CHECK(Burn_Cancel(BURN_INVALID_JOB_HANDLE) == BURN_ERROR_INVALID_HANDLE);
    CHECK(Burn_Wait(BURN_INVALID_JOB_HANDLE, 0) == BURN_ERROR_INVALID_HANDLE);
    CHECK(Burn_Release(BURN_INVALID_JOB_HANDLE) == BURN_ERROR_INVALID_HANDLE);

    /* A missing source must end as FAILED with a captured error message,
     * not hang, crash, or leak a handle. */
    BurnJobOptions options;
    std::memset(&options, 0, sizeof(options));
    options.size = (int)sizeof(options);
    options.mode = BURN_JOB_MODE_IMAGE;
    options.sourcePath = "burn_missing_source_probe.iso";
    options.devicePath = "probe-device";
    options.discType = "CD-R";
    options.speed = 4;
    remove(options.sourcePath);

    BurnJobHandle handle = Burn_Start(&options);
    CHECK(handle != BURN_INVALID_JOB_HANDLE);
    CHECK(Burn_Wait(handle, 5000) == BURN_SUCCESS);
    status = EmptyStatus();
    CHECK(Burn_GetStatus(handle, &status) == BURN_SUCCESS);
    std::cout << "Failed job: state=" << status.state
              << " code=" << status.errorCode
              << " error='" << status.errorMessage << "'" << std::endl;
    CHECK(status.state == BURN_JOB_FAILED);
    CHECK(status.errorCode == BURN_ERROR_INVALID_PARAM);
    CHECK(std::strlen(status.errorMessage) > 0);
    CHECK(std::strlen(Burn_GetJobErrorMessage(handle)) > 0);
    CHECK(Burn_Release(handle) == BURN_SUCCESS);

    /* Released handles must be rejected, never dereferenced. */
    status = EmptyStatus();
    CHECK(Burn_GetStatus(handle, &status) == BURN_ERROR_INVALID_HANDLE);
    CHECK(Burn_Release(handle) == BURN_ERROR_INVALID_HANDLE);
    CHECK(Burn_GetActiveJobCount() == 0);
}

static void TestCancellation() {
    std::cout << "\n=== Job API: cancellation and timeout ===" << std::endl;

    /* Cancel before the worker has had a chance to do any real work. */
    BurnJobOptions options = SelfTestOptions();
    BurnJobHandle handle = Burn_Start(&options);
    CHECK(handle != BURN_INVALID_JOB_HANDLE);
    CHECK(Burn_Cancel(handle) == BURN_SUCCESS);
    CHECK(Burn_Wait(handle, 5000) == BURN_SUCCESS);
    BurnJobStatus status = EmptyStatus();
    CHECK(Burn_GetStatus(handle, &status) == BURN_SUCCESS);
    std::cout << "Early cancel: state=" << status.state << " code=" << status.errorCode << std::endl;
    CHECK(status.state == BURN_JOB_CANCELLED);
    CHECK(status.errorCode == BURN_ERROR_CANCELLED);
    CHECK(status.cancelRequested == 1);
    CHECK(Burn_Release(handle) == BURN_SUCCESS);

    /* Cancel an actually running job and check Burn_Wait(0) times out. */
    options = SelfTestOptions();
    handle = Burn_Start(&options);
    CHECK(handle != BURN_INVALID_JOB_HANDLE);
    CHECK(WaitForState(handle, BURN_JOB_RUNNING, 2000));
    CHECK(Burn_Wait(handle, 0) == BURN_ERROR_TIMEOUT);
    status = EmptyStatus();
    CHECK(Burn_GetStatus(handle, &status) == BURN_SUCCESS);
    CHECK(status.state == BURN_JOB_RUNNING);
    std::cout << "Running job: " << status.percent << "% phase=" << status.phase << std::endl;
    SleepMs(80);
    CHECK(Burn_Cancel(handle) == BURN_SUCCESS);
    CHECK(Burn_Wait(handle, 5000) == BURN_SUCCESS);
    status = EmptyStatus();
    CHECK(Burn_GetStatus(handle, &status) == BURN_SUCCESS);
    std::cout << "Mid-run cancel: state=" << status.state
              << " at " << status.percent << "%" << std::endl;
    CHECK(status.state == BURN_JOB_CANCELLED);
    CHECK(status.percent < 100);
    CHECK(Burn_Release(handle) == BURN_SUCCESS);
}

static void TestSuccessAndExceptionContainment() {
    std::cout << "\n=== Job API: success / exception containment ===" << std::endl;

    BurnJobOptions options = SelfTestOptions();
    BurnJobStatus out = EmptyStatus();
    int result = Burn_Simple(&options, &out);
    std::cout << "Burn_Simple(selftest): rc=" << result
              << " state=" << out.state << " percent=" << out.percent
              << " elapsed=" << out.elapsedMs << "ms" << std::endl;
    CHECK(result == BURN_SUCCESS);
    CHECK(out.state == BURN_JOB_COMPLETED);
    CHECK(out.percent == 100);
    CHECK(out.errorCode == BURN_SUCCESS);

    /* A throwing per-job callback must be swallowed by the job engine. */
    options = SelfTestOptions();
    options.progressCallback = ThrowingProgressCallback;
    BurnJobHandle handle = Burn_Start(&options);
    CHECK(handle != BURN_INVALID_JOB_HANDLE);
    CHECK(WaitForState(handle, BURN_JOB_RUNNING, 2000));
    SleepMs(60);
    CHECK(Burn_Cancel(handle) == BURN_SUCCESS);
    CHECK(Burn_Wait(handle, 5000) == BURN_SUCCESS);
    BurnJobStatus status = EmptyStatus();
    CHECK(Burn_GetStatus(handle, &status) == BURN_SUCCESS);
    std::cout << "Throwing job callback contained: state=" << status.state
              << " code=" << status.errorCode << std::endl;
    CHECK(status.state == BURN_JOB_CANCELLED);
    CHECK(status.errorCode == BURN_ERROR_CANCELLED);
    CHECK(Burn_Release(handle) == BURN_SUCCESS);
}

static void TestCppWrapperAndLegacyCancel() {
    std::cout << "\n=== Job API: C++ RAII / CancelCurrentBurn ===" << std::endl;

    BurnTask task;
    BurnJobOptions options = SelfTestOptions();
    CHECK(task.Start(options));
    CHECK(task.IsValid());
    CHECK(Burn_Cancel(task.Handle()) == BURN_SUCCESS);
    CHECK(task.Wait(5000));
    BurnJobStatus status = EmptyStatus();
    CHECK(task.GetStatus(status));
    CHECK(status.state == BURN_JOB_CANCELLED);
    CHECK(task.Result() == BURN_ERROR_CANCELLED);
    CHECK(!task.IsRunning());

    BurnTask moved(std::move(task));
    CHECK(moved.IsValid());
    CHECK(!task.IsValid());
    moved.Release();
    CHECK(!moved.IsValid());

    /* The legacy synchronous API is now cancelable from another thread. */
    std::atomic<int> legacyResult{-99};
    BurnJobOptions simple = SelfTestOptions();
    std::thread worker([&simple, &legacyResult]() {
        legacyResult.store(Burn_Simple(&simple, nullptr));
    });

    bool active = false;
    for (int i = 0; i < 200 && !active; ++i) {
        active = Burn_GetActiveJobCount() == 1;
        if (!active) SleepMs(10);
    }
    CHECK(active);
    SleepMs(60);
    CHECK(CancelCurrentBurn() == BURN_SUCCESS);
    worker.join();
    std::cout << "Legacy Burn_Simple cancelled: rc=" << legacyResult.load() << std::endl;
    CHECK(legacyResult.load() == BURN_ERROR_CANCELLED);
    CHECK(Burn_GetActiveJobCount() == 0);
}

/* Regression for the process-wide default configuration.
 *
 * SetDefault* used to be thread_local: a job started from a worker thread
 * silently saw "no default device" and failed, even though the main thread
 * had configured one. The defaults must now be visible on every thread. */
static void TestProcessWideDefaults() {
    std::cout << "\n=== Defaults are process-wide (cross-thread) ===" << std::endl;

    SetDefaultDevice("Z:\\configured-on-main");
    SetDefaultDiscType("DVD+R");
    SetDefaultSpeed(8);

    /* Read them back from a foreign thread. */
    bool deviceVisible = false;
    bool errThreadLocalStillWorks = false;
    std::thread worker([&]() {
        char buf[512];
        buf[0] = '\0';
        deviceVisible = (GetDefaultDevice(buf, sizeof(buf)) == BURN_SUCCESS)
                        && std::strcmp(buf, "Z:\\configured-on-main") == 0;
        /* Last-error stays per-thread: setting it here must not leak. */
        Burn_GetStatus(BURN_INVALID_JOB_HANDLE, nullptr);
        errThreadLocalStillWorks = (std::strlen(GetLastErrorMessage()) > 0);
    });
    worker.join();

    std::cout << "Device default visible on worker: " << (deviceVisible ? "yes" : "NO") << std::endl;
    CHECK(deviceVisible);
    CHECK(errThreadLocalStillWorks);

    /* A job started on a worker with devicePath=NULL must inherit the
     * default instead of failing with "No burning device specified".
     * The source file EXISTS (so validation passes) but the default device
     * is fake: the job must get PAST the device check and fail later, at
     * the backend, proving the worker saw the main thread's default. */
    {
        FILE* src = fopen("burn_default_inherit.iso", "wb");
        CHECK(src != nullptr);
        if (src) { fwrite("PROBE-ISO", 1, 9, src); fclose(src); }
    }
    std::atomic<int> rc{999};
    std::string errMsg;
    std::thread worker2([&]() {
        BurnJobOptions options;
        std::memset(&options, 0, sizeof(options));
        options.size = (int)sizeof(options);
        options.mode = BURN_JOB_MODE_IMAGE;
        options.sourcePath = "burn_default_inherit.iso";
        options.devicePath = nullptr;  /* use the configured default */
        BurnJobStatus st;
        std::memset(&st, 0, sizeof(st));
        st.size = (int)sizeof(st);
        rc.store(Burn_Simple(&options, &st));
        errMsg = st.errorMessage;
    });
    worker2.join();
    remove("burn_default_inherit.iso");
    std::cout << "Worker job rc=" << rc.load() << " err='" << errMsg << "'" << std::endl;
    CHECK(errMsg.find("No burning device") == std::string::npos);
    /* DEVICE_NOT_FOUND (-2) here would mean the default was NOT inherited:
     * that is the exact pre-fix signature. */
    CHECK(rc.load() != BURN_ERROR_DEVICE_NOT_FOUND);

    /* Reset so later tests see a clean slate. */
    SetDefaultDevice("");
}

/* Regression for absolute-path normalisation at the API boundary.
 *
 * libisofs rejects RELATIVE source directories while IMAPI2 accepts them,
 * so CreateISOImage("./dir") worked on Windows and failed on Linux. The
 * engine now normalises paths before handing them to a backend. */
static void TestRelativeSourcePath() {
    std::cout << "\n=== Relative source path works on every backend ===" << std::endl;

#ifdef _WIN32
    _mkdir("burn_rel_dir");
#else
    mkdir("burn_rel_dir", 0777);
#endif
    FILE* f = fopen("burn_rel_dir/inside.txt", "wb");
    if (!f) {
        std::cout << "SKIP: cannot create the relative test directory" << std::endl;
        return;
    }
    fwrite("payload", 1, 7, f);
    fclose(f);

    BurnJobOptions options;
    std::memset(&options, 0, sizeof(options));
    options.size = (int)sizeof(options);
    options.mode = BURN_JOB_MODE_CREATE_IMAGE;
    options.sourcePath = "burn_rel_dir";          /* deliberately RELATIVE */
    options.outputPath = "burn_rel_out.iso";
    options.volumeLabel = "RELATIVE";

    BurnJobStatus st;
    std::memset(&st, 0, sizeof(st));
    st.size = (int)sizeof(st);
    int rc = Burn_Simple(&options, &st);
    std::cout << "CreateISOImage(relative src) rc=" << rc
              << " err='" << st.errorMessage << "'" << std::endl;

    if (rc == BURN_SUCCESS) {
        std::cout << "  image created from a relative path" << std::endl;
    } else {
        /* Environment may lack a working image writer (unwritable TEMP,
         * missing COM component). That is acceptable here, but the old
         * relative-path rejection signature must NEVER come back. */
        CHECK(std::string(st.errorMessage).find("add source directory")
              == std::string::npos);
        std::cout << "  non-path failure tolerated: " << st.errorMessage << std::endl;
    }
    CHECK(rc == BURN_SUCCESS || rc == BURN_ERROR_ISO_CREATION_FAILED);

    remove("burn_rel_out.iso");
    remove("burn_rel_dir/inside.txt");
#ifdef _WIN32
    _rmdir("burn_rel_dir");
#else
    rmdir("burn_rel_dir");
#endif
}

int main() {
    std::cout << "=== Burning Utility Unit Tests ===" << std::endl;

    TestLegacySurface();
    TestValidationAndFailure();
    TestCancellation();
    TestSuccessAndExceptionContainment();
    TestCppWrapperAndLegacyCancel();
    TestProcessWideDefaults();
    TestRelativeSourcePath();

    if (g_failures != 0) {
        std::cout << "\n" << g_failures << " CHECK(s) FAILED" << std::endl;
        return 1;
    }
    std::cout << "\n=== All tests completed ===" << std::endl;
    std::cout << "SUCCESS" << std::endl;
    return 0;
}
