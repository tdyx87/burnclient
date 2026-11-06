#ifndef BURNING_UTILITY_H
#define BURNING_UTILITY_H

/* C++ standard headers are only valid for C++ consumers. A pure C consumer
 * must be able to #include this header unchanged. */
#ifdef __cplusplus
#include <string>
#include <vector>
#include <memory>
#endif

/* DeviceInfo struct - plain C-compatible, safe across DLL boundary */
typedef struct {
    char devicePath[512];
    char deviceName[256];
    char mediaType[64];
    int isWritable;
    int isEmpty;
} DeviceInfo;

#ifdef _WIN32
    #ifdef BUILDING_DLL
        #define DLL_API __declspec(dllexport)
    #else
        #define DLL_API __declspec(dllimport)
    #endif
#else
    #define DLL_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
/* ---------------------------------------------------------------------------
 * Platform abstraction interface (C++ only, internal - not exported)
 *
 * Note: IPlatform uses std::string/std::vector in its interface, which are
 * ABI-sensitive. It is NOT marked DLL_API because it is only used internally
 * via CreatePlatformImpl() within the DLL. Callers interact through the
 * C API or the BurningUtility wrapper class.
 * ------------------------------------------------------------------------- */
class IPlatform {
public:
    virtual ~IPlatform() = default;
    virtual std::vector<DeviceInfo> ListDevices() = 0;
    virtual bool BurnISO(const std::string& isoPath, const std::string& devicePath,
                         const std::string& discType, int speed) = 0;
    virtual bool CreateISO(const std::string& sourcePath, const std::string& isoPath,
                          const std::string& volumeLabel) = 0;
    virtual std::string GetPlatformInfo() = 0;
    virtual bool CheckToolAvailability(const std::string& toolName) = 0;
};

/* Internal factory - called only within the DLL, not exported */
std::unique_ptr<IPlatform> CreatePlatformImpl();
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define BURN_SUCCESS 0
#define BURN_ERROR_INVALID_PARAM -1
#define BURN_ERROR_DEVICE_NOT_FOUND -2
#define BURN_ERROR_NO_SPACE -3
#define BURN_ERROR_BURN_FAILED -4
#define BURN_ERROR_ISO_CREATION_FAILED -5
#define BURN_ERROR_TOOL_NOT_AVAILABLE -6
#define BURN_ERROR_PERMISSION_DENIED -7
#define BURN_ERROR_UNSUPPORTED_FORMAT -8
/* Reserved for the handle-based job API (Burn_Start / GetStatus / Cancel /
 * Wait) and for the exception boundary around every exported C function. */
#define BURN_ERROR_EXCEPTION -9
#define BURN_ERROR_CANCELLED -10
#define BURN_ERROR_TIMEOUT -11
#define BURN_ERROR_BUSY -12
#define BURN_ERROR_INVALID_HANDLE -13

/* BURN_API_VERSION: 1 = legacy synchronous API only,
 *                   2 = legacy API + handle-based job API. */
#define BURN_API_VERSION 2

typedef void (*BurnProgressCallback)(int percent, const char* status, void* userData);

DLL_API int CreateISOToDisc(const char* isoPath, const char* devicePath, const char* discType, int speed);
DLL_API int BurnDirectoryToDisc(const char* sourceDir, const char* devicePath, const char* discType, int speed);
DLL_API int CreateISOImage(const char* sourcePath, const char* isoPath, const char* volumeLabel);
DLL_API int VerifyDisc(const char* devicePath, const char* isoPath);

DLL_API int ListBurningDevices(DeviceInfo** devices, int* deviceCount);
DLL_API void FreeDeviceList(DeviceInfo* devices, int deviceCount);
DLL_API int GetDefaultDevice(char* devicePath, int bufferSize);

DLL_API const char* GetPlatformInfo();
DLL_API int CheckToolAvailability(const char* toolName);
DLL_API const char* GetSupportedFormats();
DLL_API int ValidateSourcePath(const char* path);
DLL_API void SetDefaultDevice(const char* device);
DLL_API void SetDefaultDiscType(const char* discType);
DLL_API void SetDefaultSpeed(int speed);

DLL_API const char* GetLastErrorMessage();
DLL_API void ClearLastError();

/* FreeStringMemory: only for strings that were dynamically allocated
 * and returned as non-const char* from the DLL. Do NOT use on return
 * values of GetPlatformInfo(), GetSupportedFormats(), or
 * GetLastErrorMessage() - those return pointers to internal static
 * buffers and must not be freed. */
DLL_API void FreeStringMemory(void* ptr);

DLL_API void SetProgressCallback(BurnProgressCallback callback, void* userData);

/* ---------------------------------------------------------------------------
 * Handle-based, asynchronous burn jobs
 *
 * The legacy functions above are synchronous and cannot be observed or
 * cancelled from another thread. Burn_Start() instead returns a job handle
 * immediately and runs the work on an internal worker thread. The caller can
 * poll Burn_GetStatus(), cancel with Burn_Cancel(), block with Burn_Wait(),
 * and must eventually destroy the handle with Burn_Release().
 *
 * All strings in BurnJobOptions are copied by Burn_Start(), so the caller may
 * free them as soon as it returns. BurnJobStatus is a plain C snapshot with
 * no pointers: it stays valid after Burn_Release().
 * ------------------------------------------------------------------------- */

typedef unsigned long long BurnJobHandle;
#define BURN_INVALID_JOB_HANDLE ((BurnJobHandle)0)

/* BurnJobStatus.state */
#define BURN_JOB_PENDING   0
#define BURN_JOB_RUNNING   1
#define BURN_JOB_COMPLETED 2
#define BURN_JOB_FAILED    3
#define BURN_JOB_CANCELLED 4

