# virgl-video-windows 交接文档（Windows Guest 侧）

> 更新：2026-10-15 · L0 桥接层 M1a 完成（W10 全链路验证通过）
> 定位：给"下一个接手的人"（或下一个会话的自己）。事实以本文 + `docs/WORKLOG.md`（W1–W10 全量日志）为准。

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
└─ L0 vgvdo 薄桥驱动（已合并进 viogpudo）── ✅ M1a-v3 已部署验证
     \Device\VGLVIDEO0 + 6 个 IOCTL（GET_INFO 已通；数据面 M1b）
```

四层职责边界：L0 只做"管道"（IOCTL 转发 + 内存 PIN），不懂协议；
L1 持有 wire 契约（`vgl/include/vgl/vgl_wire.h` 为权威 + 硬值表单测钉死）；
L2 是 FFmpeg 标准编码器位；L3 是 RustDesk 侧最小 diff。

## 2. 当前状态总览

| 层 | 状态 | 关键产物 | 验证 |
|---|---|---|---|
| L0 M1a 桥接 | ✅ **完成** | viogpudo.sys 107520B（v3，在机运行） | CreateFile+GET_INFO 全绿，零死机 |
| L0 M1b 数据面 | ⬜ 未开始 | vglctl.cpp 占位（返回 NOT_IMPLEMENTED） | — |
| L1 libvgl | ⬜ 未编译 | vgl/src 5 文件 | CI 有 wire 单测（build-vgl.yml） |
| L2 FFmpeg | ⬜ 未构建 | vglenc.c 骨架 6KB | build-ffmpeg.yml 目前是 M2 占位 |
| L3 hwcodec | ⬜ 未应用 | 0001 patch 2065B | build-hwcodec.yml cargo check |

### 已达成的技术验证链

- 官方 viogpudo（上游 0.1.240）+ VGL 合并代码可在同一驱动共存；
- 独立 WDM 驱动对象（IoCreateDriver）承载 VGL 控制设备，与 dxgkrnl 完全隔离；
- `\\.\VGLVIDEO0` 打开 + `IOCTL_VGL_GET_INFO`（0x220000）返回正确结构（magic='VGV1'，control_queue_depth=64）；
- 自签名测试驱动链路（证书+cat+testsigning）可用于部署迭代而不重启；
- CI（GitHub Actions + NuGet WDK）绿色可复现。

## 3. 环境与资产清单

| 项 | 值 / 位置 |
|---|---|
| 测试 VM | Windows Server 2016，virtio-gpu 显卡（已从 QXL 切回），Z: = `\\192.168.1.4\Public` |
| 工作仓 | `Z:\github\virgl-video-windows`，远端 main=`ced1c56`（haol666/virgl-video-windows） |
| 在机驱动 | oem16.inf（v3, DriverVer 10/14/2026,100.110.102.55003），设备 OK，服务 Running |
| 官方驱动备份 | `C:\Windows\System32\drivers\viogpudo.sys.bak`（107528B，MS WHCP 签名）— **勿删** |
| BCD | testsigning=Yes, nointegritychecks=Yes, isolatedcontext=No（都已生效，保持不动） |
| 测试签名证书 | CN=VirGlVideoTest，PFX=`Z:\soft\.temp\deploy\viogpudo.pfx`（口令 test1234），已入 Root/TrustedPeople/TrustedPublisher |
| WDK 工具链 | NuGet：`C:\Users\Administrator\.nuget\packages\` 下 `microsoft.windows.wdk.x64\10.0.28000.2526` 与 `microsoft.windows.sdk.cpp*\10.0.28000.1721` |
| inf2cat | WDK 包 `c\bin\10.0.28000.0\x86\Inf2Cat.exe`（注意 x86 目录） |
| signtool | SDK 包 `c\bin\10.0.28000.0\x64\signtool.exe` |
| CI 入口 | workflow 数值 ID **382795312**（build-viogpu-baseline），必须 workflow_dispatch 触发 |
| 测试脚本 | `Z:\soft\.temp\test_vgl.ps1`（CreateFile+GET_INFO，IOCTL=0x220000 勿改手算值） |
| git push | **不可用**（直连/代理全超时），唯一通路 GitHub REST API（token 在 git 配置 http.extraheader，流程见 §5.2） |
| vglctl.tmh | WPP 跟踪 GUID `{D6B96B2C-72BF-4CA5-BB89-9FCA5C82F020}`（logman 抓，但死锁型死机时缓冲不落盘，勿依赖） |

## 4. VM 上的驱动包现状（DriverStore）

| 包 | 内容 | 备注 |
|---|---|---|
| oem16.inf | v3（IoCreateDriver 版）| **当前生效** |
| oem12 / oem14 | 旧测试版本残留 | 建议下次迭代顺手 `pnputil /delete-driver oemX.inf /force` 清掉 |

服务注册：`HKLM\SYSTEM\CurrentControlSet\Services\VioGpuDod` → ImagePath=`\SystemRoot\system32\DRIVERS\viogpudo.sys`，Start=3（demand）。

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
5. `POST /actions/workflows/382795312/dispatches`（`{"ref":"main"}）`）。
坑账：Git Data API 更新 ref 不产生 push 事件 → 必须 dispatch；Trees API 不能 `sha:null` 删文件（要另走 Contents API DELETE）。

### 5.3 部署新 sys 到 VM（无重启）

```
1. staging：复制新 sys + deploy INF（DriverVer 日期=今天，不得晚于系统日期！版本号 bump）
2. inf2cat /driver:<dir> /os:Server6_3_X64  （INF 需含 CatalogFile=viogpudo.cat）
3. signtool sign /f viogpudo.pfx /p test1234 /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 <cat>
4. 设备若运行中：Disable-PnpDevice（释放文件锁）
5. pnputil /add-driver <inf> /install    （自动换装 System32\drivers 的 sys）
6. Enable-PnpDevice → 检查 Get-PnpDevice + Get-Service VioGpuDod
7. 跑 test_vgl.ps1
```

### 5.4 出事回滚（驱动又死机/起不来）

症状分级处理：
- **只是设备起不来**：切 QXL 显卡进系统（宿主侧改 XML 或 virt-manager）→ 系统起来后设备是 CM_PROB_DISABLED/PHANTOM 无害 → 回滚：`Disable-PnpDevice`（若见得到）→ `Copy-Item viogpudo.sys.bak → viogpudo.sys` → 重装官方包 `C:\temp\viogpu_official\viogpudo.inf`（或重新 pnputil 既有 oem 包）。
- **系统无法启动**：宿主侧把 GPU 换回 QXL（本次两次死机都是这么救回来的），进系统后按上一条恢复官方驱动，再分析。
- **测试中预防性原则**：动驱动前确认 `viogpudo.sys.bak` 存在且 107528B + WHCP 签名；大批量改动先 `Disable-PnpDevice` 再换文件。

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

## 7. 下一步任务分解（M1b → 端到端）

### M1b-p（polling 版数据面）— 建议先做
目标：SUBMIT/REAP/WAIT/PIN/UNPIN 五个 IOCTL 落地，**先不接 MSI-X 中断**（WAIT 轮询+超时）。
改动集中在 `vglctl.cpp`（独立驱动对象内，出事不影响显示路径——W9 教训的架构红利）：
- SUBMIT：从 VioGpuAdapter 借用既有 VirtIODevice 控制 virtqueue，发 virtio-gpu 视频协议命令（wire 契约见 `vgl_wire.h`），token 表记录 outstanding；
- REAP：轮询 used ring 按 token 取回；
- WAIT：`KeDelayExecutionThread` 轮询 + timeout_ms（msix=0 时 libvgl 走轮询路径——`notify_cap=0` 当前就为此留的位）；
- PIN/UNPIN：`MmProbeAndLockPages`（UserMode，IoReadAccess|IoWriteAccess）+ PFN 表回填（`vgl_tport_pin_t`），句柄表管理；
- 并发要点：与显示路径共享 VirtIODevice → 引用 VioGpuAdapter 已有的队列锁；IOCTL 分发保持串行（sequential 语义用自建 FAST_MUTEX 或 per-device 队列锁）。
验收：test 脚本扩展 PIN+SUBMIT+REAP 一次 echo 命令（virtio-gpu 协议 GET_CAPS 类无害命令）往返成功，系统稳定。

### M1b-i（中断版）— 性能优化，后做
MSI-X vector 映射 + event/queue kick + WAIT 睡事件而非轮询。依赖 M1b-p 的正确性先行。

### L1 libvgl 编译（可与 M1b-p 并行）
vgl/src 五文件 MSVC/CMake 出 lib + `vgl_transport_win.c` 走 `\\.\VGLVIDEO0` GET_INFO 冒烟（驱动已就位，今天就能跑）。

### L2 FFmpeg（依赖 L1 头 + M1b-p 数据面）
vglenc.c 实装 encode2（frame→NV12 资源 PIN→SUBMIT 命令→REAP 码流）；FFmpeg 7.x Windows 全量构建（外部 encoder 无法插件化，需编入 libavcodec）；CI 的 build-ffmpeg.yml 从占位转正式。

### L3 RustDesk（最后）
apply hwcodec-patch/0001 → RustDesk Windows 构建 → 端到端会话。hwcodec 的 `available_encoders()` 是"候选列表 + 720p 实帧探测（key==1+超时）"，列表加 h264_vgl 一项即自动探测——所以 **M1b-p+L1+L2 真通之前，RustDesk 是测不了的**。

### 关键路径与时间估算（每单元≈半个工作日，含部署测试循环）

```
M1b-p(3-4 单元) → L1(1) → L2(2) → 【里程碑A: ffmpeg -i test.yuv -c:v h264_vgl out.h264 成功】
                                → L3(1-2 单元) → 【里程碑B: RustDesk 会话走硬件编码】
