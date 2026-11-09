# BurningDLL v3 能力扩展 — IMAPI2 API 事实清单

本文件记录实现六类新能力时**已逐一从本机 Windows SDK 头文件核实**的接口事实
（`imapi2.h` / `imapi2fs.h`, `Windows Kits\10\Include\10.0.26100.0\um`）。
实现者不得在此之外臆造任何方法名、参数类型或枚举值。

## 1. 公开契约（本项目自定义，已在 `src/burning_utility.h` 落地）

- 新任务模式：`BURN_JOB_MODE_AUDIO_TAO=4`、`ERASE=5`、`APPEND=6`、`VERIFY=7`、
  `CREATE_RAW=8`、`BURN_RAW=9`。
- 能力 ID：`BURN_CAP_IMAGE_BURN=0 … BURN_CAP_DRIVE_INFO=11`
  （顺序：IMAGE_BURN, CREATE_IMAGE, AUDIO_TAO, ERASE, MULTISESSION, UDF,
  BOOT_IMAGE, READBACK_VERIFY, RAW_BURN, RAW_IMAGE, TRAY_CONTROL, DRIVE_INFO）。
- `BURN_VERIFY_NONE/QUICK/FULL = 0/1/2`；`BURN_FS_ISO9660/JOLIET/UDF = 1/2/4`；
  `BURN_BOOT_EMU_NONE/FLOPPY_12/FLOPPY_144/FLOPPY_288/HARDDISK = 0..4`；
  `BURN_BOOT_PLATFORM_X86/PPC/MAC/EFI = 0/1/2/0xEF`。
- `speed` 语义始终是**倍速**，由后端换算单位。
- 错误码：不支持 → `BURN_ERROR_UNSUPPORTED_FORMAT(-8)`；参数 → `-1`；
  设备 → `-2`；写入失败 → `-4`；镜像生成失败 → `-5`；取消 → `-10`。
- 引擎已在调用后端前做能力门控；后端只需如实实现 `Capability()` 与虚函数。

## 2. CLSID（可直接用于 `CoCreateInstance`）

`CLSID_MsftDiscMaster2`, `CLSID_MsftDiscRecorder2`, `CLSID_MsftDiscFormat2Data`,
`CLSID_MsftDiscFormat2Erase`, `CLSID_MsftDiscFormat2TrackAtOnce`,
`CLSID_MsftDiscFormat2RawCD`, `CLSID_MsftRawCDImageCreator`,
`CLSID_MsftMultisessionSequential`, `CLSID_MsftMultisessionRandomWrite`,
`CLSID_MsftFileSystemImage`, `CLSID_BootOptions`, `CLSID_MsftIsoImageManager`,
`CLSID_MsftStreamConcatenate`, `CLSID_MsftStreamInterleave`,
`CLSID_MsftStreamPrng001`, `CLSID_MsftStreamZero`.

IID 均以 `IID_IXxx` / `IID_DDiscFormat2XxxEvents` 形式在头文件中
`EXTERN_C const IID` 声明（`IID_IMultisession`, `IID_IMultisessionSequential`,
`IID_IMultisessionSequential2`, `IID_IBootOptions`, `IID_IIsoImageManager`,
`IID_IBurnVerification`, `IID_IDiscRecorder2Ex`, `IID_IRawCDImageCreator`,
`IID_IRawCDImageTrackInfo`, `IID_DDiscFormat2TrackAtOnceEvents`,
`IID_DDiscFormat2EraseEvents`, `IID_DDiscFormat2RawCDEvents` 等）。
也可用 `__uuidof(...)`。

## 3. 已核实的方法集（逐条来自头文件）

### `IDiscFormat2Data`（含基类 `IDiscFormat2`）
基类：`IsRecorderSupported`, `IsCurrentMediaSupported`,
`get_MediaPhysicallyBlank`, `get_MediaHeuristicallyBlank`, `get_SupportedMediaTypes`。
自身：`put_Recorder/get_Recorder`, `put_BufferUnderrunFreeDisabled`,
`put_PostgapAlreadyInImage`, `get_CurrentMediaStatus(IMAPI_FORMAT2_DATA_MEDIA_STATE*)`,
`get_WriteProtectStatus`, `get_TotalSectorsOnMedia`, `get_FreeSectorsOnMedia`,
`get_NextWritableAddress(LONG*)`, `get_StartAddressOfPreviousSession`,
`get_LastWrittenAddressOfPreviousSession`, `put_ForceMediaToBeClosed(VARIANT_BOOL)`,
`put_DisableConsumerDvdCompatibilityMode`, `get_CurrentPhysicalMediaType`,
`put_ClientName(BSTR)`, `get_Requested/CurrentWriteSpeed(LONG*)`,
`get_SupportedWriteSpeeds(SAFEARRAY**)`, `get_SupportedWriteSpeedDescriptors(SAFEARRAY**)`,
`put_ForceOverwrite(VARIANT_BOOL)`, `get_MultisessionInterfaces(SAFEARRAY**)`,
`Write(IStream*)`, `CancelWrite()`,
`SetWriteSpeed(LONG RequestedSectorsPerSecond, VARIANT_BOOL RotationTypeIsPureCAV)`。

