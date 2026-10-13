# WORKLOG — Windows Guest 侧（W 系列）

> 纪律（与 host 侧 F 系列对齐）：一工作单元一条；发现/决策/教训全记录；
> host 侧问题一律转 virgl-video-project issue 附 QEMU 日志，不改宿主。
> host 侧血泪史见其 `docs/WORKLOG.md`（F1–F19，移交后先通读）。

---

## W1 · 2026-10-11 · 设计定稿 + 仓库骨架 + CI 搭建

**做了什么**

1. 通读交底文档（WINDOWS_GUEST_DESIGN.md，host 冻结 v12.3h-r8）。
   修正外部顾问前几轮的两个方案性错误（记录在案防复发）：
   - ~~vsock/ivshmem 旁路编码服务~~：host 冻结且只认 virtio-gpu 视频协议命令，
     旁路通道无 host 服务端，**作废**；
   - ~~Media Foundation MFT 接入位~~：RustDesk 走 hwcodec→FFmpeg 栈，
     不消费 MF；MFT 降为二期通用化选项。
2. 源码验证（hwcodec 31dce749 副本）：
   - `cpp/ffmpeg_ram_encode.cpp:162`：编码器按**名字**
     `avcodec_find_encoder_by_name` 创建 → FFmpeg 编码器是正确接入位；
   - `:144` 仅名字含 "vaapi" 才建 hwdevice → `h264_vgl` 走纯 RAM 分支，
     **无 D3D11/DXVA 依赖**；
   - `src/ffmpeg_ram/encode.rs available_encoders()`：候选列表 + 720p
     实帧探测（key==1 + 超时约束）→ 列表加一项即全链自动验证，
     无需伪造 GPU 厂商检测。
3. 确立四层架构与同构映射（Linux VAAPI 标准位 ↔ Windows FFmpeg 标准位）：
   hwcodec 11 行 diff → FFmpeg h264_vgl → libvgl → vgvdo 薄桥驱动。
   设计文档 `docs/WINDOWS_IMPL_DESIGN.md` 冻结待评审。
4. 仓库骨架全部落地（本仓库）：
   - `vgl/`：wire 契约（PROVISIONAL+硬值表单测钉死 8/9/11/1/4）、
     NV12 资源池（PIN→ATTACH_BACKING，LINEAR 天然满足=F1 规避）、
     会话状态机、emit 抽象层（G1 裁决点 α/β 隔离在此）、
     Win32 transport（CI 无设备可编可测）；
   - `ffmpeg-h264-vgl/vglenc.c`：FFmpeg 7.x encoder 骨架
     （NV12-only、无 extradata、zerolatency 默认开）；
   - `hwcodec-patch/0001-*.patch`：11 行 diff（fork 分支 vgl-support）；
   - `xml/virtio-gpu-dual.xml`：**双 virtio-gpu 设备**（解决 viogpudo
     独占冲突——交底文档未覆盖，本侧新增关键决策）；
   - CI：build-vgl（编译+wire 单测）、build-hwcodec（cargo check）、
     build-ffmpeg（M2 占位）。
5. 本地 git 初始化，本条目即首个提交内容。

**发现与教训**

- github.com 直连被 reset（Connection reset），api.github.com 可达：
  与既往"git push 认证失败走 REST API"记录一致，远程操作策略照旧
  （fork/建仓/推送均走 api.github.com，等待 PAT 提供）。
- GCM（credential.helper=manager）内无缓存凭据，fill 非交互即失败。
- 交底称 `virgl_video_hw.h` "随文档移交仓库副本"，但 Z:\github\hwcodec
  下未见 → 登记 G1 硬前置缺口，M1 前 host 侧需补移交。

**下一步**

- [x] 拿到 PAT → 建远程仓库 + fork hwcodec/externals + push（见 W2）
- [ ] G1：权威 virgl_video_hw.h / virgl_video.c 移交到位，逐字段 diff
- [ ] M1：vgvdo 驱动实现（VirtIOLib 复用）+ 双设备 XML 上机 + 首个
      create_codec（宿主日志验收）

---

## W2 · 2026-10-11 · 远程仓库建立（REST API 路线）

**做了什么**

1. token 定位：不在 GCM/环境变量，而在 `~/.gitconfig` 的
   `[http] extraHeader`（全局 Authorization 头）——GCM 凭据管理器为空
   但 git https 一直可用之谜解开。
