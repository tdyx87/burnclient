/* ---------------------------------------------------------------------------
 * windows_features.cpp - IMAPI2 implementation of the API version 3 surface.
 *
 * docs/v3_imapi2_api_facts.md records every method name, parameter type and
 * constant used here, each verified against the installed Windows SDK.
 *
 * Layout
 *   helpers      - owning COM guards, UTF-8 <-> BSTR, recorder lookup
 *   OffsetStream - a read-only window over an IStream (a WAV's "data" chunk,
 *                  a RAW track), so a writer sees exactly one track
 *   UpdateSink   - progress from the Data / TrackAtOnce / RawCD event sinks
 *   CancelArm    - interruptible cancel around a blocking IMAPI2 call
 *   features     - the ten capability entry points
 * ------------------------------------------------------------------------- */
#include "windows_features.h"

#ifdef _WIN32

#include "burn_internal.h"
#include "media_format.h"

#include <windows.h>
#include <imapi2.h>
#include <imapi2error.h>   /* E_IMAPI_* HRESULT values for readable failures */
#include <imapi2fs.h>
#include <shlwapi.h>
#include <oleauto.h>
#include <ocidl.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace burnwin {

/* ==========================================================================
 * Helpers
 * ======================================================================== */

static std::string HrHex(HRESULT hr) {
    char b[16];
    snprintf(b, sizeof(b), "0x%08lX", (unsigned long)hr);
    return b;
}

static std::string HrMsg(HRESULT hr, const std::string& what) {
    return what + " failed (HRESULT " + HrHex(hr) + ")";
}

/* Owning COM pointer, released on every exit path, non-copyable. */
template <typename T>
class Com {
public:
    Com() : p_(nullptr) {}
    explicit Com(T* p) : p_(p) {}
    ~Com() { if (p_) p_->Release(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    Com& operator=(Com&& o) noexcept {
        /* No self-check: comparing addresses would call the overloaded
         * operator& below, which yields T**, not the object address.
         * Self-move-assignment is not used and remains safe here. */
        reset();
        p_ = o.p_;
        o.p_ = nullptr;
        return *this;
    }

    T** operator&() { return &p_; }
    T* operator->() const { return p_; }
    T* get() const { return p_; }
    operator bool() const { return p_ != nullptr; }
    T* detach() { T* r = p_; p_ = nullptr; return r; }
    void reset() { if (p_) { p_->Release(); p_ = nullptr; } }

private:
    T* p_;
};

/* Owning BSTR. */
class Bstr {
public:
    Bstr() : b_(nullptr) {}
    explicit Bstr(const std::string& utf8) : b_(nullptr) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
        if (wlen > 0) {
            std::vector<wchar_t> w((size_t)wlen);
            MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, w.data(), wlen);
            b_ = SysAllocString(w.data());
        }
    }
    explicit Bstr(const wchar_t* lit) : b_(SysAllocString(lit)) {}
    ~Bstr() { if (b_) SysFreeString(b_); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    Bstr(Bstr&& o) noexcept : b_(o.b_) { o.b_ = nullptr; }
    Bstr& operator=(Bstr&& o) noexcept {
        if (this != &o) { if (b_) SysFreeString(b_); b_ = o.b_; o.b_ = nullptr; }
        return *this;
    }

    operator BSTR() const { return b_; }
    BSTR* out() { return &b_; }

private:
    BSTR b_;
};

static std::string BstrToUtf8(BSTR b) {
    if (!b) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, b, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return "";
    std::vector<char> s((size_t)len);
    WideCharToMultiByte(CP_UTF8, 0, b, -1, s.data(), len, nullptr, nullptr);
    return std::string(s.data());
}

static std::string TrimTrailing(const std::string& s) {
    std::string r = s;
    while (!r.empty() && (r.back() == '\\' || r.back() == '/')) r.pop_back();
    return r;
}

/* Owning SAFEARRAY handle. */
class SafeArr {
public:
    SafeArr() : a_(nullptr) {}
    ~SafeArr() { if (a_) SafeArrayDestroy(a_); }
    SafeArr(const SafeArr&) = delete;
    SafeArr& operator=(const SafeArr&) = delete;
    SAFEARRAY** out() { return &a_; }
    SAFEARRAY* get() const { return a_; }
    long lower() const {
        LONG lb = 0;
        if (a_) SafeArrayGetLBound(a_, 1, &lb);
        return (long)lb;
    }
    long upper() const {
        LONG ub = -1;
        if (a_) SafeArrayGetUBound(a_, 1, &ub);
        return (long)ub;
    }
private:
    SAFEARRAY* a_;
};

/* IDiscRecorder2::get_VolumePathNames() returns a SAFEARRAY of VARIANT /
 * VT_BSTR values; the path is in bstrVal. Also accept a plain SAFEARRAY(BSTR)
 * for compatibility. SafeArrayGetElement() copies the element, so the copy
 * must be cleared with VariantClear() or SysFreeString(). */
static std::string VolumePathAt(SAFEARRAY* paths, LONG index) {
    if (!paths) return "";
    const UINT elemSize = SafeArrayGetElemsize(paths);
    VARTYPE elemType = VT_EMPTY;
    if (FAILED(SafeArrayGetVartype(paths, &elemType))) {
        elemType = VT_EMPTY;
    }
    if (elemSize == sizeof(VARIANT)) {
        elemType = VT_VARIANT;
    } else if (elemSize == sizeof(BSTR)) {
        elemType = VT_BSTR;
    }

    if (elemType == VT_VARIANT) {
        VARIANT value;
        VariantInit(&value);
        std::string result;
        if (SUCCEEDED(SafeArrayGetElement(paths, &index, &value)) &&
            value.vt == VT_BSTR && value.bstrVal) {
            result = BstrToUtf8(value.bstrVal);
        }
        VariantClear(&value);
        return result;
    }
    if (elemType == VT_BSTR) {
        BSTR text = nullptr;
        std::string result;
        if (SUCCEEDED(SafeArrayGetElement(paths, &index, &text)) && text) {
            result = BstrToUtf8(text);
            SysFreeString(text);
        }
        return result;
    }
    return "";
}

/* Most IMAPI2 SAFEARRAY outputs are SAFEARRAY(VARIANT), but a few are raw
 * arrays of LONG, BSTR or IDispatch*. Reading a VARIANT element into a LONG
 * or IDispatch* copies 16 bytes into a 4/8-byte object and corrupts the
 * stack. Classify by physical element size first, then by the declared
 * VARTYPE, and always use a VARIANT destination for VARIANT arrays. */
enum class SafeArrayLayout { Unknown, Variant, Bstr, Dispatch, Long };

static SafeArrayLayout ClassifySafeArray(SAFEARRAY* arr, VARTYPE& vt) {
    vt = VT_EMPTY;
    if (!arr) return SafeArrayLayout::Unknown;
    const UINT elemSize = SafeArrayGetElemsize(arr);
    if (FAILED(SafeArrayGetVartype(arr, &vt))) {
        vt = VT_EMPTY;
    }
    if (elemSize == sizeof(VARIANT)) return SafeArrayLayout::Variant;
    if (elemSize == sizeof(LONG) &&
        (vt == VT_I4 || vt == VT_INT || vt == VT_UI4 || vt == VT_UINT ||
         vt == VT_EMPTY || vt == VT_VARIANT)) {
        return SafeArrayLayout::Long;
    }
    if (elemSize == sizeof(BSTR) && vt == VT_BSTR) {
        return SafeArrayLayout::Bstr;
    }
    if (elemSize == sizeof(IDispatch*) && (vt == VT_DISPATCH || vt == VT_UNKNOWN)) {
        return SafeArrayLayout::Dispatch;
    }
    switch (vt) {
    case VT_VARIANT: return SafeArrayLayout::Variant;
    case VT_BSTR: return SafeArrayLayout::Bstr;
    case VT_DISPATCH: case VT_UNKNOWN: return SafeArrayLayout::Dispatch;
    case VT_I4: case VT_INT: case VT_UI4: case VT_UINT: return SafeArrayLayout::Long;
    default: return SafeArrayLayout::Unknown;
    }
}

static bool ReadLongElement(SAFEARRAY* arr, LONG index, LONG& value) {
    value = 0;
    VARTYPE vt = VT_EMPTY;
    const SafeArrayLayout layout = ClassifySafeArray(arr, vt);
    if (layout == SafeArrayLayout::Variant) {
        VARIANT v;
        VariantInit(&v);
        bool ok = false;
        if (SUCCEEDED(SafeArrayGetElement(arr, &index, &v))) {
            if (v.vt == VT_I4 || v.vt == VT_INT) {
                value = (LONG)v.lVal;
                ok = true;
            } else if (v.vt == VT_UI4 || v.vt == VT_UINT) {
                value = (LONG)v.ulVal;
                ok = true;
            } else if (v.vt == VT_I2) {
                value = (LONG)v.iVal;
                ok = true;
            } else if (v.vt == VT_UI2) {
                value = (LONG)v.uiVal;
                ok = true;
            }
        }
        VariantClear(&v);
        return ok;
    }
    if (layout == SafeArrayLayout::Long) {
        LONG raw = 0;
        if (SUCCEEDED(SafeArrayGetElement(arr, &index, &raw))) {
            value = raw;
            return true;
        }
    }
    return false;
}

static bool ReadDispatchElement(SAFEARRAY* arr, LONG index, IDispatch** out) {
    if (!out) return false;
    *out = nullptr;
    VARTYPE vt = VT_EMPTY;
    const SafeArrayLayout layout = ClassifySafeArray(arr, vt);
    if (layout == SafeArrayLayout::Variant) {
        VARIANT v;
        VariantInit(&v);
        bool ok = false;
        if (SUCCEEDED(SafeArrayGetElement(arr, &index, &v))) {
            IUnknown* unk = nullptr;
            if (v.vt == VT_DISPATCH && v.pdispVal) unk = v.pdispVal;
            else if (v.vt == VT_UNKNOWN && v.punkVal) unk = v.punkVal;
            if (unk && SUCCEEDED(unk->QueryInterface(IID_IDispatch, (void**)out))) {
                ok = true;
            }
        }
        VariantClear(&v);
        return ok;
    }
    if (layout == SafeArrayLayout::Dispatch) {
        if (vt == VT_UNKNOWN) {
            IUnknown* unk = nullptr;
            if (SUCCEEDED(SafeArrayGetElement(arr, &index, &unk)) && unk) {
                const bool ok = SUCCEEDED(unk->QueryInterface(IID_IDispatch, (void**)out));
                unk->Release();
                return ok;
            }
            return false;
        }
        IDispatch* d = nullptr;
        if (SUCCEEDED(SafeArrayGetElement(arr, &index, &d)) && d) {
            *out = d;
            return true;
        }
    }
    return false;
}

/* ==========================================================================
 * Recorder discovery. A recorder is matched ONLY by exact volume path:
 * writing to the wrong device is unrecoverable, so there is deliberately no
 * "fall back to the first drive" behaviour.
 * ======================================================================== */
static bool FindRecorderId(const std::string& devicePath, Bstr& outId) {
    outId = Bstr();
    if (devicePath.empty()) {
        SetBurnLastError("No burning device specified");
        return false;
    }

    Com<IDiscMaster2> master;
    HRESULT hr = CoCreateInstance(CLSID_MsftDiscMaster2, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IDiscMaster2, (void**)&master);
    if (FAILED(hr)) {
        SetBurnLastError(HrMsg(hr, "CoCreateInstance(IDiscMaster2)"));
        return false;
    }

    long count = 0;
    master->get_Count(&count);
    const std::string want = TrimTrailing(devicePath);
    bool found = false;

    for (long i = 0; i < count && !found; ++i) {
        Bstr uniqueId;
        if (FAILED(master->get_Item(i, uniqueId.out())) || !uniqueId) continue;

        Com<IDiscRecorder2> rec;
        if (FAILED(CoCreateInstance(CLSID_MsftDiscRecorder2, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IDiscRecorder2, (void**)&rec)) || !rec) {
            continue;
        }
        const HRESULT ih = rec->InitializeDiscRecorder(uniqueId);
        if (FAILED(ih)) {
            /* The recorder is enumerated but will not open (write-protected
             * driver, busy, another process). Accept a match against its
             * device-interface id so callers still get the precise
             * "cannot be opened" error from OpenRecorder rather than a
             * misleading "not found". */
            if (!_stricmp(BstrToUtf8(uniqueId).c_str(), want.c_str())) {
                outId = Bstr(BstrToUtf8(uniqueId));
                found = true;
            }
            continue;
        }

        SAFEARRAY* paths = nullptr;
        if (SUCCEEDED(rec->get_VolumePathNames(&paths)) && paths) {
            SafeArr guard;
            *guard.out() = paths;
            LONG lb = 0, ub = -1;
            SafeArrayGetLBound(paths, 1, &lb);
            SafeArrayGetUBound(paths, 1, &ub);
            for (LONG k = lb; k <= ub && !found; ++k) {
                const std::string got = TrimTrailing(VolumePathAt(paths, k));
                if (!got.empty() && !_stricmp(got.c_str(), want.c_str())) {
                    outId = Bstr(BstrToUtf8(uniqueId));
                    found = true;
                }
            }
        }
    }

    if (!found) {
        SetBurnLastError("Recorder not found for device path '" + devicePath +
                         "'. Devices are matched by exact volume path (for example \"E:\"); "
                         "call ListBurningDevices() to see what is present.");
    }
    return found;
}

static Com<IDiscRecorder2> OpenRecorder(BSTR uniqueId) {
    Com<IDiscRecorder2> rec;
    HRESULT hr = CoCreateInstance(CLSID_MsftDiscRecorder2, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IDiscRecorder2, (void**)&rec);
    if (FAILED(hr)) {
        SetBurnLastError(HrMsg(hr, "CoCreateInstance(IDiscRecorder2)"));
        return rec;
    }
    hr = rec->InitializeDiscRecorder(uniqueId);
    if (FAILED(hr)) {
        rec.reset();
        SetBurnLastError("Recorder cannot be opened exclusively (HRESULT " + HrHex(hr) +
                         "). The device may be write-protected by its driver, busy, "
                         "or locked by another process.");
    }
    return rec;
}

static Com<IStream> OpenFileStream(const std::string& path) {
    /* A directory is never a valid image stream. SHCreateStreamOnFileA
     * reports 0x80070005 for it, which callers were mistaking for a
     * recorder or permission fault. Say what actually went wrong. */
    const DWORD attrs = GetFileAttributesA(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES &&
        (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        SetBurnLastError("Cannot use a directory as an image file: " + path +
                         ". Build an ISO from it first (BURN_JOB_MODE_DIRECTORY).");
        return Com<IStream>();
    }
    IStream* raw = nullptr;
    HRESULT hr = SHCreateStreamOnFileA(path.c_str(), STGM_READ, &raw);
    Com<IStream> s(raw);
    if (FAILED(hr) || !s) {
        SetBurnLastError("Failed to open file for reading: " + path + " (" + HrHex(hr) + ")");
        s.reset();
    }
    return s;
}

/* Every writer interface in IMAPI2 exposes put_ClientName; call it through
 * a template so the compiler checks each one. */
template <typename W>
static void SetClientName(W* writer) {
    if (!writer) return;
    Bstr name(L"BurningUtility");
    writer->put_ClientName(name);
}

/* ==========================================================================
 * OffsetStream - a read-only window [offset, offset+length) over a base
 * stream. AddAudioTrack()/AddTrack() require a stream holding exactly one
 * track's payload, and a WAV's audio starts at its "data" chunk, so the
 * window is what makes passing a whole file correct.
 * ======================================================================== */
class OffsetStream : public IStream {
public:
    OffsetStream(IStream* base, unsigned long long offset, unsigned long long length)
        : refs_(1), base_(base), offset_(offset), length_(length), pos_(0) {
        base_->AddRef();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IStream) {
            *ppv = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG left = (ULONG)InterlockedDecrement(&refs_);
        if (left == 0) { base_->Release(); delete this; }
        return left;
    }

    STDMETHODIMP Read(void* pv, ULONG cb, ULONG* cbRead) override {
        const unsigned long long remaining = (pos_ < length_) ? (length_ - pos_) : 0;
        ULONG want = (unsigned long long)cb < remaining ? cb : (ULONG)remaining;
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)(offset_ + pos_);
        HRESULT hr = base_->Seek(li, STREAM_SEEK_SET, nullptr);
        if (FAILED(hr)) { if (cbRead) *cbRead = 0; return hr; }
        ULONG got = 0;
        hr = base_->Read(pv, want, &got);
        if (cbRead) *cbRead = got;
        if (SUCCEEDED(hr)) pos_ += got;
        return hr;
    }
    STDMETHODIMP Write(const void*, ULONG, ULONG*) override { return E_ACCESSDENIED; }

    STDMETHODIMP Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin,
                      ULARGE_INTEGER* plibNewPosition) override {
        unsigned long long np = pos_;
        if (dwOrigin == STREAM_SEEK_SET) np = (unsigned long long)dlibMove.QuadPart;
        else if (dwOrigin == STREAM_SEEK_CUR) np = pos_ + (unsigned long long)dlibMove.QuadPart;
        else if (dwOrigin == STREAM_SEEK_END) np = length_ + (unsigned long long)dlibMove.QuadPart;
        else return E_INVALIDARG;
        if (np > length_) np = length_;
        pos_ = np;
        if (plibNewPosition) plibNewPosition->QuadPart = (ULONGLONG)pos_;
        return S_OK;
    }
    STDMETHODIMP SetSize(ULARGE_INTEGER) override { return E_ACCESSDENIED; }

