#include "burning_utility.h"
#include "burn_internal.h"
#include <string>
#include <vector>
#include <memory>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>

#ifdef _WIN32

#include <windows.h>
#include <imapi2.h>
#include <imapi2fs.h>
#include <comdef.h>
#include <shlobj.h>
#include <shlwapi.h>   /* SHCreateStreamOnFileA */
#include <oleauto.h>   /* SAFEARRAY helpers */
#include <ocidl.h>     /* IConnectionPointContainer / IConnectionPoint */

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shlwapi.lib")

/* IDiscRecorder2::get_VolumePathNames() returns a SAFEARRAY of BSTR,
 * NOT a single BSTR. Extract the first volume path as UTF-8. */
static bool GetFirstVolumePath(IDiscRecorder2* recorder, char* outBuf, size_t outSize) {
    if (!recorder || !outBuf || outSize == 0) {
        return false;
    }

    SAFEARRAY* paths = nullptr;
    if (FAILED(recorder->get_VolumePathNames(&paths)) || !paths) {
        return false;
    }

    bool ok = false;
    LONG lower = 0;
    LONG upper = -1;
    if (SUCCEEDED(SafeArrayGetLBound(paths, 1, &lower)) &&
        SUCCEEDED(SafeArrayGetUBound(paths, 1, &upper)) && upper >= lower) {
        BSTR first = nullptr;
        if (SUCCEEDED(SafeArrayGetElement(paths, &lower, &first)) && first) {
            WideCharToMultiByte(CP_UTF8, 0, first, -1, outBuf,
                                static_cast<int>(outSize - 1), nullptr, nullptr);
            outBuf[outSize - 1] = '\0';
            SysFreeString(first);
            ok = true;
        }
    }

    SafeArrayDestroy(paths);
    return ok;
}

/* ---------------------------------------------------------------------------
 * IMAPI2 DDiscFormat2DataEvents sink
 *
 * IDiscFormat2Data::Write() is synchronous and blocked until the disc is
 * done. Without a connection point the caller only ever sees 10/30/50/100%.
 * Advising DDiscFormat2DataEvents::Update() gives elapsed/remaining time,
 * which is turned into real 0..99% progress. The same callback also notices
 * a pending cancellation and calls CancelWrite() on the writer object.
 * ------------------------------------------------------------------------- */
class BurnProgressSink : public IDispatch {
public:
    BurnProgressSink() : ref_(1) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IDispatch ||
            riid == __uuidof(DDiscFormat2DataEvents)) {
            *ppv = static_cast<IDispatch*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&ref_));
    }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG remaining = static_cast<ULONG>(InterlockedDecrement(&ref_));
        if (remaining == 0) delete this;
        return remaining;
    }

    STDMETHODIMP GetTypeInfoCount(UINT* pctinfo) override {
        if (pctinfo) *pctinfo = 0;
        return S_OK;
    }

    STDMETHODIMP GetTypeInfo(UINT, LCID, ITypeInfo** pptinfo) override {
        if (pptinfo) *pptinfo = nullptr;
        return E_NOTIMPL;
    }

    STDMETHODIMP GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override {
        return E_NOTIMPL;
    }

    STDMETHODIMP Invoke(DISPID dispid, REFIID, LCID, WORD, DISPPARAMS* params,
                        VARIANT*, EXCEPINFO*, UINT*) override {
        if (dispid != DISPID_DDISCFORMAT2DATAEVENTS_UPDATE || !params ||
            params->cArgs < 1 || !params->rgvarg) {
            return S_OK;
        }

        /* DISPPARAMS arguments arrive in reverse order:
         *   rgvarg[0] = progress (IDiscFormat2DataEventArgs)
         *   rgvarg[1] = object   (IDiscFormat2Data)                     */
        VARIANTARG& progressVar = params->rgvarg[0];
        if (progressVar.vt == VT_DISPATCH && progressVar.pdispVal) {
            IDiscFormat2DataEventArgs* args = nullptr;
            if (SUCCEEDED(progressVar.pdispVal->QueryInterface(
                    __uuidof(IDiscFormat2DataEventArgs),
                    reinterpret_cast<void**>(&args))) && args) {
                LONG elapsed = 0;
                LONG remaining = 0;
                args->get_ElapsedTime(&elapsed);
                args->get_RemainingTime(&remaining);
                LONG total = elapsed + remaining;
                int percent = total > 0 ? static_cast<int>(elapsed * 100L / total) : 50;
                if (percent < 0) percent = 0;
                if (percent > 99) percent = 99;
                char message[128];
                snprintf(message, sizeof(message), "Writing to disc... %d%%", percent);
                NotifyProgressBurn(percent, message);
                args->Release();
            }
        }

        if (IsBurnCancelled() && params->cArgs >= 2) {
            VARIANTARG& objectVar = params->rgvarg[1];
            if (objectVar.vt == VT_DISPATCH && objectVar.pdispVal) {
                IDiscFormat2Data* writer = nullptr;
                if (SUCCEEDED(objectVar.pdispVal->QueryInterface(
                        __uuidof(IDiscFormat2Data),
                        reinterpret_cast<void**>(&writer))) && writer) {
                    writer->CancelWrite();
                    writer->Release();
                }
            }
        }
        return S_OK;
    }