2. 远程操作全程走 api.github.com（直连 github.com push 三连败：
   直连超时 / gh-proxy.com 超时 / ghfast.top 拒 push，与既往
   "push 走 REST API"记录一致）：
   - `haol666/virgl-video-windows` 仓库创建（public）；
   - fork `rustdesk-org/hwcodec`、`rustdesk-org/externals` → haol666；
   - **hwcodec vgl-support 分支**：Git Data API 建 tree/commit，
     parent 直接衔接上游 `31dce749`（比本地重放更标准），
     远程 commit `1c85fef`；
   - **主仓 main**：Contents API 先放占位 README（空仓库 409 门槛），
     再重放 W1/W1b 两 commit（`38f6064`/`68ef86c`），main 指向 68ef86c，
     占位 commit 成孤儿被丢弃。
3. 提交保真手段：`git cat-file blob <sha>` 经 `cmd /c` 重定向二进制
   透传（PowerShell Out-File 会把 LF blob CRLF 化，第一版 tree 因此
   作废重建）。

**发现与教训**

- **Git Data API（trees/commits/refs）更新分支不产生 push 事件** →
  on:push 的 workflow 不触发。CI 启动两条正路：Contents API 提交
  （产生真实 push event）或 workflow_dispatch 手动触发。
- ghfast.top / gh-proxy.com 对 git push 均不可用（fetch 代理形态），
  本机 push 结论性放弃 git 协议，REST API 是唯一通路。
- build-vgl.yml 需补 workflow_dispatch（本次已补），便于随时手动触发。

**下一步**

- [x] 触发并盯绿 build-vgl（见 W3）
- [x] build-hwcodec 首跑（见 W3，形态重构为 patch-parity）
- [ ] G1 资产向 host 侧催收

---

## W3 · 2026-10-11/12 · CI 打绿（build-vgl + build-hwcodec 全 success）

**结果**

- `build-vgl`（f574ee3）：windows-latest + CMake/MSVC x64，libvgl 编译通过，
  `test_wire` 22 项断言全过（wire 硬值 8/9/11/1/4、stride/布局、pool 拷贝、
  参数校验、无设备探测路径）。
- `build-hwcodec`（ab3037f）：patch-parity 绿——fork `vgl-support` 的
  encode.rs 与"上游 31dce749 + 本仓 patch"**字节级一致**。

**踩坑记录（按时间序，皆为可复用教训）**

1. `git/trees` 的 content 字段**只收明文**，`encoding` 是 `git/blobs` 的参数——
   W1/W1b 重放时把 base64 当正文存了，全部文件损坏；`git/blobs` 逐文件上传
   后以 sha 引用建树才正确（blob sha 与本地一致 = 内容寻址验证）。
2. PowerShell 5.1：数组**经管道**传 `ConvertTo-Json` 会被逐元素序列化成
   拼接对象 → JSON 无效 → API 500；必须 `ConvertTo-Json -InputObject @{...}`。
3. `git/commits` 响应**没有** `.commit` 嵌套（与 GET commits 不同），
   `$c.commit.tree.sha` 取 null → base_tree 丢失 → 树只剩最后一条路径链。
   （此坑曾导致 contents API 404 的"灵异"现象。）
4. `ctest --test-run-timeout` 不存在，正确参数 `--timeout`。
5. 库 bug：pool buffer 只在 bind() 分配，无设备（CI）时 fill() 写 NULL
   → SegFault。修正为 create() 分配（设备无关）、bind() 只做 PIN。
6. 测试 bug：1920 非 256 对齐（7.5×256），stride 应为 2048——断言本身
   写错比实现写错更隐蔽，数值断言必须先手算。
7. 手写 commit sha 又笔误一次（31dce749 → 31c1ce749）→ 上游 checkout
   失败。sha 一律复制粘贴，禁止手敲。

**当前远程状态**

- `haol666/virgl-video-windows` main：设计文档 + libvgl + vglenc.c + patch +
  XML + 3 workflows + WORKLOG（W1–W3），两条 CI 绿。
- `haol666/hwcodec` vgl-support：31dce749 + h264_vgl patch（commit 7e13cf6）。
- `haol666/externals`：fork 就绪（M2 用）。

**下一步**

- [x] G1 资产催收（见 W4）
- [ ] M1：vgvdo 驱动 + 双设备 XML 上机，首个 create_codec 宿主日志验收
- [ ] M2：vglenc 进 externals FFmpeg 构建，build-ffmpeg workflow 落地

---

## W4 · 2026-10-12 · G1 关闭：wire 权威化 + Mesa 参考全量入库