    STDMETHODIMP CopyTo(IStream* pstm, ULARGE_INTEGER cb, ULARGE_INTEGER* read,
                        ULARGE_INTEGER* written) override {
        const unsigned long long chunk = 65536;
        unsigned long long total = (cb.QuadPart == ~0ULL) ? (length_ - pos_) : cb.QuadPart;
        std::vector<unsigned char> buf((size_t)chunk);
        unsigned long long done = 0;
        while (done < total) {
            ULONG want = (ULONG)((total - done < chunk) ? (total - done) : chunk);
            ULONG got = 0;
            HRESULT hr = Read(buf.data(), want, &got);
            if (FAILED(hr) || got == 0) break;
            ULONG put = 0;
            hr = pstm->Write(buf.data(), got, &put);
            if (FAILED(hr) || put != got) return FAILED(hr) ? hr : E_FAIL;
            done += got;
        }
        if (read) read->QuadPart = (ULONGLONG)done;
        if (written) written->QuadPart = (ULONGLONG)done;
        return S_OK;
    }
    STDMETHODIMP Commit(DWORD) override { return S_OK; }
    STDMETHODIMP Revert() override { return S_OK; }
    STDMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }

    STDMETHODIMP Stat(STATSTG* pstatstg, DWORD grfStatFlag) override {
        if (!pstatstg) return E_POINTER;
        memset(pstatstg, 0, sizeof(*pstatstg));
        pstatstg->cbSize.QuadPart = (ULONGLONG)length_;
        pstatstg->type = STGTY_STREAM;
        (void)grfStatFlag;   /* pwName is deliberately left empty */
        return S_OK;
    }
    STDMETHODIMP Clone(IStream** ppstm) override {
        if (!ppstm) return E_POINTER;
        IStream* dup = nullptr;
        HRESULT hr = base_->Clone(&dup);
        if (FAILED(hr) || !dup) return FAILED(hr) ? hr : E_FAIL;
        *ppstm = new OffsetStream(dup, offset_, length_);
        dup->Release();
        return S_OK;
    }

private:
    ~OffsetStream() {}
    volatile LONG refs_;
    IStream* base_;
    unsigned long long offset_;
    unsigned long long length_;
    unsigned long long pos_;
};

/* Wrap one audio file's sector-aligned CD-DA payload as an IStream. */
static Com<IStream> AudioTrackStream(const std::string& path) {
    burn_media::AudioPayload p = burn_media::Probe(path);
    if (!p.ok) {
        SetBurnLastError(p.error.empty() ? "Unreadable audio file: " + path : p.error);
        return Com<IStream>();
    }
    const unsigned long long bytes = p.sectorAlignedBytes();
    if (bytes == 0) {
        SetBurnLastError("Audio track is shorter than one CD sector (2352 bytes): " + path);
        return Com<IStream>();
    }
    Com<IStream> base = OpenFileStream(path);
    if (!base) return Com<IStream>();
    return Com<IStream>(new OffsetStream(base.get(), p.dataOffset, bytes));
}

/* ==========================================================================
 * UpdateSink - the IDispatch sink for
 * DDiscFormat2DataEvents / DDiscFormat2TrackAtOnceEvents /
 * DDiscFormat2RawCDEvents. All three fire Update(...) at DISPID 0x200.
 * ======================================================================== */
class UpdateSink : public IDispatch {
public:
    explicit UpdateSink(const char* label) : refs_(1), label_(label) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IDispatch) {
            *ppv = static_cast<IDispatch*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG left = (ULONG)InterlockedDecrement(&refs_);
        if (left == 0) delete this;
        return left;
    }
    STDMETHODIMP GetTypeInfoCount(UINT* n) override { if (n) *n = 0; return S_OK; }
    STDMETHODIMP GetTypeInfo(UINT, LCID, ITypeInfo** t) override {
        if (t) *t = nullptr;
        return E_NOTIMPL;
    }
    STDMETHODIMP GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP Invoke(DISPID dispid, REFIID, LCID, WORD, DISPPARAMS* params,
                        VARIANT*, EXCEPINFO*, UINT*) override {
        if (dispid != 0x200 || !params || params->cArgs < 1 || !params->rgvarg) return S_OK;
        /* DISPPARAMS arguments arrive in reverse order. */
        VARIANTARG& first = params->rgvarg[0];
        if (first.vt != VT_DISPATCH || !first.pdispVal) return S_OK;

        long elapsed = 0, remaining = 0;
        IDiscFormat2DataEventArgs* a1 = nullptr;
        IDiscFormat2TrackAtOnceEventArgs* a2 = nullptr;
        IDiscFormat2RawCDEventArgs* a3 = nullptr;
        if (SUCCEEDED(first.pdispVal->QueryInterface(IID_IDiscFormat2DataEventArgs,
                                                     (void**)&a1)) && a1) {
            a1->get_ElapsedTime(&elapsed);
            a1->get_RemainingTime(&remaining);
            a1->Release();
        } else if (SUCCEEDED(first.pdispVal->QueryInterface(IID_IDiscFormat2TrackAtOnceEventArgs,
                                                             (void**)&a2)) && a2) {
            a2->get_ElapsedTime(&elapsed);
            a2->get_RemainingTime(&remaining);
            a2->Release();
        } else if (SUCCEEDED(first.pdispVal->QueryInterface(IID_IDiscFormat2RawCDEventArgs,
                                                             (void**)&a3)) && a3) {
            a3->get_ElapsedTime(&elapsed);
            a3->get_RemainingTime(&remaining);
            a3->Release();
        } else {
            return S_OK;
        }

        long total = elapsed + remaining;
        int percent = total > 0 ? (int)(elapsed * 100L / total) : 50;
        if (percent < 0) percent = 0;
        if (percent > 99) percent = 99;
        char msg[128];
        snprintf(msg, sizeof(msg), "%s... %d%%", label_, percent);
        NotifyProgressBurn(percent, msg);
        return S_OK;
    }

    /* Subscribe to a writer's Update event. Returns false when the writer
     * refuses the connection point - the burn still proceeds, just with
     * coarser progress. */
    static bool Attach(IUnknown* writer, REFIID eventsIid, const char* label,
                       IConnectionPoint** outCp, DWORD* outCookie) {
        *outCp = nullptr;
        *outCookie = 0;
        Com<IConnectionPointContainer> cpc;
        if (FAILED(writer->QueryInterface(IID_IConnectionPointContainer, (void**)&cpc)) || !cpc)
            return false;
        Com<IConnectionPoint> cp;
        if (FAILED(cpc->FindConnectionPoint(eventsIid, &cp)) || !cp) return false;

        UpdateSink* sink = new UpdateSink(label);
        DWORD cookie = 0;
        HRESULT hr = cp->Advise(static_cast<IDispatch*>(sink), &cookie);
        sink->Release();   /* Advise() holds its own reference now */
        if (FAILED(hr)) return false;
        *outCookie = cookie;
        *outCp = cp.detach();
        return true;
    }
    static void Detach(IConnectionPoint* cp, DWORD cookie) {
        if (!cp) return;
        if (cookie) cp->Unadvise(cookie);
        cp->Release();
    }

private:
    volatile LONG refs_;
    const char* label_;
};

/* Subscribe + RAII unadvise. */
class EventSubscription {
public:
    EventSubscription() = default;
    ~EventSubscription() { UpdateSink::Detach(cp_, cookie_); }
    EventSubscription(const EventSubscription&) = delete;
    EventSubscription& operator=(const EventSubscription&) = delete;
    bool attach(IUnknown* writer, REFIID iid, const char* label) {
        return UpdateSink::Attach(writer, iid, label, &cp_, &cookie_);
    }
private:
    IConnectionPoint* cp_ = nullptr;
    DWORD cookie_ = 0;
};

/* ==========================================================================
 * Cancellation around a blocking write. The slot lives on the caller's
 * stack; ClearBurnCancelHook() waits for in-flight hooks to return, so the
 * target is nulled before clearing.
 * ======================================================================== */
enum class WriterKind { None, Data, TrackAtOnce, RawCD };

struct CancelSlot {
    std::mutex mutex;
    IUnknown* target = nullptr;
    WriterKind kind = WriterKind::None;
};

