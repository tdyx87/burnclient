#include <iostream>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <chrono>
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

    /* Exercise the legacy exception boundary: VerifyDisc() fires two global
     * progress callbacks, both of which throw here. */
    const char* probeIso = "burn_verify_probe.iso";
    FILE* probe = fopen(probeIso, "wb");
    CHECK(probe != nullptr);
    fwrite("ISO-PROBE", 1, 9, probe);
    fclose(probe);

    SetProgressCallback(ThrowingProgressCallback, nullptr);
    int verifyResult = VerifyDisc("probe-device", probeIso);
    SetProgressCallback(nullptr, nullptr);
    remove(probeIso);
    std::cout << "Throwing legacy callback contained: "
              << (verifyResult == BURN_SUCCESS ? "yes" : "no") << std::endl;
    CHECK(verifyResult == BURN_SUCCESS);
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

int main() {
    std::cout << "=== Burning Utility Unit Tests ===" << std::endl;

    TestLegacySurface();
    TestValidationAndFailure();
    TestCancellation();
    TestSuccessAndExceptionContainment();
    TestCppWrapperAndLegacyCancel();

    if (g_failures != 0) {
        std::cout << "\n" << g_failures << " CHECK(s) FAILED" << std::endl;
        return 1;
    }
    std::cout << "\n=== All tests completed ===" << std::endl;
    std::cout << "SUCCESS" << std::endl;
    return 0;
}
