#include "burning_utility.h"
#include "burn_internal.h"
#include "windows_features.h"
#include <string>
#include <vector>
#include <memory>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>

#ifdef _WIN32

#include <windows.h>
#include <winioctl.h>  /* IOCTL_DISK_IS_WRITABLE */
#include <imapi2.h>
#include <imapi2error.h>   /* E_IMAPI_RECORDER_MEDIA_* transient states */
#include <imapi2fs.h>
#include <comdef.h>
#include <shlobj.h>
#include <shlwapi.h>   /* SHCreateStreamOnFileA */
#include <oleauto.h>   /* SAFEARRAY helpers */
#include <ocidl.h>     /* IConnectionPointContainer / IConnectionPoint */

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shlwapi.lib")

/* IDiscRecorder2::get_VolumePathNames() returns a SAFEARRAY whose elements
 * are VARIANT values of type VT_BSTR; the path is in bstrVal. Accept a
 * plain SAFEARRAY(BSTR) as a compatibility fallback for other IMAPI2
 * implementations. Extract the first volume path as UTF-8. */
static bool GetFirstVolumePath(IDiscRecorder2* recorder, char* outBuf, size_t outSize) {
    if (!recorder || !outBuf || outSize == 0) {
        return false;
    }
    outBuf[0] = '\0';

    SAFEARRAY* paths = nullptr;
    HRESULT hr = recorder->get_VolumePathNames(&paths);
    if (FAILED(hr) || !paths) {
        if (paths) SafeArrayDestroy(paths);
        return false;
    }

    bool ok = false;
    LONG lower = 0;
    LONG upper = -1;
    if (SUCCEEDED(SafeArrayGetLBound(paths, 1, &lower)) &&
        SUCCEEDED(SafeArrayGetUBound(paths, 1, &upper)) && upper >= lower) {
        const UINT elemSize = SafeArrayGetElemsize(paths);
        VARTYPE elemType = VT_EMPTY;
        if (FAILED(SafeArrayGetVartype(paths, &elemType))) {
            elemType = VT_EMPTY;
        }
        /* The physical element size is authoritative: a SAFEARRAY(VARIANT)
         * is 16 bytes on x64 while a SAFEARRAY(BSTR) is 8 bytes. */
        if (elemSize == sizeof(VARIANT)) {
            elemType = VT_VARIANT;
        } else if (elemSize == sizeof(BSTR)) {
            elemType = VT_BSTR;
        }

        if (elemType == VT_VARIANT) {
            VARIANT value;
            VariantInit(&value);
            if (SUCCEEDED(SafeArrayGetElement(paths, &lower, &value)) &&
                value.vt == VT_BSTR && value.bstrVal) {
                WideCharToMultiByte(CP_UTF8, 0, value.bstrVal, -1, outBuf,
                                    static_cast<int>(outSize - 1), nullptr, nullptr);
                outBuf[outSize - 1] = '\0';
                ok = true;
            }
            VariantClear(&value);
        } else if (elemType == VT_BSTR) {
            BSTR first = nullptr;
            if (SUCCEEDED(SafeArrayGetElement(paths, &lower, &first)) && first) {
                WideCharToMultiByte(CP_UTF8, 0, first, -1, outBuf,
                                    static_cast<int>(outSize - 1), nullptr, nullptr);
                outBuf[outSize - 1] = '\0';
                SysFreeString(first);
                ok = true;
            }
        }
    }

    SafeArrayDestroy(paths);
    return ok;
}

/* IDiscFormat2Data::get_SupportedWriteSpeedDescriptors() returns a
 * SAFEARRAY(VARIANT) with VT_DISPATCH elements. Accept a plain
 * SAFEARRAY(IDispatch*) as a compatibility fallback. The returned pointer
 * owns one reference. */