static void CancelHookFn(void* ctx) {
    CancelSlot* slot = static_cast<CancelSlot*>(ctx);
    std::lock_guard<std::mutex> lock(slot->mutex);
    if (!slot->target) return;
    if (slot->kind == WriterKind::Data) {
        IDiscFormat2Data* w = nullptr;
        if (SUCCEEDED(slot->target->QueryInterface(IID_IDiscFormat2Data, (void**)&w)) && w) {
            w->CancelWrite();
            w->Release();
        }
    } else if (slot->kind == WriterKind::TrackAtOnce) {
        IDiscFormat2TrackAtOnce* w = nullptr;
        if (SUCCEEDED(slot->target->QueryInterface(IID_IDiscFormat2TrackAtOnce, (void**)&w)) && w) {
            w->CancelAddTrack();
            w->Release();
        }
    } else if (slot->kind == WriterKind::RawCD) {
        IDiscFormat2RawCD* w = nullptr;
        if (SUCCEEDED(slot->target->QueryInterface(IID_IDiscFormat2RawCD, (void**)&w)) && w) {
            w->CancelWrite();
            w->Release();
        }
    }
    /* EraseMedia() is documented as non-cancellable: nothing to do. */
}

class CancelArm {
public:
    explicit CancelArm(CancelSlot& slot) : slot_(&slot) {
        SetBurnCancelHook(CancelHookFn, slot_);
    }
    ~CancelArm() {
        {
            std::lock_guard<std::mutex> lock(slot_->mutex);
            slot_->target = nullptr;
            slot_->kind = WriterKind::None;
        }
        ClearBurnCancelHook(CancelHookFn, slot_);
    }
    CancelArm(const CancelArm&) = delete;
    CancelArm& operator=(const CancelArm&) = delete;
    void bind(IUnknown* target, WriterKind kind) {
        std::lock_guard<std::mutex> lock(slot_->mutex);
        slot_->target = target;
        slot_->kind = kind;
    }
private:
    CancelSlot* slot_;
};

/* ==========================================================================
 * Speed: the public `speed` is a multiplier; IMAPI2 wants sectors/second.
 * CD 1x = 75, DVD 1x = 676, BD 1x = 2195 (2048-byte sectors).
 * ======================================================================== */
static int BaseSectorsPerSecond(IDiscFormat2Data* writer, const std::string& discType) {
    int base = 0;
    if (writer) {
        SafeArr descs;
        if (SUCCEEDED(writer->get_SupportedWriteSpeedDescriptors(descs.out())) && descs.get()) {
            const long lb = descs.lower(), ub = descs.upper();
            if (ub >= lb) {
                IDispatch* d = nullptr;
                if (ReadDispatchElement(descs.get(), (LONG)lb, &d) && d) {
                    IWriteSpeedDescriptor* wsd = nullptr;
                    if (SUCCEEDED(d->QueryInterface(IID_IWriteSpeedDescriptor, (void**)&wsd))
                        && wsd) {
                        IMAPI_MEDIA_PHYSICAL_TYPE mt = IMAPI_MEDIA_TYPE_UNKNOWN;
                        if (SUCCEEDED(wsd->get_MediaType(&mt))) {
                            switch (mt) {
                            case IMAPI_MEDIA_TYPE_CDROM: case IMAPI_MEDIA_TYPE_CDR:
                            case IMAPI_MEDIA_TYPE_CDRW: case IMAPI_MEDIA_TYPE_DISK:
                                base = 75; break;
                            case IMAPI_MEDIA_TYPE_BDROM: case IMAPI_MEDIA_TYPE_BDR:
                            case IMAPI_MEDIA_TYPE_BDRE: case IMAPI_MEDIA_TYPE_HDDVDROM:
                            case IMAPI_MEDIA_TYPE_HDDVDR: case IMAPI_MEDIA_TYPE_HDDVDRAM:
                                base = 2195; break;
                            default:
                                if (mt != IMAPI_MEDIA_TYPE_UNKNOWN) base = 676;
                                break;
                            }
                        }
                        wsd->Release();
                    }
                    d->Release();
                }
            }
        }
    }
    if (base == 0 && !discType.empty()) {
        char c = discType[0];
        if (c == 'C' || c == 'c') base = 75;
        else if (c == 'B' || c == 'b') base = 2195;
        else base = 676;
    }
    return base;
}

static LONG MultiplierToSectorsPerSecond(int speed, int base) {
    if (speed <= 0 || base <= 0) return 0;
    long long v = (long long)speed * base;
    if (v > 0x7FFFFFFFLL) v = 0x7FFFFFFFLL;
    return (LONG)v;
}

/* ==========================================================================
 * Capability
 * ======================================================================== */
int Capability(int capability) {
    switch (capability) {
    case BURN_CAP_IMAGE_BURN:      return 1;
    case BURN_CAP_CREATE_IMAGE:    return 1;
    case BURN_CAP_AUDIO_TAO:       return 1;
    case BURN_CAP_ERASE:           return 1;
    case BURN_CAP_MULTISESSION:    return 1;
    case BURN_CAP_UDF:             return 1;
    case BURN_CAP_BOOT_IMAGE:      return 1;
    case BURN_CAP_READBACK_VERIFY: return 1;
    case BURN_CAP_RAW_BURN:        return 1;
    case BURN_CAP_RAW_IMAGE:       return 1;
    case BURN_CAP_TRAY_CONTROL:    return 1;
    case BURN_CAP_DRIVE_INFO:      return 1;
    default:                       return 0;
    }
}

/* ==========================================================================
 * Multisession inquiry
 * ======================================================================== */
long long NextSessionStartBlock(const std::string& devicePath) {
    Bstr id;
    if (!FindRecorderId(devicePath, id)) return -1;
    Com<IDiscRecorder2> recorder = OpenRecorder(id);
    if (!recorder) return -1;

    Com<IDiscFormat2Data> data;
    HRESULT hr = CoCreateInstance(CLSID_MsftDiscFormat2Data, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IDiscFormat2Data, (void**)&data);
    if (FAILED(hr) || !data) {
        SetBurnLastError(HrMsg(hr, "CoCreateInstance(IDiscFormat2Data)"));
        return -1;
    }
    data->put_Recorder(recorder.get());
    SetClientName(data.get());

    IMAPI_FORMAT2_DATA_MEDIA_STATE state = IMAPI_FORMAT2_DATA_MEDIA_STATE_UNKNOWN;
    hr = data->get_CurrentMediaStatus(&state);
    if (FAILED(hr)) {
        SetBurnLastError("Could not read the media state (" + HrHex(hr) +
                         "). Is a disc loaded in " + devicePath + "?");
        return -1;
    }
    if (state & IMAPI_FORMAT2_DATA_MEDIA_STATE_UNSUPPORTED_MEDIA) {
        SetBurnLastError("Media in " + devicePath + " is unsupported by IMAPI2");
        return -1;
    }
    if (state & IMAPI_FORMAT2_DATA_MEDIA_STATE_WRITE_PROTECTED) {
        SetBurnLastError("Media in " + devicePath + " is write protected; "
                         "no session can be appended");
        return -1;
    }
    if (state & IMAPI_FORMAT2_DATA_MEDIA_STATE_FINALIZED) {
        SetBurnLastError("Disc in " + devicePath + " is finalized: no further "
                         "session can be added");
        return -1;
    }
    if ((state & IMAPI_FORMAT2_DATA_MEDIA_STATE_BLANK) &&
        !(state & IMAPI_FORMAT2_DATA_MEDIA_STATE_APPENDABLE)) {
        return 0;   /* blank: the new session is the first one */
    }
    if (!(state & IMAPI_FORMAT2_DATA_MEDIA_STATE_APPENDABLE)) {
        SetBurnLastError("Disc in " + devicePath + " is neither blank nor appendable "
                         "(media state 0x" + HrHex((HRESULT)(unsigned)state) +
                         "); a session cannot be appended");
        return -1;
    }

    /* Preferred source: the drive's own sequential multisession object. */
    SafeArr array;
    if (SUCCEEDED(data->get_MultisessionInterfaces(array.out())) && array.get()) {
        const LONG lb = (LONG)array.lower(), ub = (LONG)array.upper();
        for (LONG k = lb; k <= ub; ++k) {
            IDispatch* d = nullptr;
            if (!ReadDispatchElement(array.get(), k, &d) || !d) continue;
            IMultisessionSequential* ms = nullptr;
            if (SUCCEEDED(d->QueryInterface(IID_IMultisessionSequential, (void**)&ms)) && ms) {
                VARIANT_BOOL supported = VARIANT_FALSE;
                ms->get_IsSupportedOnCurrentMediaState(&supported);
                if (supported == VARIANT_TRUE) {
                    LONG nwa = 0;
                    const HRESULT nh = ms->get_NextWritableAddress(&nwa);
                    ms->Release();
                    d->Release();
                    if (FAILED(nh)) {
                        SetBurnLastError(HrMsg(nh, "IMultisessionSequential::get_NextWritableAddress"));
                        return -1;
                    }
                    return nwa >= 0 ? (long long)nwa : -1;
                }
                ms->Release();
            }
            d->Release();
        }
    }

    LONG addr = 0;
    hr = data->get_NextWritableAddress(&addr);
    if (FAILED(hr)) {
        SetBurnLastError(HrMsg(hr, "IDiscFormat2Data::get_NextWritableAddress"));
        return -1;
    }
    return addr >= 0 ? (long long)addr : -1;
}

/* ==========================================================================
 * Stream -> file, with honest truncation handling
 * ======================================================================== */
static bool WriteStreamToFile(IStream* stream, const std::string& path,
                              const std::string& what) {
    if (!stream) return false;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        SetBurnLastError("Cannot open output file: " + path);
        return false;
    }
    std::vector<unsigned char> buf(65536);
    bool cancelled = false, writeError = false, streamError = false;
    for (;;) {
        if (IsBurnCancelled()) { cancelled = true; break; }
        ULONG got = 0;
        const HRESULT hr = stream->Read(buf.data(), (ULONG)buf.size(), &got);
        if (FAILED(hr)) { streamError = true; break; }
        if (got == 0) break;
        if (fwrite(buf.data(), 1, got, f) != got) { writeError = true; break; }
        if (got < buf.size()) break;   /* short read means end of stream */
    }
    const int closeErr = (fclose(f) != 0);  /* buffered tail fails here */

    if (cancelled) {
        remove(path.c_str());
        SetBurnLastError(what + " cancelled by caller");
        return false;
    }
    if (writeError || closeErr) {
        remove(path.c_str());
        SetBurnLastError(what + " failed while writing " + path +
                         " (disk full or I/O error)");
        return false;
    }
    if (streamError) {
        remove(path.c_str());
        SetBurnLastError(what + " failed while reading the image stream");
        return false;
    }
    return true;
}

/* ==========================================================================
 * Image creation: file systems, ISO level, UDF, El Torito, continuation
 * ======================================================================== */
static FsiFileSystems MapFileSystems(int mask) {
    if (mask == 0) mask = BURN_FS_ISO9660 | BURN_FS_JOLIET;
    unsigned fs = 0;
    if (mask & BURN_FS_ISO9660) fs |= (unsigned)FsiFileSystemISO9660;
    if (mask & BURN_FS_JOLIET) fs |= (unsigned)FsiFileSystemJoliet;
    if (mask & BURN_FS_UDF) fs |= (unsigned)FsiFileSystemUDF;
    if (fs == 0) fs = (unsigned)FsiFileSystemISO9660;
    return (FsiFileSystems)fs;
}

static bool ApplyBootImage(IFileSystemImage* image, const std::string& clipPath,
                           int emulation, int platform) {
    Com<IBootOptions> boot;
    HRESULT hr = CoCreateInstance(CLSID_BootOptions, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IBootOptions, (void**)&boot);
    if (FAILED(hr) || !boot) {
        SetBurnLastError(HrMsg(hr, "CoCreateInstance(IBootOptions)"));
        return false;
    }
    Com<IStream> clip = OpenFileStream(clipPath);
    if (!clip) return false;

    hr = boot->AssignBootImage(clip.get());
    if (FAILED(hr)) {
        SetBurnLastError(HrMsg(hr, "IBootOptions::AssignBootImage") + " for " + clipPath +
                         ". The load-clip must be a valid floppy or hard-disk image.");
        return false;
    }

    EmulationType emu = EmulationNone;
    switch (emulation) {
    case BURN_BOOT_EMU_FLOPPY_12:  emu = Emulation12MFloppy; break;
    case BURN_BOOT_EMU_FLOPPY_144: emu = Emulation144MFloppy; break;
    case BURN_BOOT_EMU_FLOPPY_288: emu = Emulation288MFloppy; break;
    case BURN_BOOT_EMU_HARDDISK:   emu = EmulationHardDisk; break;
    default: emu = EmulationNone; break;
    }
    boot->put_Emulation(emu);

    PlatformId pid = PlatformX86;
    switch (platform) {
    case BURN_BOOT_PLATFORM_PPC: pid = PlatformPowerPC; break;
    case BURN_BOOT_PLATFORM_MAC: pid = PlatformMac; break;
    case BURN_BOOT_PLATFORM_EFI: pid = PlatformEFI; break;
    default: pid = PlatformX86; break;
    }
    boot->put_PlatformId(pid);

    hr = image->put_BootImageOptions(boot.get());
    if (FAILED(hr)) {
        SetBurnLastError(HrMsg(hr, "IFileSystemImage::put_BootImageOptions"));
        return false;
    }
    return true;
}

