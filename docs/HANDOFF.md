# virgl-video-windows 交接文档（Windows Guest 侧）

> 更新：2026-10-15 晚 · L0 M1b-p0.4 已部署验证，L1→L2→L3 待推进
> 定位：给"下一个接手的人"（或下一个会话的自己）。事实以本文 + `docs/WORKLOG.md`（W1–W12 全量日志）为准。

---

## 0. 当前开发纪律（强制遵守）

### 0.1 先静态，后动态

**任何 PS1 测试脚本的修改，必须先完成以下三项静态核对，全部通过后方可运行：**

1. **IOCTL 码核对**：脚本中的 IOCTL 常量必须与 `vgl/include/vgl/vgl_transport.h` 中的 `CTL_CODE` 宏一致（function<<2 后与 DeviceType 重叠位被吸收，0x8000 不可手算）
2. **缓冲区大小核对**：每个 IOCTL 的输入/输出缓冲区大小必须 ≥ 内核 handler 的最小要求（GET_INFO≥36B, SUBMIT≥40B, REAP≥424B, WAIT≥8B）
3. **结构体字段偏移核对**：脚本按偏移读取的字段必须与 `vgl_transport.h` 中的结构体布局一致（常见陷阱：reap_t 的 has_data 在 offset 8 不是 12；submit_t 被内核拷回输出缓冲区前端 offset 0-15，token 在 offset 8 不是 32）

**未完成以上三项核对，绝对不允许运行脚本。** 违反此纪律已导致多次蓝屏。

### 0.2 稳定性第一

- 驱动测试脚本只做数据面 IOCTL 往返（SUBMIT→REAP→WAIT），**绝不执行任何驱动管理操作**（Disable-PnpDevice / Enable-PnpDevice / pnputil 安装等）
- 每次部署新驱动版本前必须：先 Disable-PnpDevice → 替换 sys → Enable-PnpDevice，绝不在驱动运行时手动改 System32\drivers 下的文件
- 版本号 DriverVer 每次 staging 必须严格递增，bump 基准是当前在机版本

### 0.3 开发节奏

- 当前阶段：先学习显卡驱动开发本体知识，梳理完 HANDOFF 后再继续编码
- 每次改驱动代码 → 出新 sys → 静态检查测试脚本 → 部署 → 运行测试，完整循环后方可进入下一步

---

## 1. 项目一句话 & 四层架构

在 KVM Windows 虚机内，把宿主 AMD 5500U 的 VCN H.264 硬件编码器透传给 RustDesk。
**宿主零改动**（host 冻结 v12.3h-r8），一切通过既有 virtio-gpu 视频协议命令通道完成。

```
┌─ L3 RustDesk hwcodec fork ── 11 行 patch（已有：hwcodec-patch/0001-*.patch）
│    编码器按名字 avcodec_find_encoder_by_name("h264_vgl") 创建
├─ L2 FFmpeg h264_vgl encoder ── vglenc.c 骨架（已有，未编入 FFmpeg）
│    NV12-only / 无 extradata / zerolatency 默认开
├─ L1 libvgl 用户态库 ── vgl/src 5 文件 + Win32 transport（已有，未编译）
│    会话状态机 + NV12 资源池(PIN→ATTACH_BACKING) + emit 抽象(G1 裁决点)
└─ L0 vgvdo 薄桥驱动（已合并进 viogpudo）── ✅ M1b-p0.4 已部署验证
     \Device\VGLVIDEO0 + 6 个 IOCTL（GET_INFO/SUBMIT/REAP/WAIT/PIN/UNPIN）
```

四层职责边界：L0 只做"管道"（IOCTL 转发 + 内存 PIN），不懂协议；
L1 持有 wire 契约（`vgl/include/vgl/vgl_wire.h` 为权威 + 硬值表单测钉死）；
L2 是 FFmpeg 标准编码器位；L3 是 RustDesk 侧最小 diff。

---

## 2. 当前状态总览

