#ifndef BURNING_UTILITY_H
#define BURNING_UTILITY_H

/* C++ standard headers are only valid for C++ consumers. A pure C consumer
 * must be able to #include this header unchanged. */
#ifdef __cplusplus
#include <string>
#include <vector>
#include <memory>
#endif

/* DeviceInfo struct - plain C-compatible, safe across DLL boundary
 *
 * isWritable: truthful capability report from the backend.
 *   - Windows: 1 when IMAPI2 could exclusively open the recorder, 0 when the
 *     driver is write-protected or the device is busy (see deviceName).
 *   - Linux: from the MMC capability bits of the drive scan; DVD+R/DVD+RW/
 *     DVD-RW/BD-only recorders may report 0 there (documented libburn gap).
 *   - macOS: 1 for every row of `drutil list`, which enumerates burning
 *     devices only; a machine without a recorder returns an EMPTY list,
 *     never a fabricated device.
 * isEmpty: assumed 1 - media state is deliberately not probed during
 * enumeration (that would spin idle drives up in a UI refresh). */
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
 *
 * New capabilities are added as virtuals WITH DEFAULT BODIES that report
 * "unsupported": a backend opts in by overriding exactly what it can do.
 * The engine calls SetBurnLastError itself when a default body refuses.
 * ------------------------------------------------------------------------- */

/* One CD-audio track for TAO burning. audioPath is absolute; the file is
 * either a Microsoft WAV (PCM 44.1 kHz 16-bit stereo, header parsed and
 * skipped) or headerless raw samples in that same format. */
struct BurnAudioTrackSpec {
    std::string audioPath;
    std::string isrc;          /* 12 chars or empty */
    int preemphasis;           /* 0/1 (RAW paths only; TAO ignores it) */
};

/* Description of a filesystem image to build (CREATE_IMAGE / DIRECTORY /
 * APPEND job modes). sessionStartBlock > 0 makes the image a continuation
 * session (multisession); 0 means first session. */
struct BurnImageSpec {
    std::string sourcePath;    /* directory (or file) to import */
    std::string outputPath;    /* image file to produce */
    std::string volumeLabel;
    int fileSystems;           /* BURN_FS_* bitmask */
    int interchangeLevel;      /* 1..3, 0 = backend default */
    std::string bootImagePath; /* El Torito load-clip; empty = no boot */
    int bootEmulation;         /* BURN_BOOT_EMU_* */
    int bootPlatform;          /* 0 X86, 1 PPC, 2 MAC, 0xEF EFI */
    int udfRevision;           /* 0 = default (e.g. 0x0201 */
    unsigned long long sessionStartBlock; /* LBA of new session, 0 = first */
    /* Device whose existing session a continuation image is built against
     * (multisession import). Empty = no drive involvement. */
    std::string devicePath;
};

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

    /* ---- extended capability surface (all optional) ---- */

    /* 1 = supported now, 0 = not implemented on this backend. */
    virtual int Capability(int capability);

    /* First writable LBA of the next session on devicePath (0 = blank disc,
     * >0 = appendable disc). Returns -1 when the disc cannot accept a new
     * session, -2 when multisession inquiry is unsupported. */
    virtual long long NextSessionStartBlock(const std::string& devicePath);

    /* Build the image described by spec (pure software path; no drive is
     * written). Default body delegates to the legacy CreateISO. */
    virtual bool CreateImage(const BurnImageSpec& spec);

    /* Write an already-built image. finalize = true closes the session so
     * no further append is possible. Default body delegates to BurnISO. */
    virtual bool BurnImageFile(const std::string& imagePath, const std::string& devicePath,
                               const std::string& discType, int speed, int verifyLevel,
                               bool finalize, unsigned long long sessionStartBlock);

    /* CD audio, track-at-once. */
    virtual bool BurnAudioTracks(const std::vector<BurnAudioTrackSpec>& tracks,
                                 const std::string& devicePath, int speed,
                                 int verifyLevel, bool finalizeDisc);

    /* CD-RW blank / DVD+RW-RAM format. full = zero the whole media. */
    virtual bool EraseDisc(const std::string& devicePath, bool full);

    /* RAW: build a 2352-byte-sector PQ-interleaved image from 1..99 tracks
     * (software path, drive-free). */
    virtual bool CreateRawImage(const std::vector<BurnAudioTrackSpec>& tracks,
                                const std::string& outputPath,
                                const std::string& mcn, int startingTrackNumber);

    /* RAW: burn a PQ image built by CreateRawImage (or an equivalent dump). */
    virtual bool BurnRawImage(const std::string& imagePath, const std::string& devicePath,
                              int speed, int verifyLevel);

    /* Byte-level read-back of a data session against the written image.
     * level: BURN_VERIFY_QUICK (sample) / BURN_VERIFY_FULL (every sector). */
    virtual bool VerifyWrite(const std::string& devicePath,
                             const std::string& expectedImagePath, int level);

    /* Tray control and live drive/media info (human-readable, may be empty). */
    virtual int EjectTray(const std::string& devicePath);
    virtual int CloseTray(const std::string& devicePath);
    virtual std::string DriveInformation(const std::string& devicePath);
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
 *                   2 = legacy API + handle-based job API,
 *                   3 = + audio TAO, erase, multisession append, UDF,
 *                       El Torito boot images, RAW writing/images,
 *                       disc read-back verification, tray control. */