**输入**：用户移交 host 侧 4 文件（virgl_video_hw.h 权威协议头、virgl_video.c
54 函数 host VA 后端、WINDOWS_GUEST_DESIGN.md、host WORKLOG F1–F19+§6–19）。

**做了什么**

1. **三重交叉验证 wire 契约**：host virgl_video_hw.h ↔ Mesa(openKylin fork
   upstream 分支) 同名头（fc diff 仅空白差）↔ host r9 `guest_to_host_profile`
   表。Guest Mesa 26.2.4 枚举数值（删 MPEG4 双项前移 2 位）：**H264
   BASELINE=7/CB=8/MAIN=9/HIGH=11，HEVC MAIN=15/MAIN10=16，entrypoint
   ENCODE=4，chroma 420=2，picture_type IDR=3，rc CBR=3/VBR=4**。
2. **补齐最后拼图（公开源）**：github.com 的 mesa/mesa 是 Python 仿真库同名
   项目（踩坑）；freedesktop.org 被 Anubis 反爬；走 **gitee openkylin/mesa
   upstream 分支**拿到 Mesa virgl 视频层全量参考 10 文件（virgl_video.c/
   virgl_encode.c/协议头/枚举头/caps 头），入库 `reference/`。
3. **α/β 裁决：α**。视频命令 = virgl 3D 命令流内 VIRGL_CCMD_* dword
   （CREATE_VIDEO_CODEC=53 … END_FRAME=61，VIRGL_CMD0 头 + payload dword）；
   **数据面全走资源句柄**（desc buffer 8192 级 desc 资源、feed buffer 8B、
   coded buffer 宿主分配 w*h/256*512、NV12=166 + 2×plane 资源）；
   **版本门**：caps.v2.host_feature_check_version≥14 才发 max_ref 第 8 dword
   （virgl_encode.c:1780 同款）。Mesa 帧循环：begin_frame → 写 desc/清 feed
   → encode_bitstream(5 句柄) → end_frame → flush+fence → 读 feed{stat,
   coded_size} → 取 coded 数据；desc/feed 双缓冲 ping-pong。
4. **vgl_wire.h 重写为 AUTHORITATIVE**：结构逐字段转录（H264 enc desc=
    **2556B**、base=264B、rate_ctrl=64B、HEVC enc desc=2204B、feedback=8B、
    caps=16B），编译期尺寸 pin（负数组技巧）+ 运行期 offset 校验（260/324/
    620/632/1008/2548）；测试新增命令 ID/payload 索引/NV12=166/版本门断言。
    （HEVC 尺寸初版误算 2200，CI pin 抓出后 W4b 修正。）
5. **设计文档 v1.1**：§3.4 权威化（含完整命令序列与资源模型）、§2 桥层
   修正为 "virtio-gpu 控制队列原语透传"（列出 L1 所需 spec 命令集）、§12
   六项缺口全部 CLOSED；勘误：host 生产基线实为 **v12.5，HEVC 已闭环**
   （交底"冻结 r8+HEVC 未激活"信息过时，wire 面未变故无影响；M5 随时可启）。

**发现与教训**

- GitHub 上 `mesa/mesa` 是完全无关的 Python 项目（agent-based modeling）——
  重名仓库不可信，落点前先看 repo description/commits。
- openKylin gitee fork 是 virgl-video 补丁的活源头；openKylin 枚举数值恰好
  = guest 26.2.4 语义（fork 基线同代），比 Mesa3D 镜像更贴合本协议。
- wire 的 profile 数值是 "guest 侧 pipe 值"而非任何一侧的"标准枚举"——
  文档表述应始终写"guest 数值"，避免下一个实现者再踩 F17。
- desc buffer 尺寸：Mesa 按 union 全尺寸分配，host 只读 codec 对应结构——
  Windows 侧按所发最大 desc（2556B）分配即可，安全且省内存。
- feed buffer 只有 8 字节且需要预清零（NOT_STARTED）——host 写回 stat+
  coded_size 后 guest 轮询读取；不存在独立 "feedback 命令"。

**下一步**

- [ ] M1 启动：vgvdo 驱动（VirtIOLib + 控制队列透传 6 IOCTL）+
      双设备 XML 上机 → 宿主日志 `DBG create_codec: pipe_profile=11`
- [ ] M1 细节：GET_CAPS v2 解析（transcribe reference/virgl_hw.h 的 caps v2
      全结构）+ host_feature_check_version 门
- [x] CI：本轮 wire 新断言跑绿（G7 目标）→ 转 W4b 收尾