`IMAPI_FORMAT2_DATA_MEDIA_STATE`：`UNKNOWN=0`, `INFORMATIONAL_MASK=0xf`,
`OVERWRITE_ONLY/RANDOMLY_WRITABLE=0x1`, `BLANK=0x2`, `APPENDABLE=0x4`,
`FINAL_SESSION=0x8`, `DAMAGED=0x400`, `ERASE_REQUIRED=0x800`,
`NON_EMPTY_SESSION=0x1000`, `WRITE_PROTECTED=0x2000`, `FINALIZED=0x4000`,
`UNSUPPORTED_MEDIA=0x8000`。

`IMAPI_MEDIA_PHYSICAL_TYPE`：`UNKNOWN=0`, `CDROM=1`, `CDR=2`, `CDRW=3`,
`DVDROM=4`, `DVDRAM=5`, `DVDPLUSR=6`, `DVDPLUSRW=7`, `DVDPLUSR_DUALLAYER=8`,
`DVDDASHR=9`, `DVDDASHRW=0xa`, `DVDDASHR_DUALLAYER=0xb`, `DISK=0xc`,
`DVDPLUSRW_DUALLAYER=0xd`, `HDDVDROM=0xe`, `HDDVDR=0xf`, `HDDVDRAM=0x10`,
`BDROM=0x11`, `BDR=0x12`, `BDRE=0x13`。

### `IDiscFormat2Erase`
`put_Recorder`, `put_FullErase(VARIANT_BOOL)`, `get_CurrentPhysicalMediaType`,
`put_ClientName`, `EraseMedia()`。事件接口 `DDiscFormat2EraseEvents`
（`IID_DDiscFormat2EraseEvents`）。**注意：头文件中不存在
`IDiscFormat2EraseEventArgs`**，其 Update 事件的参数不可假定为强类型 args 对象。
`EraseMedia()` 文档：不可取消。

### `IDiscFormat2TrackAtOnce`
`PrepareMedia()`, `AddAudioTrack(IStream*)`, `CancelAddTrack()`, `ReleaseMedia()`,
`SetWriteSpeed(LONG, VARIANT_BOOL)`, `put_Recorder`, `put_BufferUnderrunFreeDisabled`,
`get_NumberOfExistingTracks`, `get_TotalSectorsOnMedia`, `get_FreeSectorsOnMedia`,
`get_UsedSectorsOnMedia`, `put_DoNotFinalizeMedia(VARIANT_BOOL)`,
`get_ExpectedTableOfContents`, `get_CurrentPhysicalMediaType`, `put_ClientName`,
`get_Requested/CurrentWriteSpeed`, `get_SupportedWriteSpeeds`,
`get_SupportedWriteSpeedDescriptors`。
事件：`IID_DDiscFormat2TrackAtOnceEvents`，`DISPID 0x200 = Update`。
事件参数 `IID_IDiscFormat2TrackAtOnceEventArgs` 成员含
`get_CurrentAction`, `get_CurrentTrackNumber`, `get_ElapsedTime`, `get_RemainingTime`,
`get_StartLba`, `get_SectorCount`, `get_LastReadLba`, `get_LastWrittenLba`,
`get_Free/Total/UsedSystemBuffer`。

`AddAudioTrack` 官方语义（已查 Microsoft Learn）：流内容必须是
**44.1 kHz / 16-bit / 立体声 裸 PCM（无 WAV 头）**；最少 4 秒（不足会补静音）；
轨道间自动插 2 秒 gap；最多 99 轨；需先 `put_Recorder` + `PrepareMedia`；
`put_DoNotFinalizeMedia(VARIANT_TRUE)` 可保留可追加状态。
关键错误码：`E_IMAPI_DF2TAO_MEDIA_IS_NOT_PREPARED=0xC0AA0502`,
`TRACK_LIMIT_REACHED=0xC0AA0508`, `STREAM_NOT_SUPPORTED=0xC0AA050D`,
`NOT_ENOUGH_SPACE=0xC0AA0509`。