#define BURN_API_VERSION 3

typedef void (*BurnProgressCallback)(int percent, const char* status, void* userData);

/* The `speed` parameter everywhere in this API is a SPEED MULTIPLIER
 * (4 = 4x, 16 = 16x), never an encoded transport rate: each backend
 * converts it to its native unit using the loaded media family
 * (CD 1x=176.4 kB/s, DVD 1x=1385 kB/s, BD 1x=4496 kB/s) and clamps the
 * result to what the drive actually supports. speed <= 0 means "use the
 * configured default" (SetDefaultSpeed, initially 4). */

DLL_API int CreateISOToDisc(const char* isoPath, const char* devicePath, const char* discType, int speed);
DLL_API int BurnDirectoryToDisc(const char* sourceDir, const char* devicePath, const char* discType, int speed);
DLL_API int CreateISOImage(const char* sourcePath, const char* isoPath, const char* volumeLabel);

/* VerifyDisc is superseded by Burn_VerifyWrite(), which really reads the
 * disc back and compares it with the image sector by sector. This function
 * only ever validated the IMAGE FILE (existence, non-empty, readable) and
 * then reported BURN_ERROR_UNSUPPORTED_FORMAT with an explanatory
 * last-error instead of the previous misleading BURN_SUCCESS, because a
 * file that was never compared with a disc must not look verified.
 * New code should call Burn_VerifyWrite(). */
DLL_API int VerifyDisc(const char* devicePath, const char* isoPath);

DLL_API int ListBurningDevices(DeviceInfo** devices, int* deviceCount);
DLL_API void FreeDeviceList(DeviceInfo* devices, int deviceCount);
DLL_API int GetDefaultDevice(char* devicePath, int bufferSize);

DLL_API const char* GetPlatformInfo();
DLL_API int CheckToolAvailability(const char* toolName);
DLL_API const char* GetSupportedFormats();
DLL_API int ValidateSourcePath(const char* path);
/* The defaults below are PROCESS-WIDE and thread-safe: configure them once
 * on any thread (typically at startup) and Burn_Start()/the legacy calls
 * on OTHER threads will use them too. Only GetLastErrorMessage() and its
 * buffer stay per-thread. */
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
 * Capability discovery and the version 3 feature surface
 *
 * Burn_GetCapability() answers whether THIS backend implements a feature,
 * so callers can build one UI for every platform instead of guessing from
 * the OS name. Functions below return BURN_ERROR_UNSUPPORTED_FORMAT on a
 * backend that does not implement them.
 * ------------------------------------------------------------------------- */