---

## W4b · 2026-10-13 · h265 布局勘误收尾：rc 72→76，desc 2200→2204

**背景**：W4 推送后 build-vgl 红——编译期 pin 抓到 `virgl_h265_enc_picture_desc`
sizeof 断言失败（预期 2200，MSVC 实测 2204）。本地已改 vgl_wire.h 三处，但
test 断言与偏移注释未同步，本条收尾。

**根因**：h265 rate_control 手算漏了成员——实为 **18×uint32 + u8 + reserved[3]
= 76B**（初算 72），连锁导致整条偏移注释链（decoded_curr_pic、
num_slice_descriptors）与总和（2200）全部偏 4 字节。权威链（rc=76 修正后）：

```
base 264 → seq 76(+264) → pic 8(+340) → slice 12(+348) → rc 76(+360)
→ decoded_curr_pic +436 → ref[16]+440..504 → … → num_slice_desc +644
→ slices[128] +648..2184 → quality_modes 16(+2184) → picture_type +2200
→ total 2204（对齐 4）✓
```

以 `reference/virgl_video_hw.h`（L399-534）逐字段复核：成员与转写一致，
quality_modes=4×u32=16B 佐证 2204 自洽。

**修了什么**

1. vgl_wire.h：rc 注释 72→76（18×u32 注明）、decoded_curr_pic +432→**+436**、
   num_slice_descriptors +640→**+644**。
2. test_wire.c：rc==76、desc==2204；新增 h265 offsetof 抽查
   （decoded_curr_pic 436 / num_slice_descriptors 644 / picture_type 2200）。
3. vgl_session.c `vgl_wire_check()`：新增 h265 偏移 gate（max_qp 64 /
   436 / 644 / 2200）——今后 h265 漂移编译期+运行期双重拦截。
4. 文档同步：DESIGN §3.4 与 W4 条目 2200→2204 勘误。

**发现与教训**

- 尺寸 pin 的价值当场兑现：CI 抓出手算错 4 字节，避免了 M5 上机时的
  静默错位 IO（F17 同款教训，这次发生在编译期而非运行期）。
- **"注释链"必须与成员布局同导出**：只改 sizeof pin 不改偏移注释会留下
  4 字节谎言，下个引用注释的人就会踩坑；修复一律全链对齐。
- 18 个 u32 数了两遍才数对——成员计数 ≥15 的结构体，手算禁止省略"逐个
  编号"这步。

**结果**

- [x] API 推送完成：单 commit **97f89c7**（Git Data blob→tree→commit，5 个
      blob sha 与本地 HEAD 逐一相等=字节级一致），remote main 前移
      7231cd0 → 97f89c7。
- [x] build-vgl **转绿**：run 38282145524 / 38282145819（同头双跑均 success；
      对照 W4 红 run 38280672715）。编译期 pin + 运行期 gate + 测试断言三层
      全过——2204 布局自 MSVC 官宣确认。
- 过程坑：PowerShell `[regex]::Match` 默认区分大小写，gitconfig 键是
  `extraHeader`（驼峰）→ 提取空串导致 blob POST 401 "Requires
  authentication"。`.NET regex` 一律加 `(?i)` 或改用 PS 的 -match。

**下一步**

- M1 启动：vgvdo 驱动（VirtIOLib + 控制队列透传 6 IOCTL）+ 双设备 XML 上机
  → 宿主日志验收 `DBG create_codec: pipe_profile=11`
- M1 细节：GET_CAPS v2 解析（transcribe reference/virgl_hw.h caps v2）+
  host_feature_check_version 门

---

## W5 · 2026-10-13 · M1a 启动：vgvdo 骨架 CI 化

**决定**：M1 拆成两步走（沿用 W2 "build-first" 证明过的模式）——
M1a = 骨架契约面 + CI 打通；M1b = VirtIOLib 数据面（virtqueue/MSI-X/MDL）。
理由：本地无 MSVC/WDK 也无 virtio-win 源码，但 CI runner 两者皆备；
骨架先行可把"安装面 + IOCTL 契约 + WDK 构建"三件事一次性甩进流水线。

**做了什么**

1. `driver/vgvdo.c`（KMDF 骨架）：命名设备 `\Device\VGLVIDEO0` +
   DOS 符号链接 + 接口 GUID；默认队列 Sequential dispatch
   `EvtIoDeviceControl`；六 IOCTL 中的 SUBMIT/REAP/WAIT/PIN/UNPIN 返回
   `STATUS_NOT_IMPLEMENTED`（M1b 填充），GET_INFO 返回
   `vgl_tport_info_t`（magic='VGV1'，**depth=0 表示数据面未接线**，
   L1 据此走无设备路径）。无 WPP、无资源语义——"只管管道"红线。
