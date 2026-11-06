#ifndef BURNING_UTILITY_H
#define BURNING_UTILITY_H

#include <string>
#include <vector>
#include <memory>

/* DeviceInfo struct — plain C-compatible, safe across DLL boundary */
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
 * Platform abstraction interface (C++ only, internal — not exported)
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

/* Internal factory — called only within the DLL, not exported */
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
 * GetLastErrorMessage() — those return pointers to internal static
 * buffers and must not be freed. */
DLL_API void FreeStringMemory(void* ptr);

DLL_API void SetProgressCallback(BurnProgressCallback callback, void* userData);

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
#endif

#endif