static bool GetDispatchElement(SAFEARRAY* arr, LONG index, IDispatch** out) {
    if (!out) return false;
    *out = nullptr;
    if (!arr) return false;

    const UINT elemSize = SafeArrayGetElemsize(arr);
    VARTYPE vt = VT_EMPTY;
    if (FAILED(SafeArrayGetVartype(arr, &vt))) {
        vt = VT_EMPTY;
    }
    if (elemSize == sizeof(VARIANT)) {
        VARIANT value;
        VariantInit(&value);
        bool ok = false;
        if (SUCCEEDED(SafeArrayGetElement(arr, &index, &value))) {
            IUnknown* unk = nullptr;
            if (value.vt == VT_DISPATCH && value.pdispVal) unk = value.pdispVal;
            else if (value.vt == VT_UNKNOWN && value.punkVal) unk = value.punkVal;
            if (unk && SUCCEEDED(unk->QueryInterface(IID_IDispatch, (void**)out))) {
                ok = true;
            }
        }
        VariantClear(&value);
        return ok;
    }
    if (elemSize == sizeof(IDispatch*) && vt == VT_UNKNOWN) {
        IUnknown* unk = nullptr;
        if (SUCCEEDED(SafeArrayGetElement(arr, &index, &unk)) && unk) {
            const bool ok = SUCCEEDED(unk->QueryInterface(IID_IDispatch, (void**)out));
            unk->Release();
            return ok;
        }
        return false;
    }
    if (elemSize == sizeof(IDispatch*) && (vt == VT_DISPATCH || vt == VT_EMPTY)) {
        IDispatch* d = nullptr;
        if (SUCCEEDED(SafeArrayGetElement(arr, &index, &d)) && d) {
            *out = d;
            return true;
        }
    }
    return false;
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

static std::string HrToHex(HRESULT hr) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%08lX", (unsigned long)hr);
    return std::string(buf);
}

/* Probe writability through a read-only handle. Some virtual recorders
 * (e.g. KernSafe in read-only mode) mark their device write-protected at
 * the driver level; IMAPI2 then fails InitializeDiscRecorder with
 * E_ACCESSDENIED and there is no way to burn. Knowing this up front lets
 * callers skip cleanly instead of dying deep inside IMAPI2. */