/* ---- capability IDs for Burn_GetCapability() ---- */
#define BURN_CAP_IMAGE_BURN        0   /* baseline data-disc write          */
#define BURN_CAP_CREATE_IMAGE      1   /* baseline ISO creation             */
#define BURN_CAP_AUDIO_TAO         2   /* audio CD-R/CD-RW, track-at-once   */
#define BURN_CAP_ERASE             3   /* CD-RW blank / DVD+RW-RAM format   */
#define BURN_CAP_MULTISESSION      4   /* append sessions + block inquiry   */
#define BURN_CAP_UDF               5   /* UDF filesystem in images          */
#define BURN_CAP_BOOT_IMAGE        6   /* El Torito boot catalog            */
#define BURN_CAP_READBACK_VERIFY   7   /* sector-exact disc verification    */
#define BURN_CAP_RAW_BURN          8   /* RAW (2352 B/sector) writing       */
#define BURN_CAP_RAW_IMAGE         9   /* RAW PQ image building             */
#define BURN_CAP_TRAY_CONTROL      10  /* eject / close tray                */
#define BURN_CAP_DRIVE_INFO        11  /* MMC drive + media reporting       */

/* ---- verification levels ---- */
#define BURN_VERIFY_NONE   0
#define BURN_VERIFY_QUICK  1   /* drive-side or sampled read-back */
#define BURN_VERIFY_FULL   2   /* every sector compared byte-wise */

/* ---- file systems to place in a created image (bitmask) ---- */
#define BURN_FS_ISO9660  1
#define BURN_FS_JOLIET   2
#define BURN_FS_UDF      4

/* ---- El Torito boot emulation ---- */
#define BURN_BOOT_EMU_NONE        0
#define BURN_BOOT_EMU_FLOPPY_12   1
#define BURN_BOOT_EMU_FLOPPY_144  2
#define BURN_BOOT_EMU_FLOPPY_288  3
#define BURN_BOOT_EMU_HARDDISK    4

/* ---- El Torito platform id ---- */
#define BURN_BOOT_PLATFORM_X86   0
#define BURN_BOOT_PLATFORM_PPC   1
#define BURN_BOOT_PLATFORM_MAC   2
#define BURN_BOOT_PLATFORM_EFI   0xEF

/* One CD-audio track. audioPath should be absolute (relative paths are
 * resolved by the engine). Accepted file format: a Microsoft WAV holding
 * PCM 44.1 kHz 16-bit stereo (the header is parsed and skipped), or
 * headerless raw samples of exactly that format. */
typedef struct BurnAudioTrackC {
    char audioPath[512];
    char isrc[16];       /* 12 chars + NUL, or empty */
    int  preemphasis;    /* 0/1; used by the RAW path only */
} BurnAudioTrackC;

DLL_API int Burn_GetCapability(int capability);

/* First writable LBA of the next session. 0 = blank disc, >0 = the LBA to
 * pass as sessionStartBlock when appending. Negative on failure (-2 when
 * the backend has no multisession support at all). */
DLL_API long long Burn_GetNextSessionBlock(const char* devicePath);

/* Synchronous convenience wrappers over the version 3 job modes; each is
 * exactly Burn_Simple() with the corresponding options filled in. */
DLL_API int BurnAudioCd(const char* const* wavPaths, int trackCount,
                        const char* devicePath, int speed, int verifyLevel);
DLL_API int BurnEraseDisc(const char* devicePath, int fullErase);
DLL_API int BurnAppendSession(const char* sourceDir, const char* devicePath,
                              const char* volumeLabel, int speed,
                              unsigned long long sessionStartBlock,
                              int leaveOpen);

/* Tray control: BURN_SUCCESS or an error code. Some drives ignore CloseTray
 * when the tray is already closed. */
DLL_API int BurnEjectTray(const char* devicePath);
DLL_API int BurnCloseTray(const char* devicePath);

/* Human-readable, multi-line MMC report for the device (vendor, product,
 * profiles, media state, speeds). Returns a pointer to a thread-local
 * buffer; do NOT free. Empty string when unsupported or unknown. */
DLL_API const char* Burn_GetDriveInformation(const char* devicePath);

/* Byte-level read-back of a written disc against the image it came from.
 * This is the verification VerifyDisc() cannot offer: it compares sector by
 * sector. Requires BURN_CAP_READBACK_VERIFY. level: BURN_VERIFY_QUICK
 * (sampled blocks) or BURN_VERIFY_FULL (every sector). */