/* BurnJobOptions.mode */
#define BURN_JOB_MODE_IMAGE        0  /* burn an existing image file      */
#define BURN_JOB_MODE_DIRECTORY    1  /* build an ISO from a dir, then burn */
#define BURN_JOB_MODE_CREATE_IMAGE 2  /* build an ISO only, no drive used   */
#define BURN_JOB_MODE_SELFTEST     3  /* diagnostic: cancellable no-hardware
                                      * job used to validate an integration */

typedef struct BurnJobOptions {
    int  size;                     /* set to sizeof(BurnJobOptions) */
    int  mode;                     /* BURN_JOB_MODE_* */
    const char* sourcePath;        /* image file or source directory */
    const char* outputPath;        /* BURN_JOB_MODE_CREATE_IMAGE only */
    const char* devicePath;        /* NULL/"" -> configured default device */
    const char* discType;          /* NULL/"" -> configured default disc type */
    const char* volumeLabel;       /* NULL/"" -> "DISC" */
    int  speed;                    /* <= 0 -> configured default speed */
    int  verify;                   /* reserved, set to 0 */
    BurnProgressCallback progressCallback; /* per-job progress, may be NULL */
    void* userData;
} BurnJobOptions;

typedef struct BurnJobStatus {
    int  size;                     /* set to sizeof(BurnJobStatus) */
    int  state;                    /* BURN_JOB_* */
    int  percent;                  /* 0..100, monotonic within a job */
    int  errorCode;                /* BURN_SUCCESS / BURN_ERROR_* */
    int  cancelRequested;
    char phase[64];                /* e.g. "creating-image", "burning" */
    char message[256];             /* last human-readable progress message */
    char errorMessage[512];        /* populated on failure/cancellation */
    unsigned long long elapsedMs;
} BurnJobStatus;

/* Returns BURN_INVALID_JOB_HANDLE on invalid options; use
 * GetLastErrorMessage() for the reason. */
DLL_API BurnJobHandle Burn_Start(const BurnJobOptions* options);

/* Snapshot the job. Returns BURN_ERROR_INVALID_HANDLE once released. */
DLL_API int Burn_GetStatus(BurnJobHandle job, BurnJobStatus* status);

/* Best-effort, thread-safe cancellation. Returns BURN_SUCCESS for a valid
 * handle even if the job has already finished. */
DLL_API int Burn_Cancel(BurnJobHandle job);

/* timeoutMs < 0 waits indefinitely; 0 only polls. Returns BURN_SUCCESS once
 * the job is terminal, BURN_ERROR_TIMEOUT if it is still running. */
DLL_API int Burn_Wait(BurnJobHandle job, int timeoutMs);

/* Invalidates the handle. A still-running job is asked to cancel and its
 * worker thread is detached; every handle must be released exactly once. */
DLL_API int Burn_Release(BurnJobHandle job);

/* Convenience: Start + Wait + snapshot + Release in one blocking call.
 * Returns the job's errorCode (BURN_SUCCESS on success). */
DLL_API int Burn_Simple(const BurnJobOptions* options, BurnJobStatus* outStatus);

/* Pointer to an internal thread-local buffer; do NOT free. */
DLL_API const char* Burn_GetJobErrorMessage(BurnJobHandle job);

/* Cancel the in-flight Burn_Simple() call, from any thread. */
DLL_API int CancelCurrentBurn(void);

DLL_API int Burn_GetActiveJobCount(void);
DLL_API int Burn_GetApiVersion(void);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <string>
#include <vector>

/* C++ convenience wrapper class.
 * Uses pImpl idiom to hide implementation details and avoid exporting
 * STL types across the DLL boundary. */
class DLL_API BurningUtility {
public:
    BurningUtility();
    ~BurningUtility();

    bool CreateISOToDisc(const std::string& isoPath, const std::string& devicePath,
                        const std::string& discType = "CD-R", int speed = 4);
    bool BurnDirectoryToDisc(const std::string& sourceDir, const std::string& devicePath,
                            const std::string& discType = "CD-R", int speed = 4);
    bool CreateISOImage(const std::string& sourcePath, const std::string& isoPath,
                       const std::string& volumeLabel = "DISC");
    bool VerifyDisc(const std::string& devicePath, const std::string& isoPath);

    std::vector<DeviceInfo> ListBurningDevices();
    std::string GetDefaultDevice();
    std::string GetPlatformInfo();

    bool CheckToolAvailability(const std::string& toolName);
    std::string GetSupportedFormats();
    bool ValidateSourcePath(const std::string& path);

    void SetDefaultDevice(const std::string& device);
    void SetDefaultDiscType(const std::string& discType);
    void SetDefaultSpeed(int speed);
    void SetProgressCallback(BurnProgressCallback callback, void* userData);

    std::string GetLastErrorMessage();
    void ClearLastError();

private:
    class Impl;
    Impl* pImpl;
};

/* RAII wrapper around the handle-based job API. The destructor releases the
 * handle, so no Burn_Release() call is needed. Move-only. */
class DLL_API BurnTask {
public:
    BurnTask();
    explicit BurnTask(const BurnJobOptions& options);
    ~BurnTask();

    BurnTask(const BurnTask&) = delete;
    BurnTask& operator=(const BurnTask&) = delete;
    BurnTask(BurnTask&& other) noexcept;
    BurnTask& operator=(BurnTask&& other) noexcept;

    bool Start(const BurnJobOptions& options);
    bool IsValid() const;
    BurnJobHandle Handle() const;
    bool GetStatus(BurnJobStatus& status) const;
    bool Cancel();
    bool Wait(int timeoutMs = -1);
    bool IsRunning() const;
    int  Result() const;          /* errorCode of the terminal state */
    void Release();

private:
    BurnJobHandle handle_;
};
#endif

#endif