/* Attach the drive's sequential multisession interface to the image so the
 * created filesystem is a continuation session that keeps the previous one
 * readable. The interfaces come from the writer, which is why the recorder
 * has to be attached to it first. */
static bool EnableMultisession(IFileSystemImage* image, IDiscFormat2Data* writer,
                               long long sessionStartBlock) {
    SafeArr array;
    HRESULT hr = writer->get_MultisessionInterfaces(array.out());
    if (FAILED(hr) || !array.get()) {
        SetBurnLastError(HrMsg(hr, "IDiscFormat2Data::get_MultisessionInterfaces"));
        return false;
    }
    bool chosen = false;
    const LONG lb = (LONG)array.lower(), ub = (LONG)array.upper();
    for (LONG k = lb; k <= ub && !chosen; ++k) {
        IDispatch* d = nullptr;
        if (!ReadDispatchElement(array.get(), k, &d) || !d) continue;
        IMultisessionSequential* ms = nullptr;
        if (SUCCEEDED(d->QueryInterface(IID_IMultisessionSequential, (void**)&ms)) && ms) {
            VARIANT_BOOL supported = VARIANT_FALSE;
            ms->get_IsSupportedOnCurrentMediaState(&supported);
            if (supported == VARIANT_TRUE) {
                hr = ms->put_InUse(VARIANT_TRUE);
                if (FAILED(hr)) {
                    SetBurnLastError(HrMsg(hr, "IMultisessionSequential::put_InUse"));
                } else {
                    chosen = true;
                }
            }
            ms->Release();
        }
        d->Release();
    }
    if (!chosen) {
        SetBurnLastError("This drive/media combination exposes no usable sequential "
                         "multisession interface, so a session cannot be appended");
        return false;
    }

    hr = image->put_MultisessionInterfaces(array.get());
    if (FAILED(hr)) {
        SetBurnLastError(HrMsg(hr, "IFileSystemImage::put_MultisessionInterfaces"));
        return false;
    }

    hr = image->put_SessionStartBlock((LONG)sessionStartBlock);
    if (FAILED(hr)) {
        SetBurnLastError(HrMsg(hr, "IFileSystemImage::put_SessionStartBlock") +
                         " (block " + std::to_string(sessionStartBlock) + ")");
        return false;
    }
    return true;
}

bool CreateImage(const BurnImageSpec& spec) {
    if (IsBurnCancelled()) {
        SetBurnLastError("ISO creation cancelled by caller");
        return false;
    }
    NotifyProgressBurn(10, "Creating ISO image...");

    Com<IFileSystemImage> image;
    HRESULT hr = CoCreateInstance(CLSID_MsftFileSystemImage, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IFileSystemImage, (void**)&image);
    if (FAILED(hr) || !image) {
        SetBurnLastError("Failed to create IFileSystemImage (" + HrHex(hr) +
                         "). imapi2fs.dll must be registered and the temp directory writable.");
        return false;
    }

    Bstr label(spec.volumeLabel.empty() ? std::string("DISC") : spec.volumeLabel);
    image->put_VolumeName(label);

    hr = image->put_FileSystemsToCreate(MapFileSystems(spec.fileSystems));
    if (FAILED(hr)) {
        SetBurnLastError(HrMsg(hr, "put_FileSystemsToCreate") + " (mask " +
                         std::to_string(spec.fileSystems) + ")");
        return false;
    }

    if (spec.interchangeLevel >= 1 && spec.interchangeLevel <= 3) {
        hr = image->put_ISO9660InterchangeLevel((LONG)spec.interchangeLevel);
        if (FAILED(hr)) {
            SetBurnLastError(HrMsg(hr, "put_ISO9660InterchangeLevel") + " (level " +
                             std::to_string(spec.interchangeLevel) + ")");
            return false;
        }
    }

    if (spec.udfRevision != 0) {
        hr = image->put_UDFRevision((LONG)spec.udfRevision);
        if (FAILED(hr)) {
            std::string known;
            SafeArr revs;
            if (SUCCEEDED(image->get_UDFRevisionsSupported(revs.out())) && revs.get()) {
                for (LONG k = (LONG)revs.lower(); k <= (LONG)revs.upper(); ++k) {
                    LONG v = 0;
                    if (ReadLongElement(revs.get(), k, v)) {
                        char b[16];
                        snprintf(b, sizeof(b), "%s0x%04lX", known.empty() ? "" : " ",
                                 (unsigned long)v);
                        known += b;
                    }
                }
            }
            SetBurnLastError(HrMsg(hr, "put_UDFRevision") +
                             (known.empty() ? "" : ". Supported revisions: " + known));
            return false;
        }
    }

    /* A continuation session needs a live writer on the device. */
    if (spec.sessionStartBlock != 0) {
        if (spec.devicePath.empty()) {
            SetBurnLastError("A continuation image requires devicePath so the existing "
                             "session can be imported");
            return false;
        }
        Bstr id;
        if (!FindRecorderId(spec.devicePath, id)) return false;
        Com<IDiscRecorder2> recorder = OpenRecorder(id);
        if (!recorder) return false;
        Com<IDiscFormat2Data> writer;
        hr = CoCreateInstance(CLSID_MsftDiscFormat2Data, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IDiscFormat2Data, (void**)&writer);
        if (FAILED(hr) || !writer) {
            SetBurnLastError(HrMsg(hr, "CoCreateInstance(IDiscFormat2Data)"));
            return false;
        }
        writer->put_Recorder(recorder.get());
        SetClientName(writer.get());
        if (!EnableMultisession(image.get(), writer.get(),
                                (long long)spec.sessionStartBlock)) {
            return false;
        }
    }

    NotifyProgressBurn(30, "Importing source...");
    IFsiDirectoryItem* root = nullptr;
    hr = image->get_Root(&root);
    if (FAILED(hr) || !root) {
        SetBurnLastError("Failed to get the root directory of the ISO image");
        return false;
    }
    Bstr source(spec.sourcePath);
    hr = root->AddTree(source, VARIANT_FALSE);
    root->Release();
    if (FAILED(hr)) {
        SetBurnLastError("Failed to add source to the ISO image (" + HrHex(hr) + "): " +
                         spec.sourcePath);
        return false;
    }

    if (!spec.bootImagePath.empty()) {
        NotifyProgressBurn(40, "Adding El Torito boot catalog...");
        if (!ApplyBootImage(image.get(), spec.bootImagePath, spec.bootEmulation,
                            spec.bootPlatform)) {
            return false;
        }
    }

    NotifyProgressBurn(50, "Writing ISO file...");
    Com<IFileSystemImageResult> result;
    hr = image->CreateResultImage(&result);
    if (FAILED(hr) || !result) {
        SetBurnLastError("Failed to create the result image (" + HrHex(hr) +
                         "). The source may be too large for the requested file "
                         "systems or ISO level.");
        return false;
    }
    Com<IStream> stream;
    hr = result->get_ImageStream(&stream);
    if (FAILED(hr) || !stream) {
        SetBurnLastError("The ISO result image exposes no stream");
        return false;
    }
    if (!WriteStreamToFile(stream.get(), spec.outputPath, "ISO creation")) return false;

    NotifyProgressBurn(100, "ISO creation completed");
    return true;
}

/* ==========================================================================
 * Image writing with the v3 options
 * ======================================================================== */
/* Defined in the drive-information section below; needed by the write
 * preflight that lives earlier in this file. */
static std::string MediaTypeName(IMAPI_MEDIA_PHYSICAL_TYPE mt);
static bool IsTransientMediaHr(HRESULT hr);

/* IMAPI2 reports most "you cannot burn to this" conditions only from
 * Write(), with an HRESULT and no explanation. Translate the ones we can
 * into an instruction the caller can act on, keeping the original code. */
static std::string WriteFailureHint(HRESULT hr) {
    switch (hr) {
    case E_IMAPI_DF2DATA_MEDIA_IS_NOT_SUPPORTED:
        return " No writable disc is present or the inserted medium is not "
               "supported by this recorder. Insert a blank CD-R/DVD-R or a "
               "rewritable disc; check Burn_GetMediaState() before starting.";
    case E_IMAPI_DF2DATA_MEDIA_NOT_BLANK:
        return " The disc is not blank. ForceOverwrite is only enabled for "
               "randomly writable media (DVD-RAM/DVD+RW); erase a rewritable "
               "disc or use a blank one.";
    case E_IMAPI_RECORDER_MEDIA_NO_MEDIA:
        return " There is no media in the device; insert a blank disc.";
    case E_IMAPI_RECORDER_MEDIA_INCOMPATIBLE:
        return " The media is incompatible or of unknown physical format; "
               "insert a blank CD-R/DVD-R.";
    case E_IMAPI_RECORDER_MEDIA_UPSIDE_DOWN:
        return " The disc is inserted upside down; flip it and retry.";
    case E_IMAPI_DF2DATA_RECORDER_NOT_SUPPORTED:
        return " This recorder does not support the requested data format.";
    default:
        return "";
    }
}

bool BurnImageFile(const std::string& imagePath, const std::string& devicePath,
                   const std::string& discType, int speed, int verifyLevel,
                   bool finalize, unsigned long long sessionStartBlock) {
    NotifyProgressBurn(10, "Initializing burn process...");
    if (IsBurnCancelled()) {
        SetBurnLastError("Burn cancelled by caller");
        return false;
    }

    Bstr id;
    if (!FindRecorderId(devicePath, id)) return false;
    Com<IDiscRecorder2> recorder = OpenRecorder(id);
    if (!recorder) return false;

    NotifyProgressBurn(30, "Preparing disc...");
    Com<IDiscFormat2Data> writer;
    HRESULT hr = CoCreateInstance(CLSID_MsftDiscFormat2Data, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IDiscFormat2Data, (void**)&writer);
    if (FAILED(hr) || !writer) {
        SetBurnLastError(HrMsg(hr, "CoCreateInstance(IDiscFormat2Data)"));
        return false;
    }
    writer->put_Recorder(recorder.get());
    SetClientName(writer.get());

    /* Fail fast when the tray is empty or the inserted medium is unusable.
     * Without this check IMAPI2 spends a second "preparing" and then returns
     * 0xC0AA0406 with no hint about what to do. Only refuse on an explicit
     * VARIANT_FALSE: if the query itself fails, let Write() decide so a
     * quirky recorder is never blocked by a diagnostic call. */
    IMAPI_MEDIA_PHYSICAL_TYPE mt = IMAPI_MEDIA_TYPE_UNKNOWN;
    HRESULT mediaHr = writer->get_CurrentPhysicalMediaType(&mt);
    for (int attempt = 0; attempt < 3 && FAILED(mediaHr) &&
         IsTransientMediaHr(mediaHr); ++attempt) {
        Sleep(500);
        mediaHr = writer->get_CurrentPhysicalMediaType(&mt);
    }
    VARIANT_BOOL mediaOk = VARIANT_FALSE;
    const HRESULT mediaCheck =
        writer->IsCurrentMediaSupported(recorder.get(), &mediaOk);
    if (SUCCEEDED(mediaCheck) && mediaOk != VARIANT_TRUE) {
        SetBurnLastError("No writable disc detected in " + devicePath +
                         "; insert a blank CD-R/DVD-R or a rewritable disc. "
                         "IMAPI2 IsCurrentMediaSupported returned false; " +
                         "physical media query " + HrHex(mediaHr) +
                         " (" + MediaTypeName(mt) + "). See Burn_GetMediaState().");
        return false;
    }

    /* Randomly writable media (DVD-RAM / DVD+RW) is never blank: IMAPI2
     * reports OVERWRITE_ONLY and Write() returns
     * E_IMAPI_DF2DATA_MEDIA_NOT_BLANK unless ForceOverwrite is enabled.
     * Only enable it for that media state, never for ordinary sequential
     * media, so a non-blank CD-R / DVD-R is still refused. */
    IMAPI_FORMAT2_DATA_MEDIA_STATE mediaState = IMAPI_FORMAT2_DATA_MEDIA_STATE_UNKNOWN;
    if (SUCCEEDED(writer->get_CurrentMediaStatus(&mediaState)) &&
        (mediaState & IMAPI_FORMAT2_DATA_MEDIA_STATE_OVERWRITE_ONLY)) {
        writer->put_ForceOverwrite(VARIANT_TRUE);
    }

    /* Default IMAPI2 behaviour already leaves a sequential disc appendable;
     * closing it is an explicit request. */
    writer->put_ForceMediaToBeClosed(finalize ? VARIANT_TRUE : VARIANT_FALSE);

    if (sessionStartBlock != 0) {
        IMAPI_FORMAT2_DATA_MEDIA_STATE state = IMAPI_FORMAT2_DATA_MEDIA_STATE_UNKNOWN;
        if (SUCCEEDED(writer->get_CurrentMediaStatus(&state)) &&
            !(state & (IMAPI_FORMAT2_DATA_MEDIA_STATE_APPENDABLE |
                       IMAPI_FORMAT2_DATA_MEDIA_STATE_BLANK))) {
            SetBurnLastError("A session-start block was requested but the disc in " +
                             devicePath + " is neither blank nor appendable");
            return false;
        }
    }

    const LONG sps = MultiplierToSectorsPerSecond(speed, BaseSectorsPerSecond(writer.get(), discType));
    if (sps > 0) {
        /* An unsupported exact value is clamped by the drive itself. */
        writer->SetWriteSpeed(sps, VARIANT_FALSE);
    }

    NotifyProgressBurn(50, "Burning in progress...");
    Com<IStream> stream = OpenFileStream(imagePath);
    if (!stream) return false;

    EventSubscription events;
    events.attach(writer.get(), IID_DDiscFormat2DataEvents, "Writing to disc");

    /* Drive-side verification while writing, when requested and available. */
    if (verifyLevel != BURN_VERIFY_NONE) {
        Com<IBurnVerification> verification;
        writer->QueryInterface(IID_IBurnVerification, (void**)&verification);
        if (verification) {
            verification->put_BurnVerificationLevel(
                verifyLevel == BURN_VERIFY_FULL ? IMAPI_BURN_VERIFICATION_FULL
                                               : IMAPI_BURN_VERIFICATION_QUICK);
        } else {
            SetBurnLastError("This recorder does not expose IBurnVerification; "
                             "verification is skipped. Use Burn_VerifyWrite() for a "
                             "byte-exact read-back.");
        }
    }

    CancelSlot slot;
    CancelArm arm(slot);
    arm.bind(writer.get(), WriterKind::Data);

    hr = writer->Write(stream.get());

    if (SUCCEEDED(hr)) {
        NotifyProgressBurn(100, "Burn completed");
        return true;
    }
    if (IsBurnCancelled()) {
        SetBurnLastError("Burn cancelled by caller");
        return false;
    }
    SetBurnLastError("Burn write failed (" + HrHex(hr) + ") for " + imagePath +
                     " on " + devicePath + "." + WriteFailureHint(hr));
    return false;
}