```
- 里程碑 A（FFmpeg 编码出帧）：净剩 ~6-8 单元 ≈ **3-4 个工作日**（全职节奏）；若 M1b 遇内核态新坑则顺延；
- 里程碑 B（RustDesk 端到端）：A 之后再 1-2 天（RustDesk Windows 全量构建本身 1-2 小时/次）。
- 最大风险项：M1b-p 与显示路径共享 VirtIODevice 的并发正确性（W8/W9 都是内核态结构性踩坑，这里同类风险最高）。缓解：polling 优先、全部代码困在独立驱动对象内、PIN 走标准 MDL API、每步 Enable 后立即跑稳定性脚本。

## 8. 附录：路径速查

```
Z:\github\virgl-video-windows\
├─ docs\WORKLOG.md                  # W1-W10 全量工作日志（每个坑的原始现场）
├─ docs\WINDOWS_IMPL_DESIGN.md      # 设计文档 v1.1（301 行，host 契约冻结 v12.3h-r8）
├─ docs\HANDOFF.md                  # 本文档
├─ vgl\include\vgl\                 # L1 公共头（vgl_wire.h 权威契约 / vgl_transport.h IOCTL 契约）
├─ vgl\src\                         # libvgl 源码（待编译）
├─ ffmpeg-h264-vgl\vglenc.c         # L2 encoder 骨架
├─ hwcodec-patch\0001-*.patch       # L3 RustDesk 补丁
├─ third_party\viogpu\viogpudo\     # L0 驱动（viogpudo.cpp 4100+ 行 + vglctl.cpp 264 行）
└─ .github\workflows\build-viogpu-baseline.yml   # CI（dispatch ID 382795312）

Z:\soft\.temp\
├─ artifact\viogpudo.sys            # 最新 CI 产物（v3=107520B）
├─ deploy\{inf,cat,pfx}             # 签名资产 + 官方 INF 基线
├─ stage_v3\                        # v3 部署包样例（SOP 模板）
├─ test_vgl.ps1                     # 冒烟测试
└─ push_simple.py                   # 早期推送脚本（现流程见 §5.2，脚本可参考）

C:\temp\viogpu_official\            # 官方恢复包（INF+sys，应急用）
C:\Windows\System32\drivers\viogpudo.sys.bak   # 官方 sys 备份（107528B）
```