| 层 | 状态 | 关键产物 | 验证 |
|---|---|---|---|
| L0 M1a 桥接 | ✅ **完成** | viogpudo.sys（v3 起，IoCreateDriver 版） | CreateFile+GET_INFO 全绿，零死机 |
| L0 M1b-p0 数据面 | ✅ **p0.4 已部署** | vglctl.cpp SUBMIT/REAP/WAIT 已实现，double-free 已修，嵌套自旋锁已修 | SUBMIT+REAP 通道稳定不崩溃；但 REAP 返回 has_data=0（响应未到达用户态） |
| L0 M1b-p1 响应竞态 | 🔄 **根因待确认** | — | has_data=0 根因推测 2 种（R1: DPC 未触发；R2: resp_buf 未正确 DMA 映射），待加 DbgPrint 排查 |
| L0 M1b-p PIN/UNPIN | ⬜ 未实现 | — | MmProbeAndLockPages + PFN 表回填 |
| L1 libvgl | ⬜ 未编译 | vgl/src 5 文件 | CI 有 wire 单测（build-vgl.yml） |
| L2 FFmpeg | ⬜ 未构建 | vglenc.c 骨架 6KB | build-ffmpeg.yml 目前是 M2 占位 |
| L3 hwcodec | ⬜ 未应用 | 0001 patch 2065B | build-hwcodec.yml cargo check |

### 已达成的技术验证链

- 官方 viogpudo（上游 0.1.240）+ VGL 合并代码可在同一驱动共存；
- 独立 WDM 驱动对象（IoCreateDriver）承载 VGL 控制设备，与 dxgkrnl 完全隔离；
- `\\.\VGLVIDEO0` 打开 + `IOCTL_VGL_GET_INFO`（0x220000）返回正确结构（magic='VGV1'，control_queue_depth=64）；
- SUBMIT（0x220004）+ REAP（0x220008）+ WAIT（0x22000C）三个 IOCTL 通道稳定，不崩溃；
- 自签名测试驱动链路（证书+cat+testsigning）可用于部署迭代而不重启；
- CI（GitHub Actions + NuGet WDK）绿色可复现。

### has_data=0 根因分析（R1/R2 假设）

REAP 返回 has_data=0 不是崩溃，是响应数据未到达用户态。两种可能原因（优先级从高到低）：

**R1: DPC 未被触发（概率最高）**

完整调用链：`QueueBuffer` → doorbell → MSI-X 中断 → ISR → DPC。如果链路在任何一环断裂，DPC 不会执行，`g_VglDoneFlag` 保持 0，REAP 永远看不到 has_data=1。

排查：在 `VglCompleteCB`（vglctl.cpp:79）加 `DbgPrint`，运行测试，看日志是否有输出。无输出 = DPC 未执行。

**R2: resp_buf 未正确 DMA 映射**

virtio-gpu host 写到 `vbuf->resp_buf` 指向的物理内存。如果 DMA 映射不正确，host 写回的数据是垃圾或全 0，`resp->type` 不会是 `0x1101`，DPC 也不会调 `complete_cb`。

排查：在 DPC（viogpudo.cpp:3849）打印 `resp->type` 和 `pvbuf->resp_buf` 值。如果 type=0 则 host 没写到有效响应。

> ⚠️ 之前的描述"DPC 先于 REAP 释放 vbuf"不精确——实际是 auto_release=false 让 DPC 不释放 vbuf，REAP 在锁外释放。问题不在生命周期管理，在 DPC 是否执行/resp_buf 是否有效。

### M1b 数据面正确模式（p0.3/p0.4 已落库）

照抄驱动自身 AskDisplayInfo 模式——`auto_release=false` + `complete_cb=VglCompleteCB`（DPC 只置完成标志）+ 响应落在**自己的 512B ExAlloc 缓冲**；REAP 在锁内只摘指针、锁外 ReleaseBuffer/ExFreePool。

### SUBMIT 缓冲区契约

输入：`[command_data(len)][vgl_tport_submit_t(16B) 位于尾部]`，内核用 `inLen - sizeof(hdr)` 定位头部；返回 token 时必须把头部结构**拷回 SystemBuffer 开头**（METHOD_BUFFERED 只拷回前 Information 字节）。

### IOCTL 码（勿手算！）

GET_INFO=0x220000 SUBMIT=**0x220004** REAP=0x220008 WAIT=0x22000C（function 字段<<2；0x8000 越界 12-bit 导致位重叠，PS 与内核/用户态必须共用 vgl_transport.h 宏）

---

## 3. 环境与资产清单