### `IDiscFormat2RawCD`
`PrepareMedia`, `WriteMedia(IStream*)`, `WriteMedia2(IStream* data, LONG streamLeadInSectors)`,
`CancelWrite`, `ReleaseMedia`, `SetWriteSpeed(LONG, VARIANT_BOOL)`, `put_Recorder`,
`put_BufferUnderrunFreeDisabled`, `get_StartOfNextSession`,
`get_LastPossibleStartOfLeadout`, `get_CurrentPhysicalMediaType`,
`get_SupportedSectorTypes(SAFEARRAY**)`,
`put_RequestedSectorType/get_RequestedSectorType(IMAPI_FORMAT2_RAW_CD_DATA_SECTOR_TYPE)`,
`put_ClientName`, `get_*WriteSpeed*`。
事件 `IID_DDiscFormat2RawCDEvents`，`DISPID 0x200 = Update`；
`IID_IDiscFormat2RawCDEventArgs` 存在。
`IMAPI_FORMAT2_RAW_CD_DATA_SECTOR_TYPE`：`SUBCODE_PQ_ONLY=0x1`,
`SUBCODE_IS_COOKED=0x2`, `SUBCODE_IS_RAW=0x3`。

### `IRawCDImageCreator`（`CLSID_MsftRawCDImageCreator`）
`CreateResultImage(IStream**)`, `AddTrack(IMAPI_CD_SECTOR_TYPE dataType, IStream* data, LONG* trackIndex)`,
`AddSpecialPregap(IStream* data)`, `AddSubcodeRWGenerator(IStream* subcode)`,
`put_ResultingImageType/get_ResultingImageType(IMAPI_FORMAT2_RAW_CD_DATA_SECTOR_TYPE)`,
`get_StartOfLeadout`, `put_StartOfLeadoutLimit(LONG)`, `get_StartOfLeadoutLimit`,
`put_DisableGaplessAudio(VARIANT_BOOL)`,
`put_MediaCatalogNumber(BSTR)/get_MediaCatalogNumber`,
`put_StartingTrackNumber(LONG 1..99)/get_StartingTrackNumber`,
`get_TrackInfo`, `get_NumberOfExistingTracks`, `get_LastUsedUserSectorInImage`,
`get_ExpectedTableOfContents(SAFEARRAY**)`。

`IMAPI_CD_SECTOR_TYPE`：`AUDIO=0`, `MODE_ZERO=1`, `MODE1=2`, `MODE2FORM0=3`,
`MODE2FORM1=4`, `MODE2FORM2=5`, `MODE1RAW=6`, `MODE2FORM0RAW=7`,
`MODE2FORM1RAW=8`, `MODE2FORM2RAW=9`。

`IRawCDImageTrackInfo`：`get_StartingLba`, `get_SectorCount`, `get_TrackNumber`,
`get_SectorType`, `get_ISRC/put_ISRC(BSTR)`,
`get/put_DigitalAudioCopySetting`, `get/put_AudioHasPreemphasis(VARIANT_BOOL)`,
`get_TrackIndexes`, `AddTrackIndex(LONG lbaOffset)`, `ClearTrackIndex(LONG)`。

### `IFileSystemImage`（+ `IFileSystemImage2`, `3`）
基础：`put_VolumeName(BSTR)`, `put_FileSystemsToCreate(FsiFileSystems)`,
`get_Root(IFsiDirectoryItem**)`, `CreateResultImage(IFileSystemImageResult**)`,
`put_ISO9660InterchangeLevel(LONG)`, `get_ISO9660InterchangeLevel`,
`get_ISO9660InterchangeLevelsSupported`,
`put_UDFRevision(LONG)`, `get_UDFRevision`, `get_UDFRevisionsSupported`,
`put_SessionStartBlock(LONG)` / `get_SessionStartBlock`,
`put_FreeMediaBlocks(LONG)`, `get_FreeMediaBlocks`, `SetMaxMediaBlocksFromDevice`,
`ImportFileSystem(FsiFileSystems*)`, `ImportSpecificFileSystem(FsiFileSystems)`,
`IdentifyFileSystemsOnDisc`, `GetDefaultFileSystemForImport`,
`CalculateDiscIdentifier`, `get_BootImageOptions(IBootOptions**)` /
`put_BootImageOptions(IBootOptions*)`,
`put_MultisessionInterfaces(SAFEARRAY*)` / `get_MultisessionInterfaces`,
`ChooseImageDefaults`, `ChooseImageDefaultsForMediaType`, `get_UsedBlocks`,
`get_FileCount`, `get_DirectoryCount`, `put_WorkingDirectory(BSTR)`。
v2 增：`get_BootImageOptionsArray(SAFEARRAY**)` / `put_BootImageOptionsArray(SAFEARRAY*)`。
v3 增：`put_CreateRedundantUdfMetadataFiles(VARIANT_BOOL)`, `ProbeSpecificFileSystem`。