DLL_API int Burn_VerifyWrite(const char* devicePath, const char* expectedImagePath, int level);

/* RAW (2352-byte sector) pipeline: build a PQ image from 1..99 audio/data
 * tracks WITHOUT a drive (pure software, so it is fully testable), then
 * burn that image with subcode-accurate writing. */
DLL_API int BurnCreateRawImage(const char* const* wavPaths, int trackCount,
                               const char* outputPath,
                               const char* mediaCatalogNumber, int startingTrackNumber);
DLL_API int BurnRawImageToDisc(const char* rawImagePath, const char* devicePath,
                               int speed, int verifyLevel);

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
/* New in API version 3. Query Burn_GetCapability() first: a backend that
 * does not implement one of these modes fails the job with
 * BURN_ERROR_UNSUPPORTED_FORMAT and an explanatory last-error. */
#define BURN_JOB_MODE_AUDIO_TAO    4  /* audio CD, track-at-once            */
#define BURN_JOB_MODE_ERASE        5  /* blank CD-RW / format DVD+RW-RAM    */
#define BURN_JOB_MODE_APPEND       6  /* continuation session (multisession)*/
#define BURN_JOB_MODE_VERIFY       7  /* read-back verification vs an image */
#define BURN_JOB_MODE_CREATE_RAW   8  /* build a RAW PQ image (no drive)    */
#define BURN_JOB_MODE_BURN_RAW     9  /* burn a RAW PQ image                */

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

/* API version 3 options. The first eleven fields are layout-identical to
 * BurnJobOptions, so Burn_Start() dispatches on `size`: submit
 * sizeof(BurnJobOptions) for the v1 struct or sizeof(BurnJobOptions3) for
 * the extended one. Every v3 field may keep its zero value for "backend
 * default"; job modes 4..9 require a matching BURN_CAP_* capability or the
 * job fails with BURN_ERROR_UNSUPPORTED_FORMAT. */
typedef struct BurnJobOptions3 {
    int  size;                     /* set to sizeof(BurnJobOptions3) */
    int  mode;                     /* BURN_JOB_MODE_* */
    const char* sourcePath;        /* image file or source directory */
    const char* outputPath;        /* CREATE_IMAGE / CREATE_RAW only */
    const char* devicePath;        /* NULL/"" -> configured default device */
    const char* discType;          /* NULL/"" -> configured default disc type */
    const char* volumeLabel;       /* NULL/"" -> "DISC" */
    int  speed;                    /* <= 0 -> configured default speed */
    int  verify;                   /* deprecated alias of verifyLevel */
    BurnProgressCallback progressCallback;
    void* userData;
    /* ---- version 3 additions ---- */
    int  verifyLevel;              /* BURN_VERIFY_* */
    int  leaveOpen;                /* 1 = do NOT close the session so the disc
                                    * stays appendable. 0 = close (default).
                                    * Meaningful on sequential media only. */
    int  fileSystems;              /* BURN_FS_* bitmask; 0 = ISO9660+Joliet */
    int  interchangeLevel;         /* ISO9660 level 1..3; 0 = backend default */
    const char* bootImagePath;     /* El Torito load-clip; NULL = no boot */
    int  bootEmulation;            /* BURN_BOOT_EMU_* */
    int  bootPlatform;             /* BURN_BOOT_PLATFORM_* */
    int  udfRevision;              /* e.g. 0x0201; 0 = backend default */
    unsigned long long sessionStartBlock; /* continuation session LBA, from
                                    * Burn_GetNextSessionBlock(); 0 = first
                                    * session or query the drive */
    int  eraseFull;                /* ERASE: 1 = full erase, 0 = fast blank */
    const BurnAudioTrackC* audioTracks;  /* AUDIO_TAO / CREATE_RAW */
    int  audioTrackCount;          /* 1..99 */
    const char* mediaCatalogNumber;      /* CREATE_RAW: MCN or NULL */
    int  startingTrackNumber;      /* CREATE_RAW: 1..99; 0 = 1 */
} BurnJobOptions3;

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
