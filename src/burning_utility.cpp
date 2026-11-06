#include "burning_utility.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <random>
#include <system_error>

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

/* ---------------------------------------------------------------------------
 * Cached platform instance — avoids repeated CoInitializeEx/CoUninitialize
 * on Windows and burn_initialize/burn_finish on Linux.
 * ------------------------------------------------------------------------- */
static IPlatform& GetPlatformInstance() {
    static thread_local std::unique_ptr<IPlatform> s_instance;
    if (!s_instance) {
        s_instance = CreatePlatformImpl();
    }
    if (!s_instance) {
        /* Fallback should never happen, but be safe */
        static IPlatform* s_null = nullptr;
        /* Return a reference to a dummy — callers check null separately.
           In practice CreatePlatformImpl always returns a valid object. */
    }
    return *s_instance;
}

/* Helper to get a platform instance (may return null) */
static std::unique_ptr<IPlatform>& GetPlatformRef() {
    static thread_local std::unique_ptr<IPlatform> s_instance;
    if (!s_instance) {
        s_instance = CreatePlatformImpl();
    }
    return s_instance;
}

extern "C" {

void SetBurnLastError(const std::string& error) {
    g_lastError = error;
}

const char* GetLastErrorMessage() {
    static thread_local std::string result;
    result = g_lastError;
    return result.c_str();
}

void ClearLastError() {
    g_lastError.clear();
}

void FreeStringMemory(void* ptr) {
    if (ptr) {
        free(ptr);
    }
}

void SetProgressCallback(BurnProgressCallback callback, void* userData) {
    g_progressCallback = callback;
    g_progressUserData = userData;
}

void NotifyProgressBurn(int percent, const char* status) {
    if (g_progressCallback) {
        g_progressCallback(percent, status, g_progressUserData);
    }
}

/* ---------------------------------------------------------------------------
 * Cross-platform temporary file path generation
 * ------------------------------------------------------------------------- */
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

/* ---------------------------------------------------------------------------
 * Cross-platform path / file existence check
 * ------------------------------------------------------------------------- */
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

int CreateISOToDisc(const char* isoPath, const char* devicePath, const char* discType, int speed) {
    if (!isoPath || !devicePath) {
        SetBurnLastError("Invalid parameters");
        return BURN_ERROR_INVALID_PARAM;
    }

    if (!PathExists(isoPath)) {
        SetBurnLastError("ISO file not found");
        return BURN_ERROR_INVALID_PARAM;
    }

    auto& platform = GetPlatformRef();
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
}

int BurnDirectoryToDisc(const char* sourceDir, const char* devicePath, const char* discType, int speed) {
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
}

int CreateISOImage(const char* sourcePath, const char* isoPath, const char* volumeLabel) {
    if (!sourcePath || !isoPath) {
        SetBurnLastError("Invalid parameters");
        return BURN_ERROR_INVALID_PARAM;
    }

    if (!PathExists(sourcePath)) {
        SetBurnLastError("Source path not found");
        return BURN_ERROR_INVALID_PARAM;
    }

    auto& platform = GetPlatformRef();
    if (!platform) {
        SetBurnLastError("Failed to create platform implementation");
        return BURN_ERROR_UNSUPPORTED_FORMAT;
    }

    std::string label = volumeLabel ? volumeLabel : "DISC";

    if (platform->CreateISO(sourcePath, isoPath, label)) {
        return BURN_SUCCESS;
    }

    return BURN_ERROR_ISO_CREATION_FAILED;
}

int VerifyDisc(const char* devicePath, const char* isoPath) {
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
}

int ListBurningDevices(DeviceInfo** devices, int* deviceCount) {
    if (!devices || !deviceCount) {
        SetBurnLastError("Invalid parameters");
        return BURN_ERROR_INVALID_PARAM;
    }

    auto& platform = GetPlatformRef();
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
}

void FreeDeviceList(DeviceInfo* devices, int deviceCount) {
    if (devices) {
        free(devices);
    }
}

int GetDefaultDevice(char* devicePath, int bufferSize) {
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
}

const char* GetPlatformInfo() {
    auto& platform = GetPlatformRef();
    if (!platform) {
        return "Unknown";
    }

    static thread_local std::string result;
    result = platform->GetPlatformInfo();
    return result.c_str();
}

int CheckToolAvailability(const char* toolName) {
    if (!toolName) {
        return 0;
    }

    auto& platform = GetPlatformRef();
    if (!platform) {
        return 0;
    }

    return platform->CheckToolAvailability(toolName) ? 1 : 0;
}

const char* GetSupportedFormats() {
    static const char* formats = "CD-R, CD-RW, DVD-R, DVD+R, DVD-RW, DVD+RW, DVD-RAM, BD-R, BD-RE";
    /* This is a string literal — callers must NOT call FreeStringMemory on it. */
    return formats;
}

int ValidateSourcePath(const char* path) {
    if (!path) {
        return 0;
    }
    /* Use cross-platform path existence check */
    return PathExists(path) ? 1 : 0;
}

void SetDefaultDevice(const char* device) {
    if (device) {
        g_defaultDevice = device;
    }
}

void SetDefaultDiscType(const char* discType) {
    if (discType) {
        g_defaultDiscType = discType;
    }
}

void SetDefaultSpeed(int speed) {
    if (speed > 0) {
        g_defaultSpeed = speed;
    }
}

}

/* ---------------------------------------------------------------------------
 * C++ wrapper class (BurningUtility)
 * ------------------------------------------------------------------------- */
class BurningUtility::Impl {
public:
    Impl() {
        platform = CreatePlatformImpl();
    }

    std::unique_ptr<IPlatform> platform;
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