`FsiFileSystems`：`None=0`, `ISO9660=1`, `Joliet=2`, `UDF=4`, `Unknown=0x40000000`。

`IFileSystemImageResult`：`get_ImageStream`, `get_TotalBlocks`, `get_BlockSize`,
`get_DiscId`, `get_ProgressItems`。

`IFsiDirectoryItem`：`AddTree(BSTR, VARIANT_BOOL recurseSubfolders)`, `Add`,
`AddDirectory`, `AddFile`, `Remove`, `RemoveTree`, `get_Count`, `get_Item` …

### `IBootOptions`（`CLSID_BootOptions`）
`put_Manufacturer(BSTR)`, `get/put_PlatformId(PlatformId)`,
`get/put_Emulation(EmulationType)`, `get_ImageSize(ULONG*)`,
`get_BootImage(IStream**)`, `AssignBootImage(IStream* newVal)`。
`EmulationType`：`None=0`, `12MFloppy=1`, `144MFloppy=2`, `288MFloppy=3`,
`HardDisk=4`。`PlatformId`：`X86=0`, `PowerPC=1`, `Mac=2`, `EFI=0xef`。

### `IMultisession` / `IMultisessionSequential(2)`
基类 `IMultisession`：`get_IsSupportedOnCurrentMediaState`, `put_InUse(VARIANT_BOOL)`,
`get_InUse`, `get_ImportRecorder(IDiscRecorder2**)`。
`IMultisessionSequential` 增：`get_IsFirstDataSession`,
`get_StartAddressOfPreviousSession(LONG*)`,
`get_LastWrittenAddressOfPreviousSession(LONG*)`,
`get_NextWritableAddress(LONG*)`, `get_FreeSectorsOnMedia(LONG*)`。
`IMultisessionSequential2` 增：`get_WriteUnitSize`。

### `IBurnVerification`（`IID_IBurnVerification`）
`put_BurnVerificationLevel(IMAPI_BURN_VERIFICATION_LEVEL)` / `get_BurnVerificationLevel`。
`IMAPI_BURN_VERIFICATION_LEVEL`：`NONE=0`, `QUICK=1`, `FULL=2`。
需 `QueryInterface` 自 `IDiscFormat2Data` 实例（coclass 无独立 CLSID）。

### `IDiscRecorder2`（托盘与信息）
`InitializeDiscRecorder(BSTR)`, `ReleaseExclusiveAccess`,
`AcquireExclusiveAccess(VARIANT_BOOL force, BSTR clientName)`,
`EjectMedia()`, `CloseTray()`, `get_ProductId(BSTR*)`, `get_VendorId`,
`get_ProductRevision`, `get_VolumePathNames(SAFEARRAY** /* of VARIANT/VT_BSTR */)`,
`get_VolumeName`, `get_LegacyDeviceNumber`, `get_DeviceCanLoadMedia`,
`get_SupportedProfiles(SAFEARRAY**)`, `get_CurrentProfiles(SAFEARRAY**)`,
`get_SupportedFeaturePages`, `get_SupportedModePages`,
`get_ActiveDiscRecorder`, `get_ExclusiveAccessOwner`, `EnableMcn/DisableMcn`。
`IDiscRecorder2Ex`：`GetDiscInformation(BYTE**, ULONG*)`, `GetTrackInformation`,
`GetFeaturePage`, `GetModePage/SetModePage`, `SendCommandGetDataFromDevice(...)`,
`GetDeviceDescriptor/GetAdapterDescriptor`, `ReadDvdStructure` 等。