/* ==========================================================================
 * Audio CD, track-at-once
 * ======================================================================== */
bool BurnAudioTracks(const std::vector<BurnAudioTrackSpec>& tracks,
                     const std::string& devicePath, int speed,
                     int verifyLevel, bool finalizeDisc) {
    if (tracks.empty() || tracks.size() > 99) {
        SetBurnLastError("An audio CD holds between 1 and 99 tracks");
        return false;
    }
    if (verifyLevel != BURN_VERIFY_NONE) {
        SetBurnLastError("Verification is not available on the audio track-at-once "
                         "path; run Burn_VerifyWrite() afterwards if required");
    }

    NotifyProgressBurn(5, "Preparing audio session...");
    Bstr id;
    if (!FindRecorderId(devicePath, id)) return false;
    Com<IDiscRecorder2> recorder = OpenRecorder(id);
    if (!recorder) return false;

    Com<IDiscFormat2TrackAtOnce> tao;
    HRESULT hr = CoCreateInstance(CLSID_MsftDiscFormat2TrackAtOnce, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_IDiscFormat2TrackAtOnce,
                                  (void**)&tao);
    if (FAILED(hr) || !tao) {
        SetBurnLastError(HrMsg(hr, "CoCreateInstance(IDiscFormat2TrackAtOnce)"));
        return false;
    }
    tao->put_Recorder(recorder.get());
    SetClientName(tao.get());
    if (speed > 0) {
        tao->SetWriteSpeed(MultiplierToSectorsPerSecond(speed, 75), VARIANT_FALSE);
    }
    /* The Red Book table of contents is written by ReleaseMedia() unless the
     * caller wants the disc to stay appendable. */
    tao->put_DoNotFinalizeMedia(finalizeDisc ? VARIANT_FALSE : VARIANT_TRUE);

    hr = tao->PrepareMedia();
    if (FAILED(hr)) {
        std::string why;
        if (hr == (HRESULT)0xC0AA0502L) why = " - media was not prepared";
        else if (hr == (HRESULT)0xC0AA0202L) why = " - no disc in the drive";
        else if (hr == (HRESULT)0xC0AA0203L) why = " - unsupported media";
        else if (hr == (HRESULT)0xC0AA0209L) why = " - media is write protected";
        SetBurnLastError("IDiscFormat2TrackAtOnce::PrepareMedia failed (" + HrHex(hr) + ")" +
                         why + ". Blank CD-R or CD-RW media is required.");
        return false;
    }

    EventSubscription events;
    events.attach(tao.get(), IID_DDiscFormat2TrackAtOnceEvents, "Writing audio track");

    CancelSlot slot;
    CancelArm arm(slot);
    arm.bind(tao.get(), WriterKind::TrackAtOnce);

    const int trackCount = (int)tracks.size();
    bool failed = false;
    for (int i = 0; i < trackCount && !failed; ++i) {
        if (IsBurnCancelled()) { failed = true; break; }
        char msg[128];
        snprintf(msg, sizeof(msg), "Writing audio track %d of %d", i + 1, trackCount);
        NotifyProgressBurn((i * 100) / (trackCount + 1), msg);

        Com<IStream> stream = AudioTrackStream(tracks[(size_t)i].audioPath);
        if (!stream) { failed = true; break; }

        hr = tao->AddAudioTrack(stream.get());
        if (FAILED(hr)) {
            std::string why;
            if (hr == (HRESULT)0xC0AA050DL) why = " - the stream is not 44.1kHz/16-bit/stereo PCM";
            else if (hr == (HRESULT)0xC0AA0508L) why = " - the disc already holds 99 tracks";
            else if (hr == (HRESULT)0xC0AA0509L) why = " - no space left on the media";
            SetBurnLastError("AddAudioTrack for track " + std::to_string(i + 1) + " (" +
                             tracks[(size_t)i].audioPath + ") failed (" + HrHex(hr) + ")" + why);
            failed = true;
            break;
        }
    }

    if (!failed && !IsBurnCancelled()) {
        /* Commits the table of contents. Nothing is durably recorded before
         * this call returns. */
        hr = tao->ReleaseMedia();
        if (FAILED(hr)) {
            SetBurnLastError("IDiscFormat2TrackAtOnce::ReleaseMedia failed (" + HrHex(hr) +
                             "). The session may be incomplete: do not reuse this disc.");
            failed = true;
        }
    } else {
        tao->CancelAddTrack();
        SetBurnLastError("Audio session cancelled. Track-at-once cannot be aborted "
                         "cleanly once a track started writing: a CD-R may be lost, "
                         "a CD-RW can be recovered with BurnEraseDisc.");
    }

    if (failed) return false;
    NotifyProgressBurn(100, "Audio session completed");
    return true;
}

/* ==========================================================================
 * Erase / format
 * ======================================================================== */
static std::string MediaTypeName(IMAPI_MEDIA_PHYSICAL_TYPE mt) {
    switch (mt) {
    case IMAPI_MEDIA_TYPE_CDROM: return "CD-ROM";
    case IMAPI_MEDIA_TYPE_CDR: return "CD-R";
    case IMAPI_MEDIA_TYPE_CDRW: return "CD-RW";
    case IMAPI_MEDIA_TYPE_DVDROM: return "DVD-ROM";
    case IMAPI_MEDIA_TYPE_DVDRAM: return "DVD-RAM";
    case IMAPI_MEDIA_TYPE_DVDPLUSR: return "DVD+R";
    case IMAPI_MEDIA_TYPE_DVDPLUSRW: return "DVD+RW";
    case IMAPI_MEDIA_TYPE_DVDPLUSR_DUALLAYER: return "DVD+R DL";
    case IMAPI_MEDIA_TYPE_DVDDASHR: return "DVD-R";
    case IMAPI_MEDIA_TYPE_DVDDASHRW: return "DVD-RW";
    case IMAPI_MEDIA_TYPE_DVDDASHR_DUALLAYER: return "DVD-R DL";
    case IMAPI_MEDIA_TYPE_DVDPLUSRW_DUALLAYER: return "DVD+RW DL";
    case IMAPI_MEDIA_TYPE_BDROM: return "BD-ROM";
    case IMAPI_MEDIA_TYPE_BDR: return "BD-R";
    case IMAPI_MEDIA_TYPE_BDRE: return "BD-RE";
    case IMAPI_MEDIA_TYPE_HDDVDROM: return "HD DVD-ROM";
    case IMAPI_MEDIA_TYPE_HDDVDR: return "HD DVD-R";
    case IMAPI_MEDIA_TYPE_HDDVDRAM: return "HD DVD-RAM";
    case IMAPI_MEDIA_TYPE_DISK: return "CD-RW (packet)";
    case IMAPI_MEDIA_TYPE_UNKNOWN: return "no media or unknown";
    default: return "media type " + std::to_string((int)mt);
    }
}

bool EraseDisc(const std::string& devicePath, bool full) {
    NotifyProgressBurn(5, "Preparing erase...");
    if (IsBurnCancelled()) {
        SetBurnLastError("Erase cancelled by caller");
        return false;
    }
    Bstr id;
    if (!FindRecorderId(devicePath, id)) return false;
    Com<IDiscRecorder2> recorder = OpenRecorder(id);
    if (!recorder) return false;

    Com<IDiscFormat2Erase> erase;
    HRESULT hr = CoCreateInstance(CLSID_MsftDiscFormat2Erase, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IDiscFormat2Erase, (void**)&erase);
    if (FAILED(hr) || !erase) {
        SetBurnLastError(HrMsg(hr, "CoCreateInstance(IDiscFormat2Erase)"));
        return false;
    }
    erase->put_Recorder(recorder.get());
    SetClientName(erase.get());
    erase->put_FullErase(full ? VARIANT_TRUE : VARIANT_FALSE);

    IMAPI_MEDIA_PHYSICAL_TYPE mt = IMAPI_MEDIA_TYPE_UNKNOWN;
    erase->get_CurrentPhysicalMediaType(&mt);
    const std::string media = MediaTypeName(mt);

    NotifyProgressBurn(20, ("Erasing " + media + "...").c_str());
    /* EraseMedia() is not cancellable: IMAPI2 hands the drive one command and
     * blocks until it finishes (fast blank: seconds, full erase: minutes). */
    hr = erase->EraseMedia();
    if (FAILED(hr)) {
        std::string why;
        if (hr == (HRESULT)0xC0AA0202L) why = " - no disc in the drive";
        else if (hr == (HRESULT)0xC0AA0203L) why = " - the media is not rewritable";
        else if (hr == (HRESULT)0xC0AA0209L) why = " - the media is write protected";
        SetBurnLastError("EraseMedia on " + devicePath + " (" + media + ") failed (" +
                         HrHex(hr) + ")" + why +
                         ". Only CD-RW, DVD-RW, DVD+RW, DVD-RAM and BD-RE can be erased.");
        return false;
    }
    NotifyProgressBurn(100, "Erase completed");
    return true;
}

/* ==========================================================================
 * RAW PQ image building (drive-free) and RAW writing
 * ======================================================================== */
