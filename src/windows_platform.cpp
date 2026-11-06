#include "burning_utility.h"
#include <string>
#include <vector>
#include <memory>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#ifdef _WIN32

#include <windows.h>
#include <imapi2.h>
#include <imapi2fs.h>
#include <comdef.h>
#include <shlobj.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

extern "C" void SetBurnLastError(const std::string& error);
extern "C" void NotifyProgressBurn(int percent, const char* status);

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
            strcpy(info.mediaType, "CD/DVD");
            strcpy(info.devicePath, "D:");
            strcpy(info.deviceName, "Default Optical Drive");
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
                            strcpy(info.mediaType, "CD/DVD");

                            BSTR volPath = nullptr;
                            pRecorder->get_VolumePathNames(&volPath);
                            if (volPath) {
                                WideCharToMultiByte(CP_UTF8, 0, volPath, -1, info.devicePath, 511, nullptr, nullptr);
                                SysFreeString(volPath);
                            } else {
                                strcpy(info.devicePath, "D:");
                            }

                            BSTR prodId = nullptr;
                            pRecorder->get_ProductId(&prodId);
                            if (prodId) {
                                WideCharToMultiByte(CP_UTF8, 0, prodId, -1, info.deviceName, 255, nullptr, nullptr);
                                SysFreeString(prodId);
                            } else {
                                strcpy(info.deviceName, "Optical Drive");
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
        NotifyProgressBurn(10, "Initializing burn process...");

        IDiscMaster2* pDiscMaster = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_MsftDiscMaster2, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IDiscMaster2, (void**)&pDiscMaster);
        if (FAILED(hr)) {
            SetBurnLastError("Failed to create IDiscMaster2");
            return false;
        }

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
                        BSTR volPath = nullptr;
                        pRec->get_VolumePathNames(&volPath);
                        char path[512] = "";
                        if (volPath) {
                            WideCharToMultiByte(CP_UTF8, 0, volPath, -1, path, 511, nullptr, nullptr);
                            SysFreeString(volPath);
                        }
                        if (strcmp(path, devicePath.c_str()) == 0 || !pRecorder) {
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

        if (speed > 0) {
            pRecorder->put_RequestedWriteSpeed(speed * 1000);
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
        pDataWriter->put_ClientName(L"BurningUtility");

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

        hr = pDataWriter->Write(pStream);
        if (FAILED(hr)) {
            SetBurnLastError("Burn write failed");
            pStream->Release();
            pDataWriter->Release();
            pRecorder->Release();
            pDiscMaster->Release();
            return false;
        }

        pStream->Release();
        pDataWriter->Release();
        pRecorder->Release();
        pDiscMaster->Release();

        NotifyProgressBurn(100, "Burn completed");
        return true;
    }

    bool CreateISO(const std::string& sourcePath, const std::string& isoPath,
                   const std::string& volumeLabel) override {
        NotifyProgressBurn(10, "Creating ISO image...");

        IFileSystemImage* pFsImage = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_MsftFileSystemImage, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IFileSystemImage, (void**)&pFsImage);
        if (FAILED(hr)) {
            SetBurnLastError("Failed to create IFileSystemImage");
            return false;
        }

        int wlen = MultiByteToWideChar(CP_UTF8, 0, volumeLabel.c_str(), -1, nullptr, 0);
        if (wlen > 0) {
            wchar_t* wbuf = new wchar_t[wlen];
            MultiByteToWideChar(CP_UTF8, 0, volumeLabel.c_str(), -1, wbuf, wlen);
            BSTR bstrLabel = SysAllocString(wbuf);
            pFsImage->put_VolumeName(bstrLabel);
            SysFreeString(bstrLabel);
            delete[] wbuf;
        }

        pFsImage->put_FileSystemsToCreate(FsiFileSystemISO9660 | FsiFileSystemJoliet);

        int slen = MultiByteToWideChar(CP_UTF8, 0, sourcePath.c_str(), -1, nullptr, 0);
        if (slen > 0) {
            wchar_t* wbuf = new wchar_t[slen];
            MultiByteToWideChar(CP_UTF8, 0, sourcePath.c_str(), -1, wbuf, slen);
            BSTR bstrSource = SysAllocString(wbuf);
            pFsImage->put_ImportedDirectoryItems(bstrSource);
            SysFreeString(bstrSource);
            delete[] wbuf;
        }

        NotifyProgressBurn(50, "Writing ISO file...");

        IFileSystemImageResult* pResult = nullptr;
        hr = pFsImage->CreateResultImage(&pResult);
        if (FAILED(hr)) {
            SetBurnLastError("Failed to create ISO result image");
            pFsImage->Release();
            return false;
        }

        IStream* pStream = nullptr;
        pResult->get_ImageStream(&pStream);
        if (pStream) {
            FILE* f = fopen(isoPath.c_str(), "wb");
            if (f) {
                char buf[65536];
                ULONG read = 0;
                while (SUCCEEDED(pStream->Read(buf, sizeof(buf), &read)) && read > 0) {
                    fwrite(buf, 1, read, f);
                }
                fclose(f);
            } else {
                SetBurnLastError("Cannot open output file: " + isoPath);
            }
            pStream->Release();
        }

        pResult->Release();
        pFsImage->Release();

        NotifyProgressBurn(100, "ISO creation completed");
        return true;
    }

    std::string GetPlatformInfo() override {
        return "Windows (IMAPI2)";
    }

    bool CheckToolAvailability(const std::string& toolName) override {
        return true;
    }
};

std::unique_ptr<IPlatform> CreatePlatformImpl() {
    return std::make_unique<WindowsPlatform>();
}

#endif