private:
    volatile LONG ref_;
};

/* Native cancellation hook. The context is stack-allocated in BurnISO() and
 * stays valid until ClearBurnCancelHook() returns (it waits for in-flight
 * hooks), which is why the hook may be called from Burn_Cancel()'s thread. */
struct WindowsCancelContext {
    std::mutex mutex;
    IDiscFormat2Data* writer = nullptr;
};

static void WindowsCancelHook(void* context) {
    WindowsCancelContext* ctx = static_cast<WindowsCancelContext*>(context);
    std::lock_guard<std::mutex> lock(ctx->mutex);
    if (ctx->writer) {
        ctx->writer->CancelWrite();
    }
}

class WindowsPlatform : public IPlatform {
public:
    WindowsPlatform() {
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    }

    ~WindowsPlatform() override {
        CoUninitialize();
    }

    std::vector<DeviceInfo> ListDevices() override {
        std::vector<DeviceInfo> devices;

        IDiscMaster2* pDiscMaster = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_MsftDiscMaster2, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IDiscMaster2, (void**)&pDiscMaster);
        if (FAILED(hr)) {
            SetBurnLastError("Failed to create IDiscMaster2");
            DeviceInfo info = {};
            info.isWritable = 1;
            info.isEmpty = 1;
            strncpy(info.mediaType, "CD/DVD", sizeof(info.mediaType) - 1);
            strncpy(info.devicePath, "D:", sizeof(info.devicePath) - 1);
            strncpy(info.deviceName, "Default Optical Drive", sizeof(info.deviceName) - 1);
            devices.push_back(info);
            return devices;
        }

