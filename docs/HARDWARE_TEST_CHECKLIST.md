# 真硬件验收清单（v3 六类刻录能力）

本清单用于在**有真实刻录机与空白/可擦写介质**的机器上，验收 API v3 的六类能力。
自动化测试（`ctest`）只覆盖无盘可验证部分；写入类路径必须按本清单人工确认。

配套程序：`hw_check`（构建产物在 `build/bin/<config>/`）。它把下面的步骤变成
可执行的 PASS / SKIP / FAIL 报告，并把不需要硬件的步骤先跑一遍。

```
# 只做只读与无盘步骤（安全，不改动介质）
hw_check E:

# 带破坏性写入步骤（会消耗介质），并指定音频轨与参考镜像
hw_check E: --destructive --audio track01.wav,track02.wav --iso hw_plain.iso
```

Linux 上设备路径形如 `/dev/sr0`（先 `ls /dev/sr*` 确认）。

退出码：0 = 全部通过或被明确跳过；1 = 有步骤失败。

---

## 0. 两平台能力差异（先看这里）

| 能力 | Windows (IMAPI2) | Linux (libburn/libisofs) | 说明 |
|---|---|---|---|
| 数据盘刻录 | 支持 | 支持 | |
| 镜像生成 (ISO9660/Joliet) | 支持 | 支持 | |
| 音频 CD (TAO) | 支持 | 支持 | Linux 需把 WAV 剥头为裸 PCM |
| 擦除 / 格式化 | 支持 | 支持 | Linux 仅可擦写介质 |
| 多会话追加 | 支持 | 支持 | |
| **UDF 文件系统** | 支持 | **不支持** | libisofs 1.5.2 完全无 UDF 支持；`Burn_GetCapability(BURN_CAP_UDF)` 返回 0，请求 UDF 会明确报错 |
| El Torito 引导镜像 | 支持 | 支持 | 两平台均通过同一套字节级断言 |
| 刻后回读校验 | 支持 | 支持 | Windows 走 MMC `READ(10)` 直通；Linux 直接读设备节点 |
| RAW 写盘 | 支持 | 支持 | Linux 用 `BURN_WRITE_RAW` + `BURN_BLOCK_RAW96R` |
| RAW 镜像生成 | 支持 | 支持 | 两者容器格式不同，见下 |
| 托盘控制 / 设备信息 | 支持 | 支持 | |

**RAW 镜像格式差异（重要）**：Windows 的 IMAPI2 PQ 输出是厂商自有容器
（开头为全零 lead-in，扇长不是 2352 的整数倍）；Linux 侧由本项目自建
**RAW96R** 镜像（每扇 2352 音频 + 96 子码 = 2448 字节，含正确编码的
Q 子码：CRC-16 校验、轨迹号、索引与绝对时间，首程序扇区为 00:02:00）。
两者**不能互相烧录**，请在本平台内成对使用。

**UDF 限制**：Linux 上请求 UDF 会被明确拒绝（`BURN_ERROR_UNSUPPORTED_FORMAT`
+ "libisofs has no UDF writer"），而不是静默生成一个只有 ISO9660 的镜像。

---

## 1. 前置条件

| 项目 | 要求 |
|---|---|
| 刻录机 | 支持 CD 写入；DVD/BD 项按需 |
| 介质 | 至少：1 张空白 CD-R（音频/数据）、1 张 CD-RW 或 DVD+RW（擦除/多会话） |
| 权限 | Windows：对光驱有独占访问权（未被其他刻录软件占用）<br>Linux：需在 `cdrom` 组或 root，且 `TEMP`/`TMPDIR` 可写 |
| 环境 | `TEMP`/`TMPDIR` 必须可写（IMAPI2FS 的暂存文件目录，否则 ISO 生成会失败） |
| 音频素材 | 44.1 kHz / 16 bit / 立体声 PCM 的 WAV，单轨 **不少于 4 秒** |

> 音频轨最短 4 秒是 CD-DA 与 IMAPI2 TAO 的共同约束；短于此的轨道会被写入器
> 补静音或被 RAW 生成器拒绝。

