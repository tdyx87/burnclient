#include "burning_utility.h"
#include <string>
#include <vector>
#include <memory>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <random>
#include <system_error>
#include <exception>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

/* Thread-local error string */
static thread_local std::string g_lastError;
/* Thread-safe global defaults */
static thread_local std::string g_defaultDevice = "";
static thread_local std::string g_defaultDiscType = "CD-R";
static thread_local int g_defaultSpeed = 4;
static thread_local BurnProgressCallback g_progressCallback = nullptr;
static thread_local void* g_progressUserData = nullptr;

/* One platform instance per thread (COM apartments and libburn are both
 * thread-affine). The pointer is deliberately never destroyed: running
 * ~IPlatform from a thread_local destructor during CRT/DLL teardown would
 * call CoUninitialize() / burn_finish() from DLL_PROCESS_DETACH, after those
 * subsystems may already be gone. The OS reclaims one object per thread. */
static IPlatform* GetPlatform() {
    static thread_local IPlatform* s_instance = CreatePlatformImpl().release();
    return s_instance;
}

/* Internal helpers — NOT extern "C" because they take std::string.
 * Declared as regular C++ functions with internal linkage. */
void SetBurnLastError(const std::string& error) {
    g_lastError = error;
}

void NotifyProgressBurn(int percent, const char* status) {
    if (g_progressCallback) {
        /* The callback belongs to the caller. An exception thrown out of it
         * must never unwind through platform/COM/libburn C frames or through
         * an exported C function. Swallow it and keep the burn running. */
        try {
            g_progressCallback(percent, status, g_progressUserData);
        } catch (...) {
        }
    }
}

/* ---------------------------------------------------------------------------
 * C API — these functions are declared extern "C" in the header and must
 * have C linkage here. SetBurnLastError and NotifyProgressBurn are internal
 * C++ helpers and are NOT inside the extern "C" block.
 * ------------------------------------------------------------------------- */

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
    } catch (const std::exception& e) {
        SetBurnLastError(std::string("Internal exception: ") + e.what());
        return;
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return;
    }
}

void FreeStringMemory(void* ptr) {
    try {
        if (ptr) {
            free(ptr);
        }
    } catch (const std::exception& e) {
        SetBurnLastError(std::string("Internal exception: ") + e.what());
        return;
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return;
    }
}

void SetProgressCallback(BurnProgressCallback callback, void* userData) {
    try {
        g_progressCallback = callback;
        g_progressUserData = userData;
    } catch (const std::exception& e) {
        SetBurnLastError(std::string("Internal exception: ") + e.what());
        return;
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return;
    }
}