        long count = 0;
        hr = pDiscMaster->get_Count(&count);
        if (SUCCEEDED(hr)) {
            for (long i = 0; i < count; ++i) {
                BSTR uniqueId = nullptr;
                hr = pDiscMaster->get_Item(i, &uniqueId);
                if (SUCCEEDED(hr) && uniqueId) {
                    IDiscRecorder2* pRecorder = nullptr;
                    hr = CoCreateInstance(CLSID_MsftDiscRecorder2, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_IDiscRecorder2, (void**)&pRecorder);
                    if (SUCCEEDED(hr)) {
                        hr = pRecorder->InitializeDiscRecorder(uniqueId);
                        if (SUCCEEDED(hr)) {
                            DeviceInfo info = {};
                            info.isWritable = 1;
                            info.isEmpty = 1;
                            strncpy(info.mediaType, "CD/DVD", sizeof(info.mediaType) - 1);

                            if (!GetFirstVolumePath(pRecorder, info.devicePath,
                                                    sizeof(info.devicePath))) {
                                strncpy(info.devicePath, "D:", sizeof(info.devicePath) - 1);
                            }

                            BSTR prodId = nullptr;
                            pRecorder->get_ProductId(&prodId);
                            if (prodId) {
                                WideCharToMultiByte(CP_UTF8, 0, prodId, -1, info.deviceName, 255, nullptr, nullptr);
                                SysFreeString(prodId);
                            } else {
                                strncpy(info.deviceName, "Optical Drive", sizeof(info.deviceName) - 1);
                            }

                            devices.push_back(info);
                        }
                        pRecorder->Release();
                    }
                    SysFreeString(uniqueId);
                }
            }
        }
        pDiscMaster->Release();
        return devices;
    }

    bool BurnISO(const std::string& isoPath, const std::string& devicePath,
                 const std::string& discType, int speed) override {
        (void)discType;
        NotifyProgressBurn(10, "Initializing burn process...");
        if (IsBurnCancelled()) {
            SetBurnLastError("Burn cancelled by caller");
            return false;
        }

        IDiscMaster2* pDiscMaster = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_MsftDiscMaster2, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IDiscMaster2, (void**)&pDiscMaster);
        if (FAILED(hr)) {
            SetBurnLastError("Failed to create IDiscMaster2");
            return false;
        }

        /* Find the recorder whose volume path matches devicePath.
         * Only match exactly - do not fall back to the first device. */
        IDiscRecorder2* pRecorder = nullptr;
        long count = 0;
        pDiscMaster->get_Count(&count);
        for (long i = 0; i < count; ++i) {
            BSTR uniqueId = nullptr;
            pDiscMaster->get_Item(i, &uniqueId);
            if (uniqueId) {
                IDiscRecorder2* pRec = nullptr;
                if (SUCCEEDED(CoCreateInstance(CLSID_MsftDiscRecorder2, nullptr, CLSCTX_INPROC_SERVER,
                                              IID_IDiscRecorder2, (void**)&pRec))) {
                    if (SUCCEEDED(pRec->InitializeDiscRecorder(uniqueId))) {
                        char path[512] = "";
                        if (!GetFirstVolumePath(pRec, path, sizeof(path))) {
                            path[0] = '\0';
                        }
                        if (strcmp(path, devicePath.c_str()) == 0) {
                            if (pRecorder) pRecorder->Release();
                            pRecorder = pRec;
                        } else {
                            pRec->Release();
                        }
                    } else {
                        pRec->Release();
                    }
                }
                SysFreeString(uniqueId);
            }
        }

        if (!pRecorder) {
            SetBurnLastError("Device not found: " + devicePath);
            pDiscMaster->Release();
            return false;
        }

        NotifyProgressBurn(30, "Preparing disc...");
        if (IsBurnCancelled()) {
            SetBurnLastError("Burn cancelled by caller");
            pRecorder->Release();
            pDiscMaster->Release();
            return false;
        }

        IDiscFormat2Data* pDataWriter = nullptr;
        hr = CoCreateInstance(CLSID_MsftDiscFormat2Data, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IDiscFormat2Data, (void**)&pDataWriter);
        if (FAILED(hr)) {
            SetBurnLastError("Failed to create IDiscFormat2Data");
            pRecorder->Release();
            pDiscMaster->Release();
            return false;
        }

        pDataWriter->put_Recorder(pRecorder);

        BSTR clientName = SysAllocString(L"BurningUtility");
        pDataWriter->put_ClientName(clientName);
        SysFreeString(clientName);

        if (speed > 0) {
            /* IDiscFormat2Data has no put_RequestedWriteSpeed (the property
             * is get-only). Write speed is set with SetWriteSpeed() and is
             * expressed in sectors per second. */
            pDataWriter->SetWriteSpeed(speed * 1000, VARIANT_FALSE);
        }

        IStream* pStream = nullptr;
        hr = SHCreateStreamOnFileA(isoPath.c_str(), STGM_READ, &pStream);
        if (FAILED(hr)) {
            SetBurnLastError("Failed to open ISO file: " + isoPath);
            pDataWriter->Release();
            pRecorder->Release();
            pDiscMaster->Release();
            return false;
        }

        NotifyProgressBurn(50, "Burning in progress...");

        /* Subscribe to real progress notifications. If IMAPI2 refuses the
         * connection point we still burn, just with coarse progress. */
        IConnectionPointContainer* cpc = nullptr;
        IConnectionPoint* connection = nullptr;
        DWORD adviseCookie = 0;
        if (SUCCEEDED(pDataWriter->QueryInterface(__uuidof(IConnectionPointContainer),
                                                  (void**)&cpc)) && cpc) {
            if (SUCCEEDED(cpc->FindConnectionPoint(__uuidof(DDiscFormat2DataEvents),
                                                   &connection)) && connection) {
                BurnProgressSink* sink = new BurnProgressSink();
                hr = connection->Advise(static_cast<IUnknown*>(static_cast<IDispatch*>(sink)),
                                        &adviseCookie);
                sink->Release();   /* Advise() AddRef'd on success */
                if (FAILED(hr)) adviseCookie = 0;
            }
            cpc->Release();
        }

        WindowsCancelContext cancelCtx;
        {
            std::lock_guard<std::mutex> lock(cancelCtx.mutex);
            cancelCtx.writer = pDataWriter;
        }
        SetBurnCancelHook(WindowsCancelHook, &cancelCtx);

        hr = pDataWriter->Write(pStream);

        /* Neutralise the hook before clearing it: a late Burn_Cancel() must
         * never call CancelWrite() on a released COM object. */
        {
            std::lock_guard<std::mutex> lock(cancelCtx.mutex);
            cancelCtx.writer = nullptr;
        }
        ClearBurnCancelHook(WindowsCancelHook, &cancelCtx);

        if (connection) {
            if (adviseCookie) connection->Unadvise(adviseCookie);
            connection->Release();
        }

        pStream->Release();
        pDataWriter->Release();
        pRecorder->Release();
        pDiscMaster->Release();

        if (IsBurnCancelled()) {
            SetBurnLastError("Burn cancelled by caller");
            return false;
        }
        if (FAILED(hr)) {
            SetBurnLastError("Burn write failed");
            return false;
        }

        NotifyProgressBurn(100, "Burn completed");
        return true;
    }

    bool CreateISO(const std::string& sourcePath, const std::string& isoPath,
                   const std::string& volumeLabel) override {
        NotifyProgressBurn(10, "Creating ISO image...");
        if (IsBurnCancelled()) {
            SetBurnLastError("ISO creation cancelled by caller");
            return false;
        }

        IFileSystemImage* pFsImage = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_MsftFileSystemImage, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IFileSystemImage, (void**)&pFsImage);
        if (FAILED(hr)) {
            SetBurnLastError("Failed to create IFileSystemImage");
            return false;
        }

        /* Set volume name */
        int wlen = MultiByteToWideChar(CP_UTF8, 0, volumeLabel.c_str(), -1, nullptr, 0);
        if (wlen > 0) {
            wchar_t* wbuf = new wchar_t[wlen];
            MultiByteToWideChar(CP_UTF8, 0, volumeLabel.c_str(), -1, wbuf, wlen);
            BSTR bstrLabel = SysAllocString(wbuf);
            pFsImage->put_VolumeName(bstrLabel);
            SysFreeString(bstrLabel);
            delete[] wbuf;
        }

        /* The bitwise OR of two enum values is an int in C++; cast back. */
        pFsImage->put_FileSystemsToCreate(
            static_cast<FsiFileSystems>(FsiFileSystemISO9660 | FsiFileSystemJoliet));

        /* Correct API: use IFsiDirectoryItem->AddTree to import a directory. */
        IFsiDirectoryItem* pRootDir = nullptr;
        hr = pFsImage->get_Root(&pRootDir);
        if (SUCCEEDED(hr) && pRootDir) {
            int slen = MultiByteToWideChar(CP_UTF8, 0, sourcePath.c_str(), -1, nullptr, 0);
            if (slen > 0) {
                wchar_t* wbuf = new wchar_t[slen];
                MultiByteToWideChar(CP_UTF8, 0, sourcePath.c_str(), -1, wbuf, slen);
                BSTR bstrSource = SysAllocString(wbuf);
                hr = pRootDir->AddTree(bstrSource, VARIANT_FALSE);
                if (FAILED(hr)) {
                    SetBurnLastError("Failed to add source directory to ISO image");
                }
                SysFreeString(bstrSource);
                delete[] wbuf;
            }
            pRootDir->Release();
        } else {
            SetBurnLastError("Failed to get root directory of ISO image");
        }

        NotifyProgressBurn(50, "Writing ISO file...");

        IFileSystemImageResult* pResult = nullptr;
        hr = pFsImage->CreateResultImage(&pResult);
        if (FAILED(hr)) {
            SetBurnLastError("Failed to create ISO result image");
            pFsImage->Release();
            return false;
        }

        bool ok = false;
        IStream* pStream = nullptr;
        pResult->get_ImageStream(&pStream);
        if (pStream) {
            FILE* f = fopen(isoPath.c_str(), "wb");
            if (f) {
                char buf[65536];
                ULONG read = 0;
                while (SUCCEEDED(pStream->Read(buf, sizeof(buf), &read)) && read > 0) {
                    fwrite(buf, 1, read, f);
                    if (IsBurnCancelled()) break;
                }
                fclose(f);
                /* A cancelled or truncated write must not be reported as a
                 * usable image - remove the partial file. */
                if (IsBurnCancelled()) {
                    remove(isoPath.c_str());
                    SetBurnLastError("ISO creation cancelled by caller");
                } else {
                    ok = true;
                }
            } else {
                SetBurnLastError("Cannot open output file: " + isoPath);
            }
            pStream->Release();
        } else {
            SetBurnLastError("ISO result image has no stream");
        }

        pResult->Release();
        pFsImage->Release();

        if (!ok) return false;
        NotifyProgressBurn(100, "ISO creation completed");
        return true;
    }

    std::string GetPlatformInfo() override {
        return "Windows (IMAPI2)";
    }

    bool CheckToolAvailability(const std::string& toolName) override {
        char path[MAX_PATH] = {0};
        DWORD len = SearchPathA(NULL, toolName.c_str(), ".exe", MAX_PATH, path, NULL);
        if (len > 0 && len < MAX_PATH) {
            return true;
        }
        DWORD attr = GetFileAttributesA(toolName.c_str());
        return attr != INVALID_FILE_ATTRIBUTES;
    }
};

std::unique_ptr<IPlatform> CreatePlatformImpl() {
    return std::make_unique<WindowsPlatform>();
}

#endif