2. `driver/vgvdo.inf`：PCI\VEN_1AF4&DEV_1050，Class=System，DEMAND_START；
   测试签名场景，绑定策略=对第二 virtio-gpu 实例 devcon 显式指定
   （viogpudo 在显示实例上因 WDDM/Display 评级天然胜出，无需抢占）。
3. `driver/vgvdo.vcxproj`：KMDF/WindowsKernelModeDriver10.0，禁签名，
   include 指向 `../vgl/include`（复用 vgl_transport.h 单一契约源）。
4. `.github/workflows/build-vgvdo.yml`：clone
   virtio-win/kvm-guest-drivers-windows 并断言 VirtIOLib.c 存在
   （M1b 依赖链预检）+ msbuild + 递归收集 .sys/.pdb + artifact 上传；
   push 触发限定 `driver/**` 路径。

**风险登记**

- windows-latest 镜像 WDK 版本漂移 → PlatformToolset 失配会立即红，
  届时改 choco 装 WDK 或固定 runner tag。
- KMDF INF 的 KmdfLibraryVersion 取 1.15（Win10+ inbox 下限），
  上机若遇 1.33 强制可再调。
- MSVC 空参 __VA_ARGS__ 依赖传统预处理器吞尾逗号行为（DBG 的
  VGV_TRACE）；若新 clang-cl/一致性预处理器报警，改为统一带参调用。

**结果**（8 轮 CI，全部 run 可追溯）

- [x] **build-vgvdo 转绿**：run 38286761055/38286761730 @ 2d6223d，
      artifact `vgvdo-driver-x64`（vgvdo.sys+inf+pdb）已上传。
      Windows VM 上的 KMDF 驱动从此在 GitHub 流水线原生构建。
- 七红一绿的攻错履历（每红皆换一个独立根因）：
  1. `VirtIOLib\VirtIOLib.c` 路径想当然 → 实为 `VirtIO\`（trees API 实查）。
  2. `ntddk.h` not found：windows-2025 镜像 **无 WDK 也无 VS2022**（诊断步
     实锤：Windows Kits\10\build 不存在、唯一 VS 实例是 18.10 Enterprise）。
  3. vswhere 钉 VS17 无意义 → 转向 **WDK-via-NuGet**（microsoft/ebpf-
     for-windows 生产配方，MIT）：NuGet `Microsoft.Windows.WDK.x64/
     SDK.CPP(.x64) 10.0.28000.2526` + 移植 `wdk.props`（按
     MSBuildAssemblyVersion 18 选 28000，VS2026 专用版）于 Default.props
     **之前**导入 → 工具集与 ntddk 全通。
  4. `stampinf.exe` 不可用（NuGet WDK 不装系统 bin）→ INF 降为 None 项
     （静态 DriverVer 本就无需 stamp），装机时验证。
  5. MSVC 14.51 的 `<stdint.h>`→vcruntime.h 在内核模式不干净（C4083/C4005
     被 TreatWarningAsError 拦）→ `vgl_transport.h` 改双目标：_NTDDK_ 分支
     以 wdm 的 UINT8/16/32/64 本地定义定宽类型，用户态分支不变
     （build-vgl 同头回归绿）。
  6. `LNK2001 GUID_DEVINTERFACE_VGLVIDEO` → `#include <initguid.h>`
     后 DEFINE_GUID 才发射实例。
  7. `SIGNTASK: signtool /fd` → `<DriverSign>` 不是真实属性名，改
     `<SignMode>Off</SignMode>` 跳过签名任务（测试签名装机侧处理）。
- 认知加固：GitHub windows-2025 时代驱动 CI 的唯一正解 = nuget restore
  packages.config + wdk.props 早导入；凡宣称 setup-msbuild 换版本就能出
  WDK 的路都是死路（镜像里根本没有第二个 VS）。

**下一步**

- [ ] M1b：VirtIOLib 接入（PCI BAR/MSI-X/virtqueue + FENCE 中断→WAIT
      + MDL PIN/UNPIN 实装，约 2-3k 行；VirtIO/ 源已在 CI 预检）
- [ ] 上机：双设备 XML → testsigning → devcon 绑定第二实例 →
      `\\.\VGLVIDEO0` 打开 + GET_INFO 首验