static bool ProbeDeviceWritable(const wchar_t* deviceInterface) {
    HANDLE h = CreateFileW(deviceInterface, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD bytes = 0;
    BOOL writable = DeviceIoControl(h, IOCTL_DISK_IS_WRITABLE, nullptr, 0,
                                    nullptr, 0, &bytes, nullptr);
    CloseHandle(h);
    return writable != FALSE;
}

/* Fallback device identity when InitializeDiscRecorder fails: walk the
 * drive letters and claim an unclaimed DRIVE_CDROM letter. With one
 * optical device this resolves exactly; with several it is a best effort
 * and the device is additionally flagged not-writable when probing says
 * so. */
static bool FindUnclaimedCdromLetter(std::vector<std::string>& claimed,
                                     char* out, size_t outSize) {
    for (char letter = 'A'; letter <= 'Z'; ++letter) {
        char root[4] = { letter, ':', '\\', '\0' };
        if (GetDriveTypeA(root) != DRIVE_CDROM) {
            continue;
        }
        std::string path = std::string(1, letter) + ":";
        bool taken = false;
        for (size_t i = 0; i < claimed.size(); ++i) {
            if (claimed[i] == path) { taken = true; break; }
        }
        if (taken) continue;
        strncpy(out, path.c_str(), outSize - 1);
        out[outSize - 1] = '\0';
        claimed.push_back(path);
        return true;
    }
    if (outSize > 0) out[0] = '\0';
    return false;
}

/* Read-only IMAPI2 check on an already opened recorder: is there a usable
 * writable disc in the drive? InitializeDiscRecorder() succeeds for a drive
 * with an empty tray, so listing it as writable sends callers into Write()
 * where IMAPI2 fails with 0xC0AA0406. Return a BURN_MEDIA_STATE_* bitmask
 * so DeviceInfo can report the truth. */
static int ProbeMediaState(IDiscRecorder2* recorder) {
    if (!recorder) return 0;
    IDiscFormat2Data* writer = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_MsftDiscFormat2Data, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_IDiscFormat2Data,
                                  (void**)&writer);
    if (FAILED(hr) || !writer) {
        if (writer) writer->Release();
        return 0;
    }

    int state = BURN_MEDIA_STATE_RECORDER_OK;
    writer->put_Recorder(recorder);

    IMAPI_MEDIA_PHYSICAL_TYPE mt = IMAPI_MEDIA_TYPE_UNKNOWN;
    HRESULT mediaHr = writer->get_CurrentPhysicalMediaType(&mt);
    /* A real drive answers BECOMING_READY / INCOMPATIBLE / NO_MEDIA for a
     * short while after insertion. Retry briefly before declaring the tray
     * empty, so list-devices does not hide a usable burner. */
    for (int attempt = 0; attempt < 3 && FAILED(mediaHr) &&
         (mediaHr == E_IMAPI_RECORDER_MEDIA_BECOMING_READY ||
          mediaHr == E_IMAPI_RECORDER_MEDIA_FORMAT_IN_PROGRESS ||
          mediaHr == E_IMAPI_RECORDER_MEDIA_INCOMPATIBLE ||
          mediaHr == E_IMAPI_RECORDER_MEDIA_NO_MEDIA); ++attempt) {
        Sleep(500);
        mediaHr = writer->get_CurrentPhysicalMediaType(&mt);
    }
    if (SUCCEEDED(mediaHr) && mt != IMAPI_MEDIA_TYPE_UNKNOWN) {
        state |= BURN_MEDIA_STATE_MEDIA_PRESENT;
    }

    VARIANT_BOOL supported = VARIANT_FALSE;
    if (SUCCEEDED(writer->IsCurrentMediaSupported(recorder, &supported)) &&
        supported == VARIANT_TRUE) {
        state |= BURN_MEDIA_STATE_MEDIA_SUPPORTED;
    }

    IMAPI_FORMAT2_DATA_MEDIA_STATE ms = IMAPI_FORMAT2_DATA_MEDIA_STATE_UNKNOWN;
    if (SUCCEEDED(writer->get_CurrentMediaStatus(&ms))) {
        if (ms & IMAPI_FORMAT2_DATA_MEDIA_STATE_BLANK)
            state |= BURN_MEDIA_STATE_BLANK;
        if (ms & IMAPI_FORMAT2_DATA_MEDIA_STATE_APPENDABLE)
            state |= BURN_MEDIA_STATE_APPENDABLE;
        if (ms & IMAPI_FORMAT2_DATA_MEDIA_STATE_OVERWRITE_ONLY)
            state |= BURN_MEDIA_STATE_OVERWRITE_ONLY;
        if (ms & IMAPI_FORMAT2_DATA_MEDIA_STATE_WRITE_PROTECTED)
            state |= BURN_MEDIA_STATE_WRITE_PROTECTED;
        if (ms & IMAPI_FORMAT2_DATA_MEDIA_STATE_FINALIZED)
            state |= BURN_MEDIA_STATE_FINALIZED;
        if (ms & IMAPI_FORMAT2_DATA_MEDIA_STATE_DAMAGED)
            state |= BURN_MEDIA_STATE_DAMAGED;
    }
    writer->Release();

    const int writableStates = BURN_MEDIA_STATE_BLANK |
                               BURN_MEDIA_STATE_APPENDABLE |
                               BURN_MEDIA_STATE_OVERWRITE_ONLY;
    const int blockingStates = BURN_MEDIA_STATE_WRITE_PROTECTED |
                               BURN_MEDIA_STATE_FINALIZED |
                               BURN_MEDIA_STATE_DAMAGED;
    if ((state & BURN_MEDIA_STATE_MEDIA_PRESENT) &&
        (state & BURN_MEDIA_STATE_MEDIA_SUPPORTED) &&
        (state & writableStates) && !(state & blockingStates)) {
        state |= BURN_MEDIA_STATE_READY;
    }
    return state;
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
        std::vector<std::string> claimedPaths;

        IDiscMaster2* pDiscMaster = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_MsftDiscMaster2, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IDiscMaster2, (void**)&pDiscMaster);
        if (FAILED(hr)) {
            /* Report honestly: no fabricated fallback devices. Callers and
             * automated tests must see an empty list, not a phantom "D:". */
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "Failed to create IDiscMaster2 (HRESULT 0x%08lX)",
                     (unsigned long)hr);
            SetBurnLastError(msg);
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
                            const int mediaState = ProbeMediaState(pRecorder);
                            info.isWritable =
                                (mediaState & BURN_MEDIA_STATE_READY) ? 1 : 0;
                            info.isEmpty =
                                (mediaState & BURN_MEDIA_STATE_BLANK) ? 1 : 0;
                            strncpy(info.mediaType, "CD/DVD", sizeof(info.mediaType) - 1);

                            if (!GetFirstVolumePath(pRecorder, info.devicePath,
                                                    sizeof(info.devicePath))) {
                                FindUnclaimedCdromLetter(claimedPaths, info.devicePath,
                                                         sizeof(info.devicePath));
                            } else {
                                claimedPaths.push_back(info.devicePath);
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
                        } else {
                            /* Recorder enumerated but cannot be opened
                             * exclusively (write-protected driver, media
                             * locked by another process, or missing
                             * privileges). Keep it visible with an honest
                             * writability flag so tests can SKIP with a
                             * precise reason instead of "device not found". */
                            DeviceInfo info = {};
                            wchar_t* devIf = reinterpret_cast<wchar_t*>(uniqueId);
                            info.isWritable =
                                ProbeDeviceWritable(devIf) ? 1 : 0;
                            info.isEmpty = 0;  /* unknown: recorder not open */
                            strncpy(info.mediaType, "CD/DVD", sizeof(info.mediaType) - 1);
                            if (!FindUnclaimedCdromLetter(claimedPaths, info.devicePath,
                                                          sizeof(info.devicePath))) {
                                strncpy(info.devicePath, "?", sizeof(info.devicePath) - 1);
                            }
                            snprintf(info.deviceName, sizeof(info.deviceName),
                                     "Optical drive (recorder init failed, "
                                     "HRESULT 0x%08lX)",
                                     (unsigned long)hr);
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
        HRESULT lastInitFailure = S_OK;
        for (long i = 0; i < count; ++i) {
            BSTR uniqueId = nullptr;
            pDiscMaster->get_Item(i, &uniqueId);
            if (uniqueId) {
                IDiscRecorder2* pRec = nullptr;
                if (SUCCEEDED(CoCreateInstance(CLSID_MsftDiscRecorder2, nullptr, CLSCTX_INPROC_SERVER,
                                              IID_IDiscRecorder2, (void**)&pRec))) {
                    HRESULT initHr = pRec->InitializeDiscRecorder(uniqueId);
                    if (SUCCEEDED(initHr)) {
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
                        lastInitFailure = initHr;
                        pRec->Release();
                    }
                }
                SysFreeString(uniqueId);
            }
        }

        if (!pRecorder) {
            if (FAILED(lastInitFailure)) {
                SetBurnLastError(
                    "Recorder present but cannot be opened exclusively "
                    "(HRESULT 0x" + HrToHex(lastInitFailure) +
                    "). The device may be write-protected by its driver, "
                    "busy, or locked by another process.");
            } else {
                SetBurnLastError("Device not found: " + devicePath);
            }
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
            /* SetWriteSpeed() counts DISC SECTORS per second (2048 B each),
             * not speed multipliers. 1x is media-family specific:
             *   CD 1x = 75 sectors/s        (176.4 kB/s)
             *   DVD 1x = 676 sectors/s      (1385 kB/s)
             *   BD 1x  = 2195 sectors/s     (4496 kB/s)
             * The old code passed speed*1000, so 4x requested 4000 s/s =
             * ~53x CD / ~6x DVD. IMAPI2 silently clamps an unsupported
             * value to the nearest supported one (S_IMAPI_SPEEDADJUSTED),
             * which made the wrong unit invisible - but the multiplier was
             * still meaningless.
             *
             * Media family comes from the first IWriteSpeedDescriptor the
             * recorder reports (IDiscFormat2Data has no media-family
             * property; this is the only header-verified source). If that
             * fails we fall back to the requested disc type text, and if
             * even that is unusable we leave the drive at its default
             * "fastest supported" speed instead of sending a number whose
             * unit we cannot establish. */
            int baseSectors = 0;
            SAFEARRAY* descs = nullptr;
            if (SUCCEEDED(pDataWriter->get_SupportedWriteSpeedDescriptors(&descs))
                && descs) {
                LONG lb = 0, ub = -1;
                if (SUCCEEDED(SafeArrayGetLBound(descs, 1, &lb)) &&
                    SUCCEEDED(SafeArrayGetUBound(descs, 1, &ub)) && ub >= lb) {
                    IDispatch* d = nullptr;
                    if (GetDispatchElement(descs, lb, &d) && d) {
                        IWriteSpeedDescriptor* wsd = nullptr;
                        if (SUCCEEDED(d->QueryInterface(__uuidof(IWriteSpeedDescriptor),
                                                        (void**)&wsd)) && wsd) {
                            IMAPI_MEDIA_PHYSICAL_TYPE mt = IMAPI_MEDIA_TYPE_UNKNOWN;
                            if (SUCCEEDED(wsd->get_MediaType(&mt))) {
                                switch (mt) {
                                case IMAPI_MEDIA_TYPE_CDROM: case IMAPI_MEDIA_TYPE_CDR:
                                case IMAPI_MEDIA_TYPE_CDRW: case IMAPI_MEDIA_TYPE_DISK:
                                    baseSectors = 75; break;
                                case IMAPI_MEDIA_TYPE_BDROM: case IMAPI_MEDIA_TYPE_BDR:
                                case IMAPI_MEDIA_TYPE_BDRE: case IMAPI_MEDIA_TYPE_HDDVDROM:
                                case IMAPI_MEDIA_TYPE_HDDVDR: case IMAPI_MEDIA_TYPE_HDDVDRAM:
                                    baseSectors = 2195; break;
                                default:
                                    if (mt != IMAPI_MEDIA_TYPE_UNKNOWN) baseSectors = 676;
                                    break;
                                }
                            }
                            wsd->Release();
                        }
                        d->Release();
                    }
                }
                SafeArrayDestroy(descs);
            }
            if (baseSectors == 0 && !discType.empty()) {
                char c = discType[0];
                if (c == 'C' || c == 'c') baseSectors = 75;
                else if (c == 'B' || c == 'b') baseSectors = 2195;
                else baseSectors = 676;
            }
            if (baseSectors > 0) {
                long long target = (long long)speed * baseSectors;
                if (target > 0x7FFFFFFFLL) target = 0x7FFFFFFFLL;
                /* Deliberately not failing on a rejected speed: the drive
                 * keeps its own default when the hint cannot be honoured. */
                HRESULT spdHr = pDataWriter->SetWriteSpeed((LONG)target,
                                                           VARIANT_FALSE);
                (void)spdHr;
            }
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

        /* Success first, cancellation second: Write() returned S_OK only if
         * the disc was actually written. A cancel that arrived while the
         * last sectors were being burned (the CancelWrite hook raced the
         * finish line) must not turn a good disc into a "cancelled" result -
         * the caller would skip verification on complete data. */
        if (SUCCEEDED(hr)) {
            NotifyProgressBurn(100, "Burn completed");
            return true;
        }
        if (IsBurnCancelled()) {
            SetBurnLastError("Burn cancelled by caller");
            return false;
        }
        {
            char msg[128];
            snprintf(msg, sizeof(msg), "Burn write failed (HRESULT 0x%08lX)",
                     (unsigned long)hr);
            SetBurnLastError(msg);
        }
        return false;
    }

    std::string GetPlatformInfo() override {
        return "Windows (IMAPI2)";
    }

    /* ---- API version 3 capabilities, implemented in windows_features.cpp ---- */

    int Capability(int capability) override { return burnwin::Capability(capability); }

    long long NextSessionStartBlock(const std::string& devicePath) override {
        return burnwin::NextSessionStartBlock(devicePath);
    }

    bool CreateISO(const std::string& sourcePath, const std::string& isoPath,
                   const std::string& volumeLabel) override {
        BurnImageSpec spec;
        spec.sourcePath = sourcePath;
        spec.outputPath = isoPath;
        spec.volumeLabel = volumeLabel;
        /* fileSystems 0 => ISO9660+Joliet, exactly the historical default. */
        return burnwin::CreateImage(spec);
    }

    bool CreateImage(const BurnImageSpec& spec) override {
        return burnwin::CreateImage(spec);
    }

    /* BurnISO above keeps its own (already verified) implementation; this
     * override only exists to carry the version 3 write options. */
    bool BurnImageFile(const std::string& imagePath, const std::string& devicePath,
                       const std::string& discType, int speed, int verifyLevel,
                       bool finalize, unsigned long long sessionStartBlock) override {
        return burnwin::BurnImageFile(imagePath, devicePath, discType, speed,
                                      verifyLevel, finalize, sessionStartBlock);
    }

    bool BurnAudioTracks(const std::vector<BurnAudioTrackSpec>& tracks,
                         const std::string& devicePath, int speed,
                         int verifyLevel, bool finalizeDisc) override {
        return burnwin::BurnAudioTracks(tracks, devicePath, speed, verifyLevel, finalizeDisc);
    }

    bool EraseDisc(const std::string& devicePath, bool full) override {
        return burnwin::EraseDisc(devicePath, full);
    }

    bool CreateRawImage(const std::vector<BurnAudioTrackSpec>& tracks,
                        const std::string& outputPath, const std::string& mcn,
                        int startingTrackNumber) override {
        return burnwin::CreateRawImage(tracks, outputPath, mcn, startingTrackNumber);
    }

    bool BurnRawImage(const std::string& imagePath, const std::string& devicePath,
                      int speed, int verifyLevel) override {
        return burnwin::BurnRawImage(imagePath, devicePath, speed, verifyLevel);
    }

    bool VerifyWrite(const std::string& devicePath, const std::string& expectedImagePath,
                     int level) override {
        return burnwin::VerifyWrite(devicePath, expectedImagePath, level);
    }

    int EjectTray(const std::string& devicePath) override {
        return burnwin::EjectTray(devicePath);
    }

    int CloseTray(const std::string& devicePath) override {
        return burnwin::CloseTray(devicePath);
    }

    std::string DriveInformation(const std::string& devicePath) override {
        return burnwin::DriveInformation(devicePath);
    }
    std::string MediaType(const std::string& devicePath) override {
        return burnwin::MediaType(devicePath);
    }
    int MediaTypeCode(const std::string& devicePath) override {
        return burnwin::MediaTypeCode(devicePath);
    }
    int MediaState(const std::string& devicePath) override {
        return burnwin::MediaState(devicePath);
    }
    std::string MediaStateText(const std::string& devicePath) override {
        return burnwin::MediaStateText(devicePath);
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