bool CreateRawImage(const std::vector<BurnAudioTrackSpec>& tracks,
                    const std::string& outputPath, const std::string& mcn,
                    int startingTrackNumber) {
    if (tracks.empty() || tracks.size() > 99) {
        SetBurnLastError("A RAW CD image holds between 1 and 99 tracks");
        return false;
    }
    NotifyProgressBurn(5, "Building RAW PQ image...");

    Com<IRawCDImageCreator> creator;
    HRESULT hr = CoCreateInstance(CLSID_MsftRawCDImageCreator, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_IRawCDImageCreator,
                                  (void**)&creator);
    if (FAILED(hr) || !creator) {
        SetBurnLastError(HrMsg(hr, "CoCreateInstance(IRawCDImageCreator)"));
        return false;
    }

    /* PQ-only keeps the image at exactly 2352 bytes per sector, which is the
     * layout of a .bin/.cue RAW image. */
    hr = creator->put_ResultingImageType(IMAPI_FORMAT2_RAW_CD_SUBCODE_PQ_ONLY);
    if (FAILED(hr)) {
        SetBurnLastError(HrMsg(hr, "put_ResultingImageType"));
        return false;
    }

    if (startingTrackNumber >= 1 && startingTrackNumber <= 99) {
        hr = creator->put_StartingTrackNumber((LONG)startingTrackNumber);
        if (FAILED(hr)) {
            SetBurnLastError(HrMsg(hr, "put_StartingTrackNumber") + " (value " +
                             std::to_string(startingTrackNumber) + ", valid 1..99)");
            return false;
        }
    }
    if (!mcn.empty()) {
        Bstr catalog(mcn);
        hr = creator->put_MediaCatalogNumber(catalog);
        if (FAILED(hr)) {
            SetBurnLastError("put_MediaCatalogNumber failed (" + HrHex(hr) +
                             "). A media catalog number is 13 digits.");
            return false;
        }
    }

    const int trackCount = (int)tracks.size();
    for (int i = 0; i < trackCount; ++i) {
        if (IsBurnCancelled()) {
            SetBurnLastError("RAW image creation cancelled by caller");
            return false;
        }
        Com<IStream> stream = AudioTrackStream(tracks[(size_t)i].audioPath);
        if (!stream) return false;

        LONG trackIndex = 0;
        hr = creator->AddTrack(IMAPI_CD_SECTOR_AUDIO, stream.get(), &trackIndex);
        if (FAILED(hr)) {
            SetBurnLastError("IRawCDImageCreator::AddTrack failed for track " +
                             std::to_string(i + 1) + " (" + tracks[(size_t)i].audioPath +
                             ") " + HrHex(hr));
            return false;
        }

        if (!tracks[(size_t)i].isrc.empty() || tracks[(size_t)i].preemphasis) {
            Com<IRawCDImageTrackInfo> info;
            LONG index = trackIndex;
        if (SUCCEEDED(creator->get_TrackInfo(index, &info)) && info) {
                if (!tracks[(size_t)i].isrc.empty()) {
                    Bstr isrc(tracks[(size_t)i].isrc);
                    const HRESULT ih = info->put_ISRC(isrc);
                    if (FAILED(ih)) {
                        SetBurnLastError("put_ISRC failed (" + HrHex(ih) +
                                         "). An ISRC is 12 characters.");
                        return false;
                    }
                }
                if (tracks[(size_t)i].preemphasis) {
                    info->put_AudioHasPreemphasis(VARIANT_TRUE);
                }
            }
        }

        char msg[128];
        snprintf(msg, sizeof(msg), "Added track %d of %d", i + 1, trackCount);
        NotifyProgressBurn(5 + (85 * (i + 1)) / trackCount, msg);
    }

    NotifyProgressBurn(92, "Writing RAW image file...");
    Com<IStream> image;
    hr = creator->CreateResultImage(&image);
    if (FAILED(hr) || !image) {
        SetBurnLastError("IRawCDImageCreator::CreateResultImage failed (" + HrHex(hr) + ")");
        return false;
    }
    if (!WriteStreamToFile(image.get(), outputPath, "RAW image creation")) return false;

    NotifyProgressBurn(100, "RAW image creation completed");
    return true;
}

bool BurnRawImage(const std::string& imagePath, const std::string& devicePath,
                  int speed, int verifyLevel) {
    if (verifyLevel != BURN_VERIFY_NONE) {
        SetBurnLastError("Verification is not available on the RAW write path; "
                         "use Burn_VerifyWrite() after the burn");
    }
    NotifyProgressBurn(5, "Preparing RAW write...");

    Bstr id;
    if (!FindRecorderId(devicePath, id)) return false;
    Com<IDiscRecorder2> recorder = OpenRecorder(id);
    if (!recorder) return false;

    Com<IDiscFormat2RawCD> raw;
    HRESULT hr = CoCreateInstance(CLSID_MsftDiscFormat2RawCD, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IDiscFormat2RawCD, (void**)&raw);
    if (FAILED(hr) || !raw) {
        SetBurnLastError(HrMsg(hr, "CoCreateInstance(IDiscFormat2RawCD)"));
        return false;
    }
    raw->put_Recorder(recorder.get());
    SetClientName(raw.get());

    /* Ask for PQ-only subcode only when the drive really supports it: most
     * consumer writers do not, and PrepareMedia would then fail with an
     * opaque error after the caller wasted a blank disc. */
    SafeArr types;
    hr = raw->get_SupportedSectorTypes(types.out());
    if (FAILED(hr) || !types.get()) {
        SetBurnLastError(HrMsg(hr, "IDiscFormat2RawCD::get_SupportedSectorTypes"));
        return false;
    }
    bool pqSupported = false;
    for (LONG k = (LONG)types.lower(); k <= (LONG)types.upper(); ++k) {
        LONG v = 0;
        if (ReadLongElement(types.get(), k, v) &&
            v == (LONG)IMAPI_FORMAT2_RAW_CD_SUBCODE_PQ_ONLY) {
            pqSupported = true;
        }
    }
    if (!pqSupported) {
        SetBurnLastError("This recorder does not support RAW writes with PQ subcode "
                         "(IMAPI_FORMAT2_RAW_CD_SUBCODE_PQ_ONLY). Burn the image as a "
                         "plain data session instead.");
        return false;
    }
    hr = raw->put_RequestedSectorType(IMAPI_FORMAT2_RAW_CD_SUBCODE_PQ_ONLY);
    if (FAILED(hr)) {
        SetBurnLastError(HrMsg(hr, "put_RequestedSectorType"));
        return false;
    }

    hr = raw->PrepareMedia();
    if (FAILED(hr)) {
        SetBurnLastError("IDiscFormat2RawCD::PrepareMedia failed (" + HrHex(hr) +
                         "). Blank CD media and RAW-capable firmware are required.");
        return false;
    }

    if (speed > 0) {
        raw->SetWriteSpeed(MultiplierToSectorsPerSecond(speed, 75), VARIANT_FALSE);
    }

    NotifyProgressBurn(20, "Writing RAW data...");
    Com<IStream> stream = OpenFileStream(imagePath);
    if (!stream) {
        raw->ReleaseMedia();
        return false;
    }

    EventSubscription events;
    events.attach(raw.get(), IID_DDiscFormat2RawCDEvents, "Writing RAW data");

    CancelSlot slot;
    CancelArm arm(slot);
    arm.bind(raw.get(), WriterKind::RawCD);

    /* The image already carries its own lead-in, so ask the drive not to add
     * a second one: WriteMedia2 suppresses it for the lead-out path. */
    hr = raw->WriteMedia2(stream.get(), -2);

    if (SUCCEEDED(hr)) {
        const HRESULT rel = raw->ReleaseMedia();
        if (FAILED(rel)) {
            SetBurnLastError("RAW data written but ReleaseMedia failed (" + HrHex(rel) +
                             "). The session may not be closed.");
            return false;
        }
        NotifyProgressBurn(100, "Burn completed");
        return true;
    }
    raw->ReleaseMedia();
    if (IsBurnCancelled()) {
        SetBurnLastError("Burn cancelled by caller");
        return false;
    }
    SetBurnLastError("RAW write failed (" + HrHex(hr) + ") for " + imagePath);
    return false;
}

/* ==========================================================================
 * Byte-exact read-back verification.
 *
 * IMAPI2 has no disc-read API, so sectors are fetched with MMC READ(10)
 * through IDiscRecorder2Ex::SendCommandGetDataFromDevice and compared with
 * the reference image. The session base is located by matching the image's
 * own primary volume descriptor, which makes the comparison correct for a
 * first as well as an appended session.
 * ======================================================================== */
static const unsigned kSectorSize = 2048;

class DiscReader {
public:
    bool open(const std::string& devicePath) {
        if (!FindRecorderId(devicePath, id_)) return false;
        recorder_ = OpenRecorder(id_);
        if (!recorder_) return false;

        HRESULT hr = recorder_->QueryInterface(IID_IDiscRecorder2Ex, (void**)&ex_);
        if (FAILED(hr) || !ex_) {
            SetBurnLastError(HrMsg(hr, "QI(IDiscRecorder2Ex)") +
                             " - read-back verification needs MMC passthrough support");
            return false;
        }
        ULONG mask = 0;
        ex_->GetByteAlignmentMask(&mask);
        mask_ = mask;

        ULONG maxBytes = 0;
        if (FAILED(ex_->GetMaximumPageAlignedTransferSize(&maxBytes)) ||
            maxBytes < kSectorSize) {
            maxBytes = 65536;
        }
        sectorsPerCommand_ = maxBytes / kSectorSize;
        if (sectorsPerCommand_ > 32) sectorsPerCommand_ = 32;
        if (sectorsPerCommand_ < 1) sectorsPerCommand_ = 1;

        const size_t bytes = (size_t)sectorsPerCommand_ * kSectorSize;
        const size_t extra = mask ? (size_t)mask : 4096;
        raw_ = (unsigned char*)VirtualAlloc(nullptr, bytes + extra + 16,
                                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!raw_) {
            SetBurnLastError("Out of memory allocating the disc read buffer");
            return false;
        }
        size_t addr = (size_t)raw_;
        aligned_ = mask ? (unsigned char*)((addr + mask) & ~(size_t)mask) : raw_;
        alignedBytes_ = bytes;
        return true;
    }

    unsigned sectorsPerCommand() const { return sectorsPerCommand_; }

    bool read(unsigned long long lba, unsigned count, unsigned char* dst) {
        if (!ex_ || count == 0 || (size_t)count * kSectorSize > alignedBytes_) return false;
        unsigned char cdb[10] = {0};
        cdb[0] = 0x28;   /* MMC READ(10) */
        cdb[1] = (unsigned char)((lba >> 24) & 0xff);
        cdb[2] = (unsigned char)((lba >> 16) & 0xff);
        cdb[3] = (unsigned char)((lba >> 8) & 0xff);
        cdb[4] = (unsigned char)(lba & 0xff);
        cdb[7] = (unsigned char)((count >> 8) & 0xff);
        cdb[8] = (unsigned char)(count & 0xff);

        unsigned char sense[18] = {0};
        ULONG got = 0;
        const ULONG bytes = count * kSectorSize;
        const HRESULT hr = ex_->SendCommandGetDataFromDevice(
            cdb, 10, sense, 20000, aligned_, bytes, &got);
        if (FAILED(hr) || got != bytes) {
            char b[128];
            snprintf(b, sizeof(b),
                     "MMC READ(10) at LBA %llu x%u failed (HRESULT 0x%08lX, sense key 0x%02X)",
                     lba, count, (unsigned long)hr, (unsigned)(sense[2] & 0x0f));
            SetBurnLastError(b);
            return false;
        }
        memcpy(dst, aligned_, bytes);
        return true;
    }

private:
    Bstr id_;
    Com<IDiscRecorder2> recorder_;
    Com<IDiscRecorder2Ex> ex_;
    unsigned char* raw_ = nullptr;
    unsigned char* aligned_ = nullptr;
    size_t alignedBytes_ = 0;
    unsigned mask_ = 0;
    unsigned sectorsPerCommand_ = 0;
};