### `IIsoImageManager`（`CLSID_MsftIsoImageManager`, `IID_IIsoImageManager`）
`SetPath(BSTR)`, `SetStream(IStream*)`, `get_Path(BSTR*)`, `get_Stream(IStream**)`,
`Validate()`。→ 用于从光盘读出 ISO9660 文件系统流做回读比对。

### 事件 DISPID（已核实）
`DISPID_DDISCFORMAT2DATAEVENTS_UPDATE = 0x200`,
`DISPID_DDISCFORMAT2RAWCDEvents_UPDATE = 0x200`,
`DISPID_IDISCFORMAT2DATAEVENTARGS_ELAPSEDTIME = 0x300`,
`..._ESTIMATEDREMAININGTIME = 0x301`, `..._ESTIMATEDTOTALTIME = 0x302`,
`..._CURRENTACTION = 0x303`。
`DDiscFormat2TrackAtOnceEvents` 无独立 DISPID 宏，Update 的 dispid 亦为 0x200；
`DDiscFormat2EraseEvents` 同理（其事件参数类型头文件中不存在，勿假设）。

## 4. 换算常量

| 介质 | 1x | 单位 |
|---|---|---|
| CD | 75 | 扇区/秒（IMAPI2 `SetWriteSpeed`） |
| DVD | 676 | 扇区/秒 |
| BD | 2195 | 扇区/秒 |
| CD | 176.4(→177) | kB/s（libburn） |
| DVD | 1385 | kB/s |
| BD | 4496 | kB/s |

CD-DA：1 扇区 = 2352 字节 = 588 采样帧（4 字节/帧）；扇区/秒 = 75。
2 秒 = 150 扇区 = 352800 字节；4 秒 = 300 扇区 = 705600 字节（音频轨最小长度）。
`SetWriteSpeed(0xFFFFFFFF)` = 请求最快；非零值不被支持时驱动器自动就近调整
（返回 `S_IMAPI_SPEEDADJUSTED=0x00AA0004` 属成功码）。

## 5. 现有代码约定（务必遵守）

- 头文件注释不得使用破折号 `—`（MSVC C4819）。
- 进度：`NotifyProgressBurn(percent, text)`；取消：`IsBurnCancelled()`。
- 错误：`SetBurnLastError(std::string)`。
- 取消钩子：`SetBurnCancelHook(fn, ctx)` 前注册、调用完成后**先在锁内置空
  ctx 指针**再 `ClearBurnCancelHook(fn, ctx)`（它会等待在途钩子返回）。
- 每个 COM 对象在**所有**错误路径上 `Release()`；`BSTR` 用 `SysFreeString`；
  `SAFEARRAY` 用 `SafeArrayDestroy`；`CoTaskMemFree` 用于 `BYTE**` 输出。
- `get_VolumePathNames` 返回 `SAFEARRAY(VARIANT/VT_BSTR)`，不是 `SAFEARRAY(BSTR)`；
  `get_MultisessionInterfaces` / `get_SupportedWriteSpeedDescriptors` 返回
  `SAFEARRAY(VARIANT/VT_DISPATCH)`；`get_UDFRevisionsSupported` /
  `get_SupportedProfiles` / `get_SupportedWriteSpeeds` 等的整数列表也是
  `SAFEARRAY(VARIANT/VT_I4)`。一律先用 `SafeArrayGetElemsize` /
  `SafeArrayGetVartype` 判断元素布局，读出的 `VARIANT` 副本用 `VariantClear`，
  `BSTR` 副本用 `SysFreeString`，`IDispatch` 副本用 `Release`。
- 设备匹配只用**精确卷路径比较**，绝不"取第一个设备"。
- 写盘结果判定顺序：**成功优先于迟到取消**。
- 内部字符串按 UTF-8 处理，与 COM 交互时 `MultiByteToWideChar(CP_UTF8, ...)`。
- 读取**已插入光盘的物理类型**只能用
  `IDiscFormat2Data::get_CurrentPhysicalMediaType`（`IDiscFormat2Erase`
  也暴露同名属性）。`IDiscRecorder2::get_CurrentProfiles` 是驱动器当前
  宣告的功能 profile 列表（可同时多项），不是介质类型，不能拿来代替。
  `IMAPI_MEDIA_TYPE_UNKNOWN` 表示「无盘或无法识别」，不要据此断言一定有盘。

## 6. 对外读盘型接口（新增）

- `Burn_GetMediaType(devicePath)` 返回 `const char*`（DLL 内 thread_local
  缓冲，不要 free），例如 `CD-R` / `DVD+RW` / `BD-RE`；空串表示后端无法报告。