---

## 1. 能力表与控制台（`hw_check` 第 1、2 节）

- [ ] `Burn_GetApiVersion() == 3`
- [ ] `Burn_GetCapability()` 对 12 个能力 ID 返回 1（Windows/IMAPI2），
      且对未知 ID（如 9999）返回 0
- [ ] `ListBurningDevices()` 列出的设备路径能用于后续所有步骤
      （Windows 为卷路径如 `E:`；设备按**精确卷路径**匹配，不做"取第一个"）
- [ ] 不存在的设备路径会返回失败并给出明确原因（不会静默成功）

## 2. 设备信息与托盘（第 3、4 节）

- [ ] `Burn_GetDriveInformation()` 打印出厂商、型号、当前/支持 profile、
      介质类型、介质状态、可用扇区数、当前与请求写速
- [ ] 写入空白介质后，`mediaState` 应出现 `blank`/`appendable`，
      写入并封盘后应出现 `finalized`
- [ ] `BurnEjectTray()` 弹出托盘；`BurnCloseTray()` 收回
      （部分机型对"已关闭时再关闭"返回错误，属正常）

## 3. 多会话查询（第 5 节）

- [ ] 空白盘：`Burn_GetNextSessionBlock()` 返回 `0`
- [ ] 已写入未封盘的盘：返回一个 **> 0** 的 LBA，且等于驱动报告的
      Next Writable Address
- [ ] 已封盘/只读盘：返回负值，错误信息说明"不可追加"而非"设备不存在"

## 4. 镜像生成：plain / UDF / El Torito（第 6 节，无需介质）

- [ ] `hw_plain.iso` 生成成功，`CD001` 位于 LBA 16
- [ ] `hw_udf.iso` 生成成功；镜像中同时存在 ISO9660 与 UDF 结构
      （可挂载验证：Windows 资源管理器应显示该卷；Linux `mount -t udf`）
      —— **Windows 平台项**；Linux 上 libisofs 不支持 UDF，本步应被明确拒绝
- [ ] `hw_boot.iso` 生成成功；用 `isoinfo -d -i hw_boot.iso`（Linux）
      或虚拟机从该 ISO 启动，确认 El Torito 引导项存在且平台为 x86
- [ ] 挂载后文件内容与源目录一致

> 自动化测试已对这三类做**字节级**校验：UDF 锚点签名、El Torito 的
> BRVD/引导目录/校验和/平台字节、load-clip 是否被原样写入。此处的挂载验证
> 是对同一结果的端到端确认。

## 5. RAW PQ 镜像（第 7 节，无需介质）

- [ ] `BurnCreateRawImage()` 生成 `hw_disc.raw`
- [ ] 文件大小随轨道总长度增长；相同输入重复生成结果**字节一致**
- [ ] **Linux**：文件大小是 2448 的整数倍（RAW96R），首个程序扇区的 Q 子码
      绝对时间为 00:02:00，CRC-16 校验通过
- [ ] **Windows**：文件是 IMAPI2 的 PQ 容器（首个 2 秒 lead-in 为全零），
      不可与 Linux 生成的镜像互换烧录
- [ ] 用第三方工具（EAC / `cdrdao` / `readcd` 对照）确认该镜像可被识别为
      CD 镜像并显示正确 TOC（曲目数、起始 LBA）

> 自动化测试已覆盖：可复现性、随轨道长度增长、内容随采样变化、非法 MCN 被
> 拒绝；Linux 侧还校验了 Q 子码的 CRC、轨迹/索引与绝对时间。第三方 TOC 对比
> 需要人工执行。

## 6. 擦除 / 格式化（第 8a 节）

- [ ] 放入 **CD-RW**（或 DVD+RW / DVD-RAM / BD-RE），`BurnEraseDisc(dev, 0)`
      快速擦除成功
- [ ] `--destructive` 下也可测 `eraseFull=1` 全擦除：耗时明显更长且成功
- [ ] 放入 **CD-R**（不可擦写）时返回失败，错误信息说明介质不可擦写，
      **不得**报告成功