bool VerifyWrite(const std::string& devicePath, const std::string& expectedImagePath,
                 int level) {
    DiscReader reader;
    if (!reader.open(devicePath)) return false;

    FILE* img = fopen(expectedImagePath.c_str(), "rb");
    if (!img) {
        SetBurnLastError("VerifyWrite: cannot open the reference image: " + expectedImagePath);
        return false;
    }
    fseek(img, 0, SEEK_END);
    const long long imageSize = ftell(img);
    if (imageSize < (long long)(34 * kSectorSize)) {
        fclose(img);
        SetBurnLastError("VerifyWrite: the reference image is too small to contain a "
                         "primary volume descriptor: " + expectedImagePath);
        return false;
    }
    const unsigned long long totalSectors = (unsigned long long)imageSize / kSectorSize;

    /* The reference PVD: the key to finding our session on the disc. */
    std::vector<unsigned char> imagePvd(kSectorSize);
    fseek(img, 16 * (long)kSectorSize, SEEK_SET);
    if (fread(imagePvd.data(), 1, kSectorSize, img) != kSectorSize) {
        fclose(img);
        SetBurnLastError("VerifyWrite: cannot read the reference image's volume descriptor");
        return false;
    }
    if (imagePvd[0] != 1 || memcmp(imagePvd.data() + 1, "CD001", 5) != 0) {
        fclose(img);
        SetBurnLastError("VerifyWrite: the reference image has no primary volume "
                         "descriptor at its LBA 16 - is it an ISO 9660 image?");
        return false;
    }

    NotifyProgressBurn(5, "Locating the written session...");
    std::vector<unsigned char> probeSector(kSectorSize);
    unsigned long long baseLba = 0;
    bool baseFound = false;
    for (unsigned long long candidate = 16; candidate <= 16 + 512; ++candidate) {
        if (IsBurnCancelled()) {
            fclose(img);
            SetBurnLastError("Verification cancelled by caller");
            return false;
        }
        if (!reader.read(candidate, 1, probeSector.data())) continue;
        if (memcmp(probeSector.data(), imagePvd.data(), kSectorSize) == 0) {
            baseLba = candidate - 16;
            baseFound = true;
            break;
        }
    }
    if (!baseFound) {
        fclose(img);
        SetBurnLastError("VerifyWrite: no session on the disc in " + devicePath +
                         " matches the reference image (all volume descriptors differ). "
                         "Either the disc was not written from this image or the drive "
                         "could not read it.");
        return false;
    }

    /* QUICK: boundaries plus 64 evenly spaced samples. FULL: every sector. */
    std::vector<unsigned long long> targets;
    if (level == BURN_VERIFY_QUICK) {
        targets.push_back(0);
        targets.push_back(16);
        for (int i = 0; i < 64; ++i) {
            targets.push_back((unsigned long long)((double)totalSectors * i / 64.0));
        }
        if (totalSectors > 1) targets.push_back(totalSectors - 1);
    } else {
        targets.reserve((size_t)totalSectors);
        for (unsigned long long s = 0; s < totalSectors; ++s) targets.push_back(s);
    }

    std::vector<unsigned char> discSector(kSectorSize);
    std::vector<unsigned char> imageSector(kSectorSize);
    unsigned long long compared = 0, mismatched = 0, firstMismatch = 0;
    bool cancelled = false, ioFailed = false;
    for (size_t i = 0; i < targets.size(); ++i) {
        if (IsBurnCancelled()) { cancelled = true; break; }
        const unsigned long long sector = targets[i];
        if (sector >= totalSectors) continue;

        fseek(img, (long)(sector * kSectorSize), SEEK_SET);
        if (fread(imageSector.data(), 1, kSectorSize, img) != kSectorSize) {
            SetBurnLastError("VerifyWrite: short read from the reference image at sector " +
                             std::to_string(sector));
            ioFailed = true;
            break;
        }
        if (!reader.read(baseLba + sector, 1, discSector.data())) {
            ioFailed = true;   /* last-error already explains the failing LBA */
            break;
        }
        if (memcmp(discSector.data(), imageSector.data(), kSectorSize) != 0) {
            if (mismatched == 0) firstMismatch = sector;
            ++mismatched;
            if (level == BURN_VERIFY_QUICK) break;   /* one is enough to fail */
        }
        ++compared;
        if ((i % 16) == 0 || i + 1 == targets.size()) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Compared %llu of %zu sectors",
                     (unsigned long long)compared, targets.size());
            NotifyProgressBurn(5 + (int)((90 * (i + 1)) / targets.size()), msg);
        }
    }
    fclose(img);

    if (cancelled) {
        SetBurnLastError("Verification cancelled by caller");
        return false;
    }
    if (ioFailed) return false;

    if (mismatched) {
        SetBurnLastError("Verification FAILED: " + std::to_string(mismatched) + " of " +
                         std::to_string(compared) + " sectors differ, first at image "
                         "sector " + std::to_string(firstMismatch) + " (disc LBA " +
                         std::to_string(baseLba + firstMismatch) + ")");
        return false;
    }
    char ok[128];
    snprintf(ok, sizeof(ok), "Verification passed: %llu sectors compared byte-exact",
             (unsigned long long)compared);
    SetBurnLastError(ok);
    NotifyProgressBurn(100, "Verification completed");
    return true;
}

/* ==========================================================================
 * Tray control
 * ======================================================================== */
static int TrayCommand(const char* name, const std::string& devicePath, bool eject) {
    Bstr id;
    if (!FindRecorderId(devicePath, id)) return BURN_ERROR_DEVICE_NOT_FOUND;
    Com<IDiscRecorder2> recorder = OpenRecorder(id);
    if (!recorder) return BURN_ERROR_DEVICE_NOT_FOUND;

    const HRESULT hr = eject ? recorder->EjectMedia() : recorder->CloseTray();
    if (FAILED(hr)) {
        std::string why;
        if (hr == (HRESULT)0xC0AA040EL) why = " - the tray is already closed";
        else if (hr == (HRESULT)0xC0AA0210L) why = " - the device is locked by another process";
        SetBurnLastError(std::string(name) + " failed on " + devicePath + " (" +
                         HrHex(hr) + ")" + why);
        return BURN_ERROR_BURN_FAILED;
    }
    return BURN_SUCCESS;
}

int EjectTray(const std::string& devicePath) { return TrayCommand("EjectMedia", devicePath, true); }
int CloseTray(const std::string& devicePath) { return TrayCommand("CloseTray", devicePath, false); }

/* ==========================================================================
 * MMC drive + media report
 * ======================================================================== */
static std::string ProfileName(LONG p) {
    switch (p) {
    case 0x00: return "none";
    case 0x08: return "CD-ROM";
    case 0x09: return "CD-R";
    case 0x0A: return "CD-RW";
    case 0x10: return "DVD-ROM";
    case 0x11: return "DVD-R/seq";
    case 0x12: return "DVD-RAM";
    case 0x13: return "DVD-RW/restricted";
    case 0x14: return "DVD-RW/seq";
    case 0x15: return "DVD-R DL/seq";
    case 0x1A: return "DVD+RW";
    case 0x1B: return "DVD+R";
    case 0x2A: return "DVD+RW DL";
    case 0x2B: return "DVD+R DL";
    case 0x40: return "BD-ROM";
    case 0x41: return "BD-R/seq";
    case 0x42: return "BD-R/random";
    case 0x43: return "BD-RE";
    case 0x4E: return "DVD+R RWA";
    default: {
        char b[32];
        snprintf(b, sizeof(b), "0x%02lX", (unsigned long)p);
        return b;
    }
    }
}

static void AppendProfileArray(std::string& out, SAFEARRAY* arr, const std::string& label) {
    if (!arr) return;
    LONG lb = 0, ub = -1;
    SafeArrayGetLBound(arr, 1, &lb);
    SafeArrayGetUBound(arr, 1, &ub);
    out += label + ":";
    for (LONG k = lb; k <= ub; ++k) {
        LONG v = -1;
        if (ReadLongElement(arr, k, v)) {
            out += " ";
            out += ProfileName(v);
        }
    }
    out += "\n";
}

/* --------------------------------------------------------------------------
 * Inserted-media identification
 *
 * IDiscFormat2Data::get_CurrentPhysicalMediaType is the IMAPI2 property that
 * names the disc itself. get_CurrentProfiles is NOT a substitute: it lists
 * the feature profiles the drive currently advertises, which can contain
 * several entries at once. Bind a data writer to the recorder exactly like
 * DriveInformation() does, then read the one physical media type.
 * ------------------------------------------------------------------------ */
static bool QueryPhysicalMediaType(const std::string& devicePath,
                                   IMAPI_MEDIA_PHYSICAL_TYPE* outType) {
    if (!outType) return false;
    *outType = IMAPI_MEDIA_TYPE_UNKNOWN;
    Bstr id;
    if (!FindRecorderId(devicePath, id)) return false;
    Com<IDiscRecorder2> recorder = OpenRecorder(id);
    if (!recorder) return false;

    Com<IDiscFormat2Data> writer;
    HRESULT hr = CoCreateInstance(CLSID_MsftDiscFormat2Data, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_IDiscFormat2Data,
                                  (void**)&writer);
    if (FAILED(hr) || !writer) {
        SetBurnLastError(HrMsg(hr, "CoCreateInstance(IDiscFormat2Data)"));
        return false;
    }
    writer->put_Recorder(recorder.get());
    SetClientName(writer.get());
    hr = writer->get_CurrentPhysicalMediaType(outType);
    if (FAILED(hr)) {
        SetBurnLastError(HrMsg(hr, "get_CurrentPhysicalMediaType") +
                         " for " + devicePath);
        return false;
    }
    return true;
}

/* Map IMAPI2's physical media enum onto the backend-independent BURN_MEDIA_*
 * codes declared in the public header. */
static int MediaTypeCodeFrom(IMAPI_MEDIA_PHYSICAL_TYPE mt) {
    switch (mt) {
    case IMAPI_MEDIA_TYPE_CDROM: return BURN_MEDIA_CD_ROM;
    case IMAPI_MEDIA_TYPE_CDR: return BURN_MEDIA_CD_R;
    case IMAPI_MEDIA_TYPE_CDRW:
    case IMAPI_MEDIA_TYPE_DISK: return BURN_MEDIA_CD_RW;
    case IMAPI_MEDIA_TYPE_DVDROM: return BURN_MEDIA_DVD_ROM;
    case IMAPI_MEDIA_TYPE_DVDDASHR: return BURN_MEDIA_DVD_R;
    case IMAPI_MEDIA_TYPE_DVDDASHRW: return BURN_MEDIA_DVD_RW;
    case IMAPI_MEDIA_TYPE_DVDPLUSR: return BURN_MEDIA_DVD_PLUS_R;
    case IMAPI_MEDIA_TYPE_DVDPLUSRW: return BURN_MEDIA_DVD_PLUS_RW;
    case IMAPI_MEDIA_TYPE_DVDRAM: return BURN_MEDIA_DVD_RAM;
    case IMAPI_MEDIA_TYPE_DVDPLUSR_DUALLAYER:
    case IMAPI_MEDIA_TYPE_DVDDASHR_DUALLAYER:
    case IMAPI_MEDIA_TYPE_DVDPLUSRW_DUALLAYER: return BURN_MEDIA_DVD_DL;
    case IMAPI_MEDIA_TYPE_BDROM: return BURN_MEDIA_BD_ROM;
    case IMAPI_MEDIA_TYPE_BDR: return BURN_MEDIA_BD_R;
    case IMAPI_MEDIA_TYPE_BDRE: return BURN_MEDIA_BD_RE;
    case IMAPI_MEDIA_TYPE_HDDVDROM:
    case IMAPI_MEDIA_TYPE_HDDVDR:
    case IMAPI_MEDIA_TYPE_HDDVDRAM: return BURN_MEDIA_HD_DVD;
    case IMAPI_MEDIA_TYPE_UNKNOWN: return BURN_MEDIA_UNKNOWN;
    default: return BURN_MEDIA_OTHER;
    }
}

std::string MediaType(const std::string& devicePath) {
    IMAPI_MEDIA_PHYSICAL_TYPE mt = IMAPI_MEDIA_TYPE_UNKNOWN;
    if (!QueryPhysicalMediaType(devicePath, &mt)) return "";
    return MediaTypeName(mt);
}

int MediaTypeCode(const std::string& devicePath) {
    IMAPI_MEDIA_PHYSICAL_TYPE mt = IMAPI_MEDIA_TYPE_UNKNOWN;
    if (!QueryPhysicalMediaType(devicePath, &mt)) return BURN_MEDIA_UNKNOWN;
    return MediaTypeCodeFrom(mt);
}

/* A physical drive needs a moment after a disc is inserted. IMAPI2 reports
 * these HRESULTs while it spins up or re-reads the medium; a bounded retry
 * keeps a perfectly good burner from being reported as unusable. */
static bool IsTransientMediaHr(HRESULT hr) {
    switch (hr) {
    case E_IMAPI_RECORDER_MEDIA_BECOMING_READY:
    case E_IMAPI_RECORDER_MEDIA_FORMAT_IN_PROGRESS:
    case E_IMAPI_RECORDER_MEDIA_INCOMPATIBLE:
    case E_IMAPI_RECORDER_MEDIA_NO_MEDIA:
        return true;
    default:
        return false;
    }
}

/* --------------------------------------------------------------------------
 * Media readiness
 *
 * A recorder that initialises successfully is NOT necessarily able to burn:
 * a real drive with an empty tray has no medium, and IMAPI2 only reports that
 * at Write() time as E_IMAPI_DF2DATA_MEDIA_IS_NOT_SUPPORTED (0xC0AA0406).
 * Combine three independent IMAPI2 facts so callers can decide up front:
 *   - get_CurrentPhysicalMediaType: is a recognisable disc in the drive?
 *   - IsCurrentMediaSupported: does the recorder accept this medium?
 *   - get_CurrentMediaStatus: blank / appendable / rewritable / blocked?
 * All three are read-only. Returns -1 only when the recorder cannot be
 * resolved at all; otherwise a BURN_MEDIA_STATE_* bitmask.
 * ------------------------------------------------------------------------ */