| 项 | 值 / 位置 |
|---|---|
| 测试 VM | Windows Server 2016，virtio-gpu 显卡（已从 QXL 切回），Z: = `\\192.168.1.4\Public` |
| 工作仓 | `Z:\github\virgl-video-windows`，远端 main=`9534203`（haol666/virgl-video-windows） |
| 在机驱动 | oem0.inf（p0.4, DriverVer 10/15/2026,100.6.101.58003），设备 OK，服务 Running |
| 官方驱动备份 | `C:\Windows\System32\drivers\viogpudo.sys.bak`（107528B，MS WHCP 签名）— **勿删** |
| BCD | testsigning=Yes, nointegritychecks=Yes, isolatedcontext=No（都已生效，保持不动） |
| 测试签名证书 | CN=VirGlVideoTest（thumbprint 2DD2BFEC…），PFX=`Z:\soft\.temp\deploy\viogpudo.pfx`（口令 test1234），已入 Root/TrustedPeople/TrustedPublisher |
| ⚠️ 废弃证书 | CN=viogpudo-test（CD6EE0B5…）— 曾用于 M1b-p0.1 部署但未入信任存储，验证报 UnknownError，**已废弃** |
| WDK 工具链 | NuGet：`C:\Users\Administrator\.nuget\packages\` 下 `microsoft.windows.wdk.x64\10.0.28000.2526` 与 `microsoft.windows.sdk.cpp*\10.0.28000.1721` |
| inf2cat | WDK 包 `c\bin\10.0.28000.0\x86\Inf2Cat.exe`（注意 x86 目录） |
| signtool | SDK 包 `c\bin\10.0.28000.0\x64\signtool.exe` |
| CI 入口 | workflow 数值 ID **382795312**（build-viogpu-baseline），必须 workflow_dispatch 触发 |
| 测试脚本 | `Z:\soft\.temp\test_vgl_m1bp0_v2.ps1`（前置检查+GET_INFO+错误路径+SUBMIT/REAP+WAIT，5 阶段） |
| git push | **不可用**（直连/代理全超时），唯一通路 GitHub REST API（token 在 git 配置 http.extraheader，流程见 §5.2） |
| vglctl.tmh | WPP 跟踪 GUID `{D6B96B2C-72BF-4CA5-BB89-9FCA5C82F020}`（logman 抓，但死锁型死机时缓冲不落盘，勿依赖） |
| 页面文件 | 已扩到 10237 MB（原 1600 MB 太小，完整 dump 需要 RAM+2MB） |
| CrashDumpEnabled | 7（完整转储） |

---

## 4. VM 上的驱动包现状（DriverStore）

| 包 | 内容 | 备注 |
|---|---|---|
| oem0.inf | p0.4（58003，在机运行）| **当前生效** |
| oem16.inf | 旧测试版本残留 | 建议下次迭代顺手 `pnputil /delete-driver oem16.inf /force` 清掉 |
| oem12 / oem14 | 更早版本残留 | 同上 |

**注意**：PnP API 报 DEVPKEY DriverVersion=58002/InfName=oem2.inf（陈旧元数据，oem2.inf 文件不存在），不影响在机运行镜像的判断——加载镜像铁证链：System32 文件 mtime 未变+哈希=staging → 唯一一次后续启动从它加载且服务 RUNNING。

服务注册：`HKLM\SYSTEM\CurrentControlSet\Services\VioGpuDod` → ImagePath=`\SystemRoot\system32\DRIVERS\viogpudo.sys`，Start=3（demand）。

---

## 5. 标准操作流（SOP）

### 5.1 改驱动代码 → 出新 sys

```
编辑 Z:\github\virgl-video-windows\third_party\viogpu\viogpudo\
  ├─ viogpudo.h / viogpudo.cpp   （显示驱动本体 + VGL 挂接点）
  ├─ vglctl.cpp                  （VGL 桥全部逻辑——M1b 主战场）
  ├─ driver.cpp                  （DriverEntry: DxgkInitialize 后调 VglInit()）
  └─ viogpudo.vcxproj            （纯 WDM，勿再加任何 KMDF/wdf 引用）