- `Burn_GetMediaTypeCode(devicePath)` 返回与后端无关的 `BURN_MEDIA_*` 码，
  便于程序分支；`BURN_MEDIA_UNKNOWN=0` 表示无法判断。
- 两者都是实时查询，插拔盘后需重新调用。libburn / drutil 后端目前走
  `IPlatform` 默认实现（诚实返回「不支持」），不伪造盘型。
- Web 控制台：`GET /api/media?device=F:` 返回 `{"type","code"}`；
  `/api/devices` 每项附带 `mediaType` / `mediaCode`。
- CLI：`burn_cli media-type F:`；`list-devices` 输出附加 `disc=` 列。

## 7. 介质就绪状态（真实刻录机空仓问题）

`IDiscRecorder2::InitializeDiscRecorder()` 在**空仓**时也会成功：它只证明
驱动器可独占打开，不证明里面有可写介质。此时直接 `IDiscFormat2Data::Write()`
会返回 `E_IMAPI_DF2DATA_MEDIA_IS_NOT_SUPPORTED`(`0xC0AA0406`)，用户只看到
一个 HRESULT，不知道要插盘。相关 SDK 常量：

- `E_IMAPI_RECORDER_MEDIA_NO_MEDIA` = `0xC0AA0202`（无盘）
- `E_IMAPI_RECORDER_MEDIA_INCOMPATIBLE` = `0xC0AA0203`（介质不兼容/格式未知，
  空仓时 `get_CurrentPhysicalMediaType` 常返回这个）
- `E_IMAPI_DF2DATA_MEDIA_NOT_BLANK` = `0xC0AA0405`
- `E_IMAPI_DF2DATA_MEDIA_IS_NOT_SUPPORTED` = `0xC0AA0406`

三个只读属性组合起来才能判断「现在能不能刻」：

- `get_CurrentPhysicalMediaType` -> 是否有可识别盘片；
- `IsCurrentMediaSupported(recorder, VARIANT_BOOL*)` -> 该 recorder 是否接受此介质；
- `get_CurrentMediaStatus` -> `BLANK(0x2)` / `APPENDABLE(0x4)` /
  `OVERWRITE_ONLY(0x1)` / `WRITE_PROTECTED(0x2000)` / `FINALIZED(0x4000)` /
  `DAMAGED(0x400)` / `ERASE_REQUIRED(0x800)` / `UNSUPPORTED_MEDIA(0x8000)`。
  注意 `INFORMATIONAL_MASK=0xf`，`UNSUPPORTED_MASK=0xfc00`。

对外接口：

- `Burn_GetMediaState(devicePath)` 返回 `BURN_MEDIA_STATE_*` 位掩码，
  `-1` 表示设备无法解析；`BURN_MEDIA_STATE_READY` 为计算位；
  `QUERY_UNSUPPORTED` 表示后端查不到（libburn / drutil 默认实现）。
- `Burn_GetMediaStateText(devicePath)` 返回一句简短英文原因；
  `Burn_GetMediaState` / `Text` 都是实时查询。
- `DeviceInfo.isWritable` 在 Windows 现在要求「recorder 可打开 **且**
  有可写盘片」，空仓真实刻录机为 `0`；`isEmpty` 只在 `BLANK` 时为 `1`。
  Linux/macOS 后端仍按原有 MMC / drutil 语义，不改。
- 刚插盘时光驱会短暂返回 `BECOMING_READY`(`0xC0AA0205`) /
  `INCOMPATIBLE`(`0xC0AA0203`) / `NO_MEDIA`(`0xC0AA0202`) /
  `FORMAT_IN_PROGRESS`(`0xC0AA0206`)。介质查询做 3 次 × 500 ms 有界重试，
  避免把正在上电的真实刻录机误判为不可用（空仓仍会在 ~1.5 s 后给出明确原因）。
- `BurnImageFile()` 在 `Write()` 前用同一个 writer 做
  `IsCurrentMediaSupported` 预检，只在明确返回 `VARIANT_FALSE` 时提前失败；
  `Write()` 失败信息追加可读的 HRESULT 解释（见 `WriteFailureHint`）。
- `burn_cli media-state F:`；`/api/devices` 与 `/api/media` 增加
  `state` / `ready` / `reason`；`/api/start` 对 IMAGE/DIRECTORY/APPEND
  模式做同样的预检，避免浏览器提交一个必然失败的作业。