static int QueryMediaState(const std::string& devicePath, std::string* outText) {
    if (outText) outText->clear();

    Bstr id;
    if (!FindRecorderId(devicePath, id)) {
        if (outText) {
            *outText = "Recorder not found for " + devicePath +
                       "; use ListBurningDevices() to list valid device paths";
        }
        return -1;
    }
    Com<IDiscRecorder2> recorder = OpenRecorder(id);
    if (!recorder) {
        if (outText) {
            *outText = "Recorder for " + devicePath +
                       " could not be opened (busy or write-protected driver)";
        }
        return -1;
    }

    int state = BURN_MEDIA_STATE_RECORDER_OK;
    Com<IDiscFormat2Data> writer;
    HRESULT hr = CoCreateInstance(CLSID_MsftDiscFormat2Data, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_IDiscFormat2Data,
                                  (void**)&writer);
    if (FAILED(hr) || !writer) {
        if (outText) *outText = HrMsg(hr, "CoCreateInstance(IDiscFormat2Data)");
        return state;
    }
    writer->put_Recorder(recorder.get());
    SetClientName(writer.get());

    IMAPI_MEDIA_PHYSICAL_TYPE mt = IMAPI_MEDIA_TYPE_UNKNOWN;
    HRESULT mediaHr = writer->get_CurrentPhysicalMediaType(&mt);
    for (int attempt = 0; attempt < 3 &&
         (FAILED(mediaHr) && IsTransientMediaHr(mediaHr)); ++attempt) {
        Sleep(500);
        mediaHr = writer->get_CurrentPhysicalMediaType(&mt);
    }
    if (SUCCEEDED(mediaHr) && mt != IMAPI_MEDIA_TYPE_UNKNOWN) {
        state |= BURN_MEDIA_STATE_MEDIA_PRESENT;
    }

    VARIANT_BOOL supported = VARIANT_FALSE;
    const HRESULT supportHr =
        writer->IsCurrentMediaSupported(recorder.get(), &supported);
    if (SUCCEEDED(supportHr) && supported == VARIANT_TRUE) {
        state |= BURN_MEDIA_STATE_MEDIA_SUPPORTED;
    }

    IMAPI_FORMAT2_DATA_MEDIA_STATE ms = IMAPI_FORMAT2_DATA_MEDIA_STATE_UNKNOWN;
    const HRESULT statusHr = writer->get_CurrentMediaStatus(&ms);
    if (SUCCEEDED(statusHr)) {
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
        if (ms & IMAPI_FORMAT2_DATA_MEDIA_STATE_ERASE_REQUIRED)
            state |= BURN_MEDIA_STATE_ERASE_REQUIRED;
    }

    const int writableStates = BURN_MEDIA_STATE_BLANK |
                               BURN_MEDIA_STATE_APPENDABLE |
                               BURN_MEDIA_STATE_OVERWRITE_ONLY;
    const int blockingStates = BURN_MEDIA_STATE_WRITE_PROTECTED |
                               BURN_MEDIA_STATE_FINALIZED |
                               BURN_MEDIA_STATE_DAMAGED;
    const bool ready = (state & BURN_MEDIA_STATE_MEDIA_PRESENT) != 0 &&
                       (state & BURN_MEDIA_STATE_MEDIA_SUPPORTED) != 0 &&
                       (state & writableStates) != 0 &&
                       (state & blockingStates) == 0;
    if (ready) state |= BURN_MEDIA_STATE_READY;

    if (outText) {
        if (ready) {
            *outText = "Ready: " + MediaTypeName(mt) +
                       " is blank, appendable or rewritable";
        } else if (!(state & BURN_MEDIA_STATE_MEDIA_PRESENT)) {
            *outText = "No writable disc detected in " + devicePath +
                       "; insert a blank CD-R/DVD-R or a rewritable disc "
                       "(IMAPI2 " + HrHex(mediaHr) + ")";
        } else if (state & BURN_MEDIA_STATE_WRITE_PROTECTED) {
            *outText = "The disc is write-protected; remove the write "
                       "protection or use another disc";
        } else if (state & BURN_MEDIA_STATE_FINALIZED) {
            *outText = "The disc is finalized and cannot accept more data; "
                       "use a blank disc";
        } else if (state & BURN_MEDIA_STATE_DAMAGED) {
            *outText = "The disc is damaged or unreadable; use another disc";
        } else if (!(state & BURN_MEDIA_STATE_MEDIA_SUPPORTED)) {
            *outText = "The inserted medium is not supported for data "
                       "burning; use a blank CD-R/DVD-R (IMAPI2 " +
                       HrHex(supportHr) + ")";
        } else if (!(state & writableStates)) {
            *outText = "The disc is neither blank, appendable nor "
                       "rewritable; erase it or insert a blank disc";
        } else {
            *outText = "The drive is not ready for writing (media status " +
                       HrHex(statusHr) + ")";
        }
    }
    return state;
}

int MediaState(const std::string& devicePath) {
    return QueryMediaState(devicePath, nullptr);
}

std::string MediaStateText(const std::string& devicePath) {
    std::string text;
    (void)QueryMediaState(devicePath, &text);
    return text;
}

std::string DriveInformation(const std::string& devicePath) {
    std::string out;
    Bstr id;
    if (!FindRecorderId(devicePath, id)) return out;
    Com<IDiscRecorder2> recorder = OpenRecorder(id);
    if (!recorder) return out;

    Bstr prodId, vendorId, revision;
    recorder->get_ProductId(prodId.out());
    recorder->get_VendorId(vendorId.out());
    recorder->get_ProductRevision(revision.out());

    out += "devicePath: " + devicePath + "\n";
    out += "vendor:     " + BstrToUtf8(vendorId) + "\n";
    out += "product:    " + BstrToUtf8(prodId) + "\n";
    out += "revision:   " + BstrToUtf8(revision) + "\n";

    {
        SafeArr paths;
        if (SUCCEEDED(recorder->get_VolumePathNames(paths.out())) && paths.get()) {
            out += "volumePaths:";
            for (LONG k = (LONG)paths.lower(); k <= (LONG)paths.upper(); ++k) {
                const std::string vp = VolumePathAt(paths.get(), k);
                if (!vp.empty()) out += " " + vp;
            }
            out += "\n";
        }
    }
    {
        SafeArr current;
        if (SUCCEEDED(recorder->get_CurrentProfiles(current.out())) && current.get()) {
            AppendProfileArray(out, current.get(), "currentProfiles");
        }
    }
    {
        SafeArr supported;
        if (SUCCEEDED(recorder->get_SupportedProfiles(supported.out())) && supported.get()) {
            AppendProfileArray(out, supported.get(), "supportedProfiles");
        }
    }

    /* Media and writer state, read through a data writer on this device. */
    Com<IDiscFormat2Data> writer;
    if (SUCCEEDED(CoCreateInstance(CLSID_MsftDiscFormat2Data, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IDiscFormat2Data, (void**)&writer)) && writer) {
        writer->put_Recorder(recorder.get());
        SetClientName(writer.get());

        VARIANT_BOOL recOk = VARIANT_FALSE, mediaOk = VARIANT_FALSE;
        writer->IsRecorderSupported(recorder.get(), &recOk);
        writer->IsCurrentMediaSupported(recorder.get(), &mediaOk);
        out += std::string("imapi2RecorderSupported: ") + (recOk == VARIANT_TRUE ? "yes" : "no") + "\n";
        out += std::string("imapi2MediaSupported:    ") + (mediaOk == VARIANT_TRUE ? "yes" : "no") + "\n";

        IMAPI_MEDIA_PHYSICAL_TYPE mt = IMAPI_MEDIA_TYPE_UNKNOWN;
        writer->get_CurrentPhysicalMediaType(&mt);
        out += "media: " + MediaTypeName(mt) + "\n";

        IMAPI_FORMAT2_DATA_MEDIA_STATE st = IMAPI_FORMAT2_DATA_MEDIA_STATE_UNKNOWN;
        writer->get_CurrentMediaStatus(&st);
        out += "mediaState:";
        if (st == IMAPI_FORMAT2_DATA_MEDIA_STATE_UNKNOWN) out += " unknown";
        if (st & IMAPI_FORMAT2_DATA_MEDIA_STATE_BLANK) out += " blank";
        if (st & IMAPI_FORMAT2_DATA_MEDIA_STATE_APPENDABLE) out += " appendable";
        if (st & IMAPI_FORMAT2_DATA_MEDIA_STATE_FINAL_SESSION) out += " final-session";
        if (st & IMAPI_FORMAT2_DATA_MEDIA_STATE_DAMAGED) out += " damaged";
        if (st & IMAPI_FORMAT2_DATA_MEDIA_STATE_ERASE_REQUIRED) out += " erase-required";
        if (st & IMAPI_FORMAT2_DATA_MEDIA_STATE_NON_EMPTY_SESSION) out += " non-empty-session";
        if (st & IMAPI_FORMAT2_DATA_MEDIA_STATE_WRITE_PROTECTED) out += " write-protected";
        if (st & IMAPI_FORMAT2_DATA_MEDIA_STATE_FINALIZED) out += " finalized";
        if (st & IMAPI_FORMAT2_DATA_MEDIA_STATE_UNSUPPORTED_MEDIA) out += " unsupported-media";
        if (st & IMAPI_FORMAT2_DATA_MEDIA_STATE_OVERWRITE_ONLY) out += " overwrite-only";
        out += "\n";

        /* IMAPI_MEDIA_WRITE_PROTECT_STATE is a bitmask of protect reasons:
         * zero means the media is writable. */
        IMAPI_MEDIA_WRITE_PROTECT_STATE wpState = (IMAPI_MEDIA_WRITE_PROTECT_STATE)0;
        VARIANT_BOOL physBlank = VARIANT_FALSE, heurBlank = VARIANT_FALSE;
        writer->get_WriteProtectStatus(&wpState);
        writer->get_MediaPhysicallyBlank(&physBlank);
        writer->get_MediaHeuristicallyBlank(&heurBlank);
        out += std::string("writeProtected:    ") + (wpState != 0 ? "yes" : "no") + "\n";
        if (wpState != 0) {
            char why[128];
            snprintf(why, sizeof(why), " (reasons mask 0x%04lX%s%s%s%s%s%s)\n",
                     (unsigned long)wpState,
                     (wpState & IMAPI_WRITEPROTECTED_UNTIL_POWERDOWN) ? " until-powerdown" : "",
                     (wpState & IMAPI_WRITEPROTECTED_BY_CARTRIDGE) ? " cartridge" : "",
                     (wpState & IMAPI_WRITEPROTECTED_BY_MEDIA_SPECIFIC_REASON) ? " media" : "",
                     (wpState & IMAPI_WRITEPROTECTED_BY_SOFTWARE_WRITE_PROTECT) ? " software" : "",
                     (wpState & IMAPI_WRITEPROTECTED_BY_DISC_CONTROL_BLOCK) ? " dcb" : "",
                     (wpState & IMAPI_WRITEPROTECTED_READ_ONLY_MEDIA) ? " read-only-media" : "");
            out += why;
        } else {
            out += "\n";
        }
        out += std::string("physicallyBlank:   ") + (physBlank == VARIANT_TRUE ? "yes" : "no") + "\n";
        out += std::string("heuristicallyBlank: ") + (heurBlank == VARIANT_TRUE ? "yes" : "no") + "\n";

        LONG total = 0, freeSectors = 0, nextWritable = 0, curSpeed = 0, reqSpeed = 0;
        writer->get_TotalSectorsOnMedia(&total);
        writer->get_FreeSectorsOnMedia(&freeSectors);
        writer->get_NextWritableAddress(&nextWritable);
        writer->get_CurrentWriteSpeed(&curSpeed);
        writer->get_RequestedWriteSpeed(&reqSpeed);
        char nums[192];
        snprintf(nums, sizeof(nums),
                 "sectors: total=%ld free=%ld nextWritable=%ld (%.1f MB free)\n"
                 "writeSpeed: current=%ld requested=%ld sectors/s "
                 "(CD 1x=75, DVD 1x=676, BD 1x=2195)\n",
                 (long)total, (long)freeSectors, (long)nextWritable,
                 (double)freeSectors * (double)kSectorSize / (1024.0 * 1024.0),
                 (long)curSpeed, (long)reqSpeed);
        out += nums;

        const int base = BaseSectorsPerSecond(writer.get(), "");
        SafeArr speeds;
        if (SUCCEEDED(writer->get_SupportedWriteSpeeds(speeds.out())) && speeds.get()) {
            out += "supportedWriteSpeeds:";
            for (LONG k = (LONG)speeds.lower(); k <= (LONG)speeds.upper(); ++k) {
                LONG v = 0;
                if (ReadLongElement(speeds.get(), k, v)) {
                    char b[24];
                    snprintf(b, sizeof(b), " %ld", (long)(base > 0 ? v / base : v));
                    out += b;
                }
            }
            if (base > 0) out += " x";
            out += "\n";
        }
    }

    /* Raw MMC disc layout when passthrough is available. */
    Com<IDiscRecorder2Ex> ex;
    if (SUCCEEDED(recorder->QueryInterface(IID_IDiscRecorder2Ex, (void**)&ex)) && ex) {
        BYTE* discInfo = nullptr;
        ULONG discInfoSize = 0;
        if (SUCCEEDED(ex->GetDiscInformation(&discInfo, &discInfoSize)) && discInfo &&
            discInfoSize >= 10) {
            /* MMC "Read Disc Information": first track at bytes 2..5 and
             * last track/session at 6..9, both big-endian. */
            const unsigned long firstTrack =
                ((unsigned long)discInfo[2] << 24) | ((unsigned long)discInfo[3] << 16) |
                ((unsigned long)discInfo[4] << 8) | (unsigned long)discInfo[5];
            const unsigned long lastTrack =
                ((unsigned long)discInfo[6] << 24) | ((unsigned long)discInfo[7] << 16) |
                ((unsigned long)discInfo[8] << 8) | (unsigned long)discInfo[9];
            char b[128];
            snprintf(b, sizeof(b), "discInformation: firstTrack=%lu lastTrack=%lu\n",
                     firstTrack, lastTrack);
            out += b;
            CoTaskMemFree(discInfo);
        }
    }

    return out;
}

} /* namespace burnwin */

#endif /* _WIN32 */