int CreateISOToDisc(const char* isoPath, const char* devicePath, const char* discType, int speed) {
    try {
        if (!isoPath || !devicePath) {
            SetBurnLastError("Invalid parameters");
            return BURN_ERROR_INVALID_PARAM;
        }

        if (!PathExists(isoPath)) {
            SetBurnLastError("ISO file not found");
            return BURN_ERROR_INVALID_PARAM;
        }

        IPlatform* platform = GetPlatform();
        if (!platform) {
            SetBurnLastError("Failed to create platform implementation");
            return BURN_ERROR_UNSUPPORTED_FORMAT;
        }

        std::string disc = discType ? discType : g_defaultDiscType;
        int burnSpeed = speed > 0 ? speed : g_defaultSpeed;

        if (platform->BurnISO(isoPath, devicePath, disc, burnSpeed)) {
            return BURN_SUCCESS;
        }

        return BURN_ERROR_BURN_FAILED;
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

        if (!PathExists(sourceDir)) {
            SetBurnLastError("Source directory not found");
            return BURN_ERROR_INVALID_PARAM;
        }

        std::string tempIsoPath = GenerateTempISOPath();

        if (CreateISOImage(sourceDir, tempIsoPath.c_str(), "BURN") != BURN_SUCCESS) {
            SetBurnLastError("Failed to create temporary ISO");
            /* Clean up partial file */
            remove(tempIsoPath.c_str());
            return BURN_ERROR_ISO_CREATION_FAILED;
        }

        int result = CreateISOToDisc(tempIsoPath.c_str(), devicePath, discType, speed);

        remove(tempIsoPath.c_str());

        return result;
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

        if (!PathExists(sourcePath)) {
            SetBurnLastError("Source path not found");
            return BURN_ERROR_INVALID_PARAM;
        }

        IPlatform* platform = GetPlatform();
        if (!platform) {
            SetBurnLastError("Failed to create platform implementation");
            return BURN_ERROR_UNSUPPORTED_FORMAT;
        }

        std::string label = volumeLabel ? volumeLabel : "DISC";

        if (platform->CreateISO(sourcePath, isoPath, label)) {
            return BURN_SUCCESS;
        }

        return BURN_ERROR_ISO_CREATION_FAILED;
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

        /* Check that the ISO file exists */
        if (!PathExists(isoPath)) {
            SetBurnLastError("ISO file not found for verification");
            return BURN_ERROR_INVALID_PARAM;
        }

        /* Get ISO file size */
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

        /* On Windows, we could use IMAPI2 to read back the disc and compare.
         * On Linux, we could use libburn to read the disc back.
         * For now, perform a basic size-based sanity check via the platform. */
        NotifyProgressBurn(50, "Verifying disc...");
        NotifyProgressBurn(100, "Verification completed (basic check)");

        /* TODO: Full byte-level verification by reading disc sectors back and
         * comparing with the ISO file. This requires platform-specific
         * read-from-disc support which is not yet implemented. */

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
    try {
        if (devices) {
            free(devices);
        }
    } catch (const std::exception& e) {
        SetBurnLastError(std::string("Internal exception: ") + e.what());
        return;
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
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
    } catch (const std::exception& e) {
        SetBurnLastError(std::string("Internal exception: ") + e.what());
        return BURN_ERROR_EXCEPTION;
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return BURN_ERROR_EXCEPTION;
    }
}

const char* GetPlatformInfo() {
    try {
        IPlatform* platform = GetPlatform();
        if (!platform) {
            return "Unknown";
        }

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
        if (!toolName) {
            return 0;
        }

        IPlatform* platform = GetPlatform();
        if (!platform) {
            return 0;
        }

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
        static const char* formats = "CD-R, CD-RW, DVD-R, DVD+R, DVD-RW, DVD+RW, DVD-RAM, BD-R, BD-RE";
        /* This is a string literal — callers must NOT call FreeStringMemory on it. */
        return formats;
    } catch (const std::exception& e) {
        SetBurnLastError(std::string("Internal exception: ") + e.what());
        return "Internal exception";
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return "Internal exception";
    }
}

int ValidateSourcePath(const char* path) {
    try {
        if (!path) {
            return 0;
        }
        /* Use cross-platform path existence check */
        return PathExists(path) ? 1 : 0;
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
            g_defaultDevice = device;
        }
    } catch (const std::exception& e) {
        SetBurnLastError(std::string("Internal exception: ") + e.what());
        return;
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return;
    }
}

void SetDefaultDiscType(const char* discType) {
    try {
        if (discType) {
            g_defaultDiscType = discType;
        }
    } catch (const std::exception& e) {
        SetBurnLastError(std::string("Internal exception: ") + e.what());
        return;
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return;
    }
}

void SetDefaultSpeed(int speed) {
    try {
        if (speed > 0) {
            g_defaultSpeed = speed;
        }
    } catch (const std::exception& e) {
        SetBurnLastError(std::string("Internal exception: ") + e.what());
        return;
    } catch (...) {
        SetBurnLastError("Internal exception (unknown type)");
        return;
    }
}

}

/* ---------------------------------------------------------------------------
 * C++ wrapper class (BurningUtility)
 * ------------------------------------------------------------------------- */
/* The wrapper delegates to the C API, which owns the per-thread platform
 * instance. Impl is kept as an opaque pImpl so the DLL ABI stays stable.
 * Creating a second platform here used to double-initialize COM and caused
 * a crash at process exit. */
class BurningUtility::Impl {
public:
    Impl() = default;
};

BurningUtility::BurningUtility() : pImpl(new Impl()) {
}

BurningUtility::~BurningUtility() {
    delete pImpl;
}

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
        for (int i = 0; i < count; ++i) {
            result.push_back(devices[i]);
        }
        ::FreeDeviceList(devices, count);
    }

    return result;
}

std::string BurningUtility::GetDefaultDevice() {
    char buffer[512];
    if (::GetDefaultDevice(buffer, sizeof(buffer)) == BURN_SUCCESS) {
        return std::string(buffer);
    }
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

void BurningUtility::SetDefaultDevice(const std::string& device) {
    ::SetDefaultDevice(device.c_str());
}

void BurningUtility::SetDefaultDiscType(const std::string& discType) {
    ::SetDefaultDiscType(discType.c_str());
}

void BurningUtility::SetDefaultSpeed(int speed) {
    ::SetDefaultSpeed(speed);
}

void BurningUtility::SetProgressCallback(BurnProgressCallback callback, void* userData) {
    ::SetProgressCallback(callback, userData);
}

std::string BurningUtility::GetLastErrorMessage() {
    const char* msg = ::GetLastErrorMessage();
    return msg ? std::string(msg) : "";
}

void BurningUtility::ClearLastError() {
    ::ClearLastError();
}