git commit → REST API 推送（§5.2）→ dispatch CI 382795312 → 等 ~5 分钟 → 下载 artifact
```

### 5.2 REST API 推送（git push 三路全废，这是唯一通路）

1. token 从 `git config --get-all http.extraheader` 取（格式 `Authorization: token ghp_...`），**不要在输出/日志里回显完整值**；
2. 每个变更文件：`git ls-tree <commit> -- <path>` 取 blob SHA → `cmd /c "git cat-file blob <sha> > b.bin"`（**必须 cmd 直出**，PowerShell 重定向会毁内容）→ base64 `POST /git/blobs`（远端返回 SHA 必须与本地一致=字节级验证）；
3. `POST /git/trees`（`base_tree`=远端 HEAD 树，只传变更 entry）→ 得到树必须与本地 `git rev-parse <commit>^{tree}` 一致；
4. `POST /git/commits`（**tree 用完整 40 位 SHA**，传 7 位会 422）→ `PUT /git/refs/heads/main`（force=true）；
5. `POST /actions/workflows/382795312/dispatches`（`{"ref":"main"}`）。
坑账：Git Data API 更新 ref 不产生 push 事件 → 必须 dispatch；Trees API 不能 `sha:null` 删文件（要另走 Contents API DELETE）。

### 5.3 部署新 sys 到 VM（无重启）

```
1. staging：复制新 sys + deploy INF（DriverVer 日期=今天，不得晚于系统日期！版本号 bump）
2. inf2cat /driver:<dir> /os:Server6_3_X64  （INF 需含 CatalogFile=viogpudo.cat）
3. signtool sign /f viogpudo.pfx /p test1234 /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 <cat>
4. ★ 预防性：先 Disable-PnpDevice（释放文件锁）
5. pnputil /add-driver <inf> /install    （自动换装 System32\drivers 的 sys）
6. Enable-PnpDevice → 检查 Get-PnpDevice + Get-Service VioGpuDod
7. 跑 test_vgl_m1bp0_v2.ps1
```

### 5.4 测试脚本运行 SOP（强制遵守）

**每次修改 PS1 测试脚本后，必须按以下顺序执行：**

1. **静态检查**（三项，缺一不可）：
   - IOCTL 码核对：脚本值 vs `vgl_transport.h` 的 `CTL_CODE` 宏
   - 缓冲区大小核对：脚本值 vs 内核 handler 的最小要求
   - 结构体偏移核对：脚本读取偏移 vs `vgl_transport.h` 结构体布局
2. **前置检查**（脚本内置 5 项）：服务 RUNNING、设备 OK、页面文件足够、System32 哈希与 staging 一致、无未决重启
3. **后台运行**：`Start-Process powershell -File <script> -RedirectStandardOutput <log> -RedirectStandardError <err>`，轮询 25 秒
4. **检查系统健康**：运行后检查 VioGpuDod 服务状态和设备状态，确认未重启
5. **分析结果**：如果测试中途退出且系统重启了，查 EventID 41 时间线定位原因

**禁止**：未完成静态检查就运行；在驱动运行时手动改 System32\drivers 下的文件；脚本中用 `0x80000000` 作为 [uint32] 参数（会负数溢出）。

### 5.5 出事回滚（驱动又死机/起不来）

症状分级处理：
- **只是设备起不来**：切 QXL 显卡进系统（宿主侧改 XML 或 virt-manager）→ 系统起来后设备是 CM_PROB_DISABLED/PHANTOM 无害 → 回滚：`Disable-PnpDevice`（若见得到）→ `Copy-Item viogpudo.sys.bak → viogpudo.sys` → 重装官方包 `C:\temp\viogpu_official\viogpudo.inf`（或重新 pnputil 既有 oem 包）。
- **系统无法启动**：宿主侧把 GPU 换回 QXL（本次两次死机都是这么救回来的），进系统后按上一条恢复官方驱动，再分析。
- **测试中预防性原则**：动驱动前确认 `viogpudo.sys.bak` 存在且 107528B + WHCP 签名；大批量改动先 `Disable-PnpDevice` 再换文件。

---

## 6. 坑账本（血泪精编，按复发风险排序）

1. **KMDOD 不能用 KMDF**（W8）：`DxgkInitializeDisplayOnlyDriver` 不绑定 WdfFunctions 表 → 任何 `Wdf*` 调用跳零值表 BSOD。判别法：看 sys 导入表有无 WDFLDR.SYS（真 KMDF 驱动都有；微软自家 KMDOD 都没有）。**viogpudo.vcxproj 里永远不要再加 wdf 库/头。**
2. **不许碰 dxgkrnl 的 DRIVER_OBJECT 分发表**（W9）：挂钩+转发与显示路径 IRP 竞争 → BugcheckCode=0 硬挂死（两次，无 dump 无 ETL）。VGL 一切逻辑必须在 `IoCreateDriver` 的独立驱动对象里。
3. **CTL_CODE 不许手算**（W10）：function 字段只有 12 bit（私有区 0x800-0xFFF），`0x22<<16 | 0x8000<<2` 位重叠得 0x220000 而非直觉的 0x240000。用户态/内核态共用 `vgl/vgl_transport.h` 宏。Win32 error 1 同时映射 NOT_IMPLEMENTED 和 INVALID_DEVICE_REQUEST，注意甄别。
4. **IoCreateDriver 在 WDK 10.0.28000 头文件声明被删**但 ntoskrnl.lib 一直导出：本地 `extern "C" NTKERNELAPI` 补原型（C++ TU 不加 extern "C" 会 mangling → LNK2019）。
5. **PE checksum 是伪问题**：官方签名驱动也带 0x00100000 占位值，demand-start 驱动内核不校验。别再修。
6. **inf2cat DriverVer 日期不得晚于系统当天**（22.9.7 报错）。
7. **PS1 测试脚本**：GENERIC_READ|GENERIC_WRITE 相或会 int32 溢出（用 uint32 字面量）；Win32 路径 `\\.\VGLVIDEO0` 只需单层转义。
8. **PowerShell 管道重定向会毁二进制**：取 git blob 一律 `cmd /c` 直出。
9. **GitHub Actions**：日志 zip 里找 `0_build.txt`；artifact 下载走 runs/{id}/artifacts；workflow dispatch 必须用数值 ID。
10. **死锁型死机抓不到 WPP 日志**（缓冲不 flush）：定位靠系统事件 41/6008 时间线 + 代码审查；需要更硬证据时上 WinDbg KDNET 双机。
11. **DriverStore INF 会被系统进程锁定**、pnputil 无 cat 时在 test-signing 下会弹 UAC 且超时假死——有 cat + 签名后一切正常。
12. **大文件 multiedit 后必须抽查行数/尾部**（W8 期间 viogpudo.cpp 被清空过一次，git checkout 恢复重放）。
13. **REAP has_data 偏移**（W12）：`vgl_tport_reap_t` 的 has_data 在 offset 8 不是 12（token=0, out_len=4, has_data=8, status=12）。读错 offset 会导致永远读不到 has_data=1，REAP 无限重试。
14. **SUBMIT token 拷回前端**（p0.2 修复）：内核把 submit_t 拷回输出缓冲区**前端**（offset 0-15），不是尾部。token 在 offset 8 不是 32。
15. **MAX_INLINE_RESP_SIZE=24**：virtio-gpu 缓冲池内联响应上限仅 24 字节。>24 字节响应均为动态分配，ReleaseBuffer 后 resp_buf=NULL。所有有意义的控制命令响应（GET_DISPLAY_INFO=408B）走外部 resp_buf。
16. **单 CPU IRQL 模型**：DPC（DISPATCH_LEVEL）在 IOCTL（PASSIVE_LEVEL）之前执行。SUBMIT 的 Kick 触发 ISR→DPC，DPC 处理并释放 vbuf 后，REAP 才运行。响应数据在 DPC 释放后丢失——这是 M1b-p1 要解决的竞态。
17. **页面文件必须足够大**：RAM~10GB 时页面文件至少 10GB+2MB 才能捕获完整 dump。之前 1600MB 导致蓝屏时无 dump 可分析。
18. **PowerShell `${label}:` 语法**：`$label:` 会被解析为驱动器引用，必须写成 `${label}:`。
19. **`0x80000000` 作为 [uint32] 负数溢出**：PowerShell 把 0x80000000 当 Int32=-2147483648，`[uint32]` 强转失败。改用十进制 2147483648。
20. **Win32 API 参数类型**：DeviceIoControl 的 ioControlCode 参数是 [uint32]，不能用 hex 字面量负数（0xDEADBEEF）。用 0x12345678 等正数。

---

## 7. 下一步任务分解（M1b → 端到端）

### 当前阶段：先定向学习显卡驱动开发本体知识

按优先级学习以下 4 个知识点（直接对应 has_data=0 排查）：

1. **Windows DPC/IRQL 模型**（最高优先）——理解为什么 DPC 在 PASSIVE 层代码返回后才执行，MSI-X 中断如何触发 DPC，ISR→DPC→PASSIVE 的完整时序
2. **virtio-gpu 协议**——理解 control virtqueue 的工作原理、buffer descriptor 如何传递物理地址、host 如何写回响应
3. **MDL / DMA 映射**——理解 `MmProbeAndLockPages` 和 scatter-gather list，排查 R2（resp_buf 是否正确映射）
4. **KMDOD 驱动对象模型**——理解 `IoCreateDriver` 的独立设备对象如何与显示设备的 DPC 基础设施交互

学习方式：读 WDK 官方文档 + virgl-video-windows 源码对照。每个知识点学完后做笔记记录在 WORKLOG.md。

### M1b-p0 数据面（✅ 已完成）

目标：SUBMIT/REAP/WAIT/PIN/UNPIN 五个 IOCTL 落地，先不接 MSI-X 中断（WAIT 轮询+超时）。
改动集中在 `vglctl.cpp`（独立驱动对象内，出事不影响显示路径——W9 教训的架构红利）：
- SUBMIT ✅：AllocCmdResp + QueueBuffer + token 表记录 outstanding
- REAP ✅：轮询 outstanding vbuf，检查 resp_buf 有效性（NULL=DPC 已释放，non-zero type=已就绪），复制响应
- WAIT ✅：`KeDelayExecutionThread` 轮询 + timeout_ms
- PIN/UNPIN ⬜：`MmProbeAndLockPages` + PFN 表回填，M1b-p1 填充
- double-free bug ✅ 已修复：DPC 在 auto_release=TRUE 时调用 ReleaseBuffer，REAP 原代码也调用 → double-free。修复：REAP 不再调用 ReleaseBuffer。
- 嵌套自旋锁 bug ✅ 已修复（p0.4）：VglDropOutstanding 在持有 g_VglTokenLock 时调 ReleaseBuffer→FreeBuf（内部取 m_SpinLock）= 同核嵌套自旋锁 → 0xCE。修复：锁内摘链锁外释放。

### M1b-p1 响应竞态（🔄 根因待确认）

**核心原则：先定根因，再选方案。不要在根因不明时选架构方案。**

#### Step 1: 根因确认（1 个单元，最高优先）

在 vglctl.cpp 和 viogpudo.cpp 中加 DbgPrint，运行测试，确认 R1 还是 R2：

- **R1 排查**：在 `VglCompleteCB`（vglctl.cpp:79）加 `DbgPrint("VglCompleteCB called, token=%lu\n", token)`。运行 test_vgl_m1bp0_v2.ps1，看日志是否有输出。
  - 有输出 → DPC 执行了，问题在 R2（resp_buf 未正确 DMA 映射）
  - 无输出 → DPC 未执行，问题在 R1（MSI-X 中断链路断裂）

- **R2 排查**：在 DPC（viogpudo.cpp:3849）加 `DbgPrint("DPC resp type=%x, resp_buf=%p\n", resp->type, pvbuf->resp_buf)`。
  - type=0x1101 → host 正确写回，问题在别处
  - type=0 或 type>=0x1100 → host 没写到有效响应

#### Step 2: 根据根因选方案（1-2 个单元）

| 根因 | 推荐方案 | 改动范围 |
|------|---------|---------|
| R1 (DPC 不触发) | 方案①独立 VirtIODevice + 独立控制 virtqueue | 大——需理解 viogpu 如何共享 VirtIODevice 实例 |
| R1 (DPC 不触发) | 方案②PASSIVE 层轮询 flag | 小——在 SUBMIT 后用 `KeDelayExecutionThread` 轮询 flag，不依赖 DPC |
| R2 (resp_buf DMA 问题) | 修复 DMA 映射 | 小——检查 AllocCmdResp 的 resp_buf 是否正确传递给 GetBuf |

> 方案③（auto_release=FALSE+REAP 管生命周期）已实现，不是解决方案。

#### Step 3: 验证（1 个单元）

- 跑 test_vgl_m1bp0_v2.ps1，确认 has_data=1 且响应类型=0x1101
- 多轮 SUBMIT→REAP 确认稳定性

### M1b-i 中断版（性能优化，后做）

MSI-X vector 映射 + event/queue kick + WAIT 睡事件而非轮询。依赖 M1b-p 的正确性先行。

### L1 libvgl 编译（可与 M1b-p1 并行）

vgl/src 五文件 MSVC/CMake 出 lib + `vgl_transport_win.c` 走 `\\.\VGLVIDEO0` GET_INFO 冒烟（驱动已就位，不需要 SUBMIT/REAP round-trip）。

### L2 FFmpeg（依赖 L1 头 + M1b-p1 数据面）

vglenc.c 实装 encode2（frame→NV12 资源 PIN→SUBMIT 命令→REAP 码流）；FFmpeg 7.x Windows 全量构建（外部 encoder 无法插件化，需编入 libavcodec）；CI 的 build-ffmpeg.yml 从占位转正式。

### L3 RustDesk（最后）

apply hwcodec-patch/0001 → RustDesk Windows 构建 → 端到端会话。hwcodec 的 `available_encoders()` 是"候选列表 + 720p 实帧探测（key==1+超时）"，列表加 h264_vgl 一项即自动探测——所以 **M1b-p1+L1+L2 真通之前，RustDesk 是测不了的**。

### 关键路径与时间估算（每单元≈半个工作日，含部署测试循环）

```
学习DPC/IRQL+DMA(2单元) ‖ L1编译(1单元)
  ↓