## 7. 数据盘刻录（第 8b 节）

- [ ] 用空白 CD-R 刻录 `hw_plain.iso`，任务状态走到 `COMPLETED`
- [ ] 刻录后挂载，文件内容与源一致
- [ ] `finalizeDisc=0`（`leaveOpen=1`）时刻录后盘仍可追加；
      `leaveOpen=0` 时驱动器报告 `finalized`
- [ ] 人为中断（拔盘/占用驱动器）时应失败并给出 HRESULT 与设备路径

## 8. 音频 CD（第 8c 节）

- [ ] 用空白 CD-R 刻录音频轨，任务 `COMPLETED`
- [ ] 用普通 CD 机/播放器播放：曲目数正确、**播放速度正常**（不变调）、
      左右声道正确、轨间有约 2 秒间隔
- [ ] 用 `cdparanoia`/EAC 抓轨，与源 WAV 逐样本比对一致
- [ ] 故意传入 22.05 kHz 或单声道 WAV：必须在**开始刻录前**被拒绝并说明
      需要 44.1 kHz / 2ch / 16 bit

## 9. RAW 刻录（第 8d 节）

- [ ] 用空白 CD-R 把 `hw_disc.raw` 写入，任务 `COMPLETED`
- [ ] 播放/抓轨结果与第 8 节音频盘一致（RAW 与 TAO 应产出等效音频内容）
- [ ] 在不支持 PQ 子码的驱动器上，必须在 `PrepareMedia` 之前明确拒绝，
      并在错误信息中指出"该驱动器不支持 PQ 子码 RAW 写入"

## 10. 刻后回读校验（第 9 节）

- [ ] 对刚刻好的数据盘执行 `Burn_VerifyWrite(dev, hw_plain.iso, QUICK)`：
      通过，并报告比较过的扇区数
- [ ] 执行 `FULL`：逐扇区比较通过（耗时随容量增长）
- [ ] 用**不同**的 ISO 去校验同一张盘：必须失败，并指出第一个不匹配的
      镜像扇区与对应盘上 LBA
- [ ] 校验过程中取消（另起线程调用取消接口）：返回取消错误，不崩溃
- [ ] 校验过程中弹出光驱：返回可诊断的读失败，指出失败的 LBA

## 11. 多会话追加（第 10 节）

- [ ] 先用 `leaveOpen=1` 刻一次数据盘
- [ ] `Burn_GetNextSessionBlock()` 返回 > 0 的 LBA
- [ ] `BurnAppendSession(源目录, dev, 标签, 0, 0, 0)`：追加成功
- [ ] 挂载验证：**最后一个会话**的文件可见（并在支持的系统上可见前一会话）
- [ ] 对已封盘的盘追加：必须失败并说明"已封盘/不可追加"

## 12. 边界与错误路径（每台机器都跑）

- [ ] 任意写入步骤中对任务调用取消：任务进入 `CANCELLED`，驱动器回到空闲，
      半成品文件（生成的 ISO）被删除
- [ ] 设备被其他程序独占时：错误信息包含 HRESULT 与"被占用/写保护"提示
- [ ] 介质与请求不匹配（如 DVD-R 上请求 CD 音频）：明确失败，不写坏盘
- [ ] 重复 `Burn_Release()`：第二次返回 `BURN_ERROR_INVALID_HANDLE`
- [ ] 进程结束不残留临时 ISO（DIRECTORY/APPEND 模式会建临时镜像）

---

## 结果记录

| 节 | 项目 | 结果 | 备注（机型/介质/HRESULT） |
|---|---|---|---|
| 1 | 能力表 | | |
| 4 | UDF 挂载 | | |
| 8 | 音频播放/抓轨 | | |
| 10 | FULL 校验 | | |
| 11 | 多会话追加 | | |

> 记录时请附上 `hw_check` 的完整输出；失败项请保留 `GetLastErrorMessage()`
> 文本与驱动器型号，这两项是定位问题的最短路径。