根因确认(1单元) → M1b-p1实现(1-2单元)
  ↓
L2(2单元) → 【里程碑A: ffmpeg -i test.yuv -c:v h264_vgl out.h264 成功】
  → L3(1-2 单元) → 【里程碑B: RustDesk 会话走硬件编码】
```
- 里程碑 A（FFmpeg 编码出帧）：净剩 ~6-8 单元 ≈ **3-4 个工作日**（全职节奏）；若 M1b 遇内核态新坑则顺延；
- 里程碑 B（RustDesk 端到端）：A 之后再 1-2 天（RustDesk Windows 全量构建本身 1-2 小时/次）。
- 并行机会：L1 编译可与 M1b-p1 根因确认并行——L1 只需要 GET_INFO（已通），不需要 SUBMIT/REAP round-trip。
- 最大风险项：M1b-p 与显示路径共享 VirtIODevice 的并发正确性（W8/W9 都是内核态结构性踩坑，这里同类风险最高）。缓解：polling 优先、全部代码困在独立驱动对象内、PIN 走标准 MDL API、每步 Enable 后立即跑稳定性脚本。

---

## 8. 附录：路径速查

```
Z:\github\virgl-video-windows\
├─ docs\WORKLOG.md                  # W1-W12 全量工作日志（每个坑的原始现场）
├─ docs\WINDOWS_IMPL_DESIGN.md      # 设计文档 v1.1（301 行，host 契约冻结 v12.3h-r8）
├─ docs\HANDOFF.md                  # 本文档
├─ vgl\include\vgl\                 # L1 公共头（vgl_wire.h 权威契约 / vgl_transport.h IOCTL 契约）
├─ vgl\src\                         # libvgl 源码（待编译）
├─ ffmpeg-h264-vgl\vglenc.c         # L2 encoder 骨架
├─ hwcodec-patch\0001-*.patch       # L3 RustDesk 补丁
├─ third_party\viogpu\viogpudo\     # L0 驱动（viogpudo.cpp 4100+ 行 + vglctl.cpp 264 行）
│   ├─ vglctl.cpp                   # M1b 全部数据面实现（SUBMIT L174, REAP L274, WAIT L346, GET_INFO L155）
│   └─ viogpudo.cpp                 # DPC 消费循环 L3845-3875（auto_release/complete_cb 契约参考）
└─ .github\workflows\build-viogpu-baseline.yml   # CI（dispatch ID 382795312）

Z:\soft\.temp\
├─ artifact_l1b\Release\{probe_win.exe, vgl.lib}  # L1 CI artifact
├─ stage_p04\                        # p0.4 部署包（58003 三件套：inf+sys+cat）
├─ test_vgl_m1bp0_v2.ps1             # M1b-p0 冒烟测试（5 阶段：前置+GET_INFO+错误路径+SUBMIT/REAP+WAIT）
├─ test_vgl_m1bp0_clean.ps1          # 早期简化版（已弃用）
├─ deploy\{inf,cat,pfx}              # 签名资产 + 官方 INF 基线
└─ push_simple.py                    # 早期推送脚本（现流程见 §5.2，脚本可参考）

C:\temp\viogpu_official\            # 官方恢复包（INF+sys，应急用）
C:\Windows\System32\drivers\viogpudo.sys.bak   # 官方 sys 备份（107528B）
```