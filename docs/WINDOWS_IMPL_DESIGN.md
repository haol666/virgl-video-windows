# Windows Guest 视频编码透传 — 实现设计文档（v1.1）

> 负责人：Windows 端开发（haolin）
> 依据：《WINDOWS_GUEST_DESIGN.md》交底文档 + G1 资产（virgl_video_hw.h / virgl_video.c / host WORKLOG，2026-10-12 移交）
> host 生产基线勘误：交底冻结点 r8(2f8c71ce) 之后 host 侧已演进至 v12.5（16f8e79）并闭环 HEVC —— wire 协议面未变，Windows 侧无需跟进（见 §12/G6）
> 源码参照：hwcodec 31dce749（RustDesk 官方 hwcodec）+ reference/（Mesa virgl 视频层全量参考，见 §12/G1）
> 日期：2026-10-11 初稿；2026-10-12 v1.1（G1 关闭，wire 权威化）
> 状态：设计已验证；M1 可启动（无剩余前置缺口）

---

## 0. 设计总纲（一页纸）

**目标**：Windows 虚拟机内的 RustDesk 获得 H.264 硬件编码，编码由 Unraid 宿主 AMD VCN 执行，协议命令经 virtio-gpu（virgl 通道）传输，host 侧零改动。

**四层架构**（每层可独立替换/独立编译）：

| 层 | 交付物 | 语言/形态 | 对上暴露 | 对下依赖 |
|---|---|---|---|---|
| L3 应用适配 | hwcodec patch（~30 行 diff） | Rust crate 补丁 | RustDesk（零改动） | FFmpeg `h264_vgl` |
| L2 编码框架 | FFmpeg `h264_vgl` encoder（vglenc.c） | C，链入定制 FFmpeg | FFmpeg API/CLI | vgl 库 |
| L1 协议 | vgl 库（libvgl） | C，静态库 | L2 的 C API | 薄桥驱动 |
| L0 通道 | vgvdo 薄桥驱动 | C，KMDF（WDF） | IOCTL 契约 | virtio-gpu PCI 设备 |

**与 Linux 实现同构映射**（Linux 已打通，零改动层不列出）：

| Linux | Windows 对应物 | 状态 |
|---|---|---|
| h264_vaapi 进入 hwcodec 候选列表 | h264_vgl 进入候选列表 | L3 patch |
| FFmpeg h264_vaapi encoder | FFmpeg h264_vgl encoder | L2 |
| libva + Mesa gallium（VAAPI→pipe_video→virgl 命令序列化） | vgl 库（照 reference/ 的 Mesa virgl_video/encode 移植视频子集） | L1 **核心开发** |
| virtio_gpu.ko（virtqueue/资源/DRM） | vgvdo 薄桥驱动（virtqueue 通道化） | L0 |
| VASurface 系统内存 GEM BO → ATTACH_BACKING | NV12 用户内存 pin（MDL）→ ATTACH_BACKING | L1/L0 |
| Annex-B in-band 码流（VideoToolbox 验收） | 同形态直出 FFmpeg packet | 继承，天然规避 F13 |

---

## 1. 部署形态与设备拓扑（M1 前置）

### 1.1 双 virtio-gpu 设备（关键设计决策）

viogpudo 已以 WDDM 身份独占第一个 virtio-gpu 设备（显示）。同一 PCI 设备不可再绑定薄桥驱动。
**方案**：VM 定义中新增第二个 virtio-gpu PCI 设备（无显示输出，virgl 开启），专用于视频命令通道。

- libvirt 片段见 `xml/virtio-gpu-dual.xml`；
- 属于 VM 定义层配置，**不触碰 host 侧冻结边界**；
- M1 必测项：host 补丁在无显示输出的 virtio-gpu 实例上响应 create_codec（预期兼容，观察窗口 = 宿主 QEMU 日志 `DBG create_codec`）。

### 1.2 Windows VM 组件清单

| 组件 | 形态 | 安装方式 |
|---|---|---|
| vgvdo.sys | KMDF 内核驱动 | 测试签名（bcdedit testsigning），devcon/pnp 绑定第二 virtio-gpu 设备 |
| vgl.dll / vgl.lib | 用户态库 | 静态链进 FFmpeg（avcodec），不单独注册 |
| avcodec-XX.dll 等定制 FFmpeg | 替换 hwcodec 捆绑库 | 覆盖 RustDesk 发布目录同位置 dll（版本对齐 externals） |
| hwcodec 定制 crate | Rust | RustDesk Cargo `[patch]` 指向 haol666 fork 分支 |

---

## 2. L0 薄桥驱动 vgvdo（virtio-gpu video bridge）

### 2.1 定位

只做"管道"，不含任何协议语义（资源管理、命令序列、fence 逻辑全在 L1）。复用 virtio-win VirtIOLib 的 virtio PCI transport 与 virtqueue 实现（参照 NetKVM/ivshmem 驱动结构）。

**G1 结论修正（通道面）**：α 路线证实——视频命令是 virgl 3D 命令流（VIRTIO_GPU_CMD_SUBMIT_3D）内的 VIRGL_CCMD_* dword，与 CTX_CREATE/RESOURCE_CREATE/ATTACH_BACKING/GET_CAPS/FENCE 等同走 virtio-gpu **控制队列**。因此桥的语义面 = "virtio-gpu 控制队列原语透传"（对照 Linux 的 virtio_gpu.ko + DRM ioctl 子集），视频语义全部留在 L1。L1 需要的 virtio-gpu 控制命令集（virtio-gpu spec 命令，非 VIRGL_CCMD）：

`CTX_CREATE / RESOURCE_CREATE_2D / RESOURCE_ATTACH_BACKING / RESOURCE_UNREF / SUBMIT_3D / TRANSFER_TO_HOST_3D / TRANSFER_FROM_HOST_3D / GET_CAPS / FENCE_POST + 中断/响应回收`

（Linux 侧对照：reference/ 的 virgl_encode.c（CCMD 内容）+ Mesa winsys 与内核 virtio_gpu.ko（外层信封）；内核 UAPI 头 virtgpu_drm.h 属公开 spec。）

### 2.2 设备接口

- 符号链接：`\\.\VGLVIDEO0`（设备接口 GUID 由驱动注册）
- 绑定：第二 virtio-gpu PCI 设备（Vendor 0x1AF4 / Device 0x1050 virtio-gpu modern），PCI ID 经设备树匹配（仅"视频专用"实例：与 viogpudo 的绑定顺序通过设备在 XML 中的顺序+独立 PCI 槽位区分，驱动不做 viogpudo 已占实例的抢占）

### 2.3 IOCTL 契约（vgl/include/vgl_transport.h 同步定义）

| Code | 方向 | 功能 |
|---|---|---|
| `IOCTL_VGL_GET_INFO` | OUT | virtio 版本、控制队列大小（驱动固定初始化 64 深度）、notify/MSI-X 能力、设备序列号 |
| `IOCTL_VGL_SUBMIT` | IN | 提交一块已拷贝好的命令 buffer（一次 virtqueue 描述符链），驱动 kick notify；L1 可用它发任意控制队列命令（含 SUBMIT_3D 的 CCMD 流与 GET_CAPS 的 reply buffer 指派） |
| `IOCTL_VGL_REAP` | OUT | 取回一帧已完成的响应（拷贝到用户 buffer），非阻塞，无响应返回 STATUS_RETRY 类错误码 |
| `IOCTL_VGL_WAIT` | IN | 等待中断事件（带超时 ms），返回后用户态应立即 REAP |
| `IOCTL_VGL_PIN` | IN/OUT | 锁定用户内存（MDL, IoWriteAccess），返回 handle + PFN 数组（GPA 列表，供 ATTACH_BACKING 与 GET_CAPS reply 使用） |
| `IOCTL_VGL_UNPIN` | IN | 解除 PIN（编码器销毁/资源回收时调用） |

**内存模型**：ATTACH_BACKING 的 pfn 数组由 L1 从 PIN 结果构造；TRANSFER/数据页由驱动验证写权限后映射，QEMU 侧语义与 Linux 完全一致（host 侧无需感知差异）。

### 2.4 工作量与风险

- 约 2~3k 行 C（含 VirtIOLib 复用），测试签名环境自用可行；
- 风险：MSI-X 中断与 VM 挂起语义（挂起/恢复时 PIN 的页失效）→ L1 在 WAIT 超时后走会话重建（fallback 到软编由 L3 探测逻辑天然完成）。

---

## 3. L1 vgl 库（libvgl）

### 3.1 公共 API（vgl/include/vgl.h）

```c
typedef struct vgl_encoder vgl_encoder_t;

int  vgl_open(void);                       /* 进程级打开（单例，懒惰连接驱动） */
void vgl_close(void);
int  vgl_probe(vgl_caps_t *caps);          /* 能力探测（对 hwcodec 枚举），仅测 create_codec+destroy */

vgl_encoder_t *vgl_encoder_open(const vgl_enc_params_t *p);  /* 会话建立 */
int  vgl_encode_frame(vgl_encoder_t *e, const vgl_frame_t *f, vgl_packet_t *out);
int  vgl_encoder_set_bitrate(vgl_encoder_t *e, int kbps);
int  vgl_encoder_force_idr(vgl_encoder_t *e);   /* RustDesk 关键帧请求 */
void vgl_encoder_close(vgl_encoder_t *e);
```

### 3.2 会话状态机

```
OPENED → CTX_CREATED → CODEC_CREATED → RESOURCES_READY → ACTIVE
   └─ 任意态错误 → FAILED（返回错误码，hwcodec 探测自动放弃）
```

**命令序列（G1 已定，α 路线：全部为 virgl 3D 命令流内的 VIRGL_CCMD_* dword）**：
```
CTX_CREATE(virgl context) → GET_CAPS(读 feature_check_version 决定 max_ref 是否发)
→ 创建 desc_buffers[2] + feed_buffers[2]（PIPE_USAGE_STAGING 性质资源）
   + coded buffer 资源（大小 = w*h/256*512，host CODED_BUF_DEFAULT_SIZE 同式）
→ CREATE_VIDEO_CODEC(53)
→ CREATE_VIDEO_BUFFER(55)×N[NV12=166, w/h, 2×plane 资源句柄]
→ 帧循环: BEGIN_FRAME(57) → 写 desc buffer、清零 feed buffer(8B)
          → ENCODE_BITSTREAM(60: cdc/src/coded/desc/feed)
          → END_FRAME(61) → flush+fence → 等 fence → 读 feed buffer
          {stat==SUCCESS(2), coded_size} → 从 coded buffer 取码流
→ DESTROY_VIDEO_BUFFER×N(56) → DESTROY_VIDEO_CODEC(54)
```
desc/feed 双缓冲 ping-pong（Mesa `switch_buffer()` 同款）；H264 宽高**先 16 对齐**再 create_codec（Mesa 同款）；profile 用 guest 数值（见 §3.4 硬值表）。

### 3.3 NV12 资源池

- 视频缓冲在 wire 上=「1 个 video buffer 句柄 + 2 个 plane 资源句柄」（Y=R8、UV=GR88 纹理资源，Mesa `vl_video_buffer` 同款）；
- Windows L1 用 PIN 的 guest 内存做 plane 资源 backing（ATTACH_BACKING），系统内存即 LINEAR，**F1 坑天然规避**；
- 池深 4（desc/feed 借 Mesa 的 2 深度、输入面留余量），帧数据由 L2 memcpy 进 PIN 区；
- 带宽：1080p30 NV12 ≈ 0.75 Gbps，memcpy 开销 <1ms/帧。

### 3.4 wire 契约（vgl/include/vgl/vgl_wire.h，**AUTHORITATIVE**，G1 已关闭 2026-10-12）

来源三重交叉验证：host 交底副本 `virgl_video_hw.h`、Mesa(openKylin fork) 副本（fc diff 仅空白差异）、host virgl_video.c r9 映射表。参考文件全量入库 `reference/`。

- **结构**：`virgl_base_picture_desc`(264B) / `virgl_h264_enc_picture_desc`(**2556B**) / `virgl_h265_enc_picture_desc`(**2204B**，W4b 勘误：初版误算 2200，因 rc 手算 72 实为 76) / `virgl_video_encode_feedback`(8B)——逐字段转录+编译期尺寸 pin+运行期 offset 校验（h264/h265 双套）；
- **硬值表**（写成枚举，测试断言钉死）：
  - profile（guest Mesa 26.2.4 数值）：H264Baseline=**7** / ConstrainedBaseline=8 / Main=**9** / High=**11**；HEVC Main=**15** / Main10=**16**（host r9/r10/v12.5 已闭环，M5 可用）
  - entrypoint：BITSTREAM=1 / **ENCODE=4**；chroma 420=**2**（NV12）；level 0=host 推导
  - picture_type：P=0/B=1/I=2/**IDR=3**/SKIP=4（force_idr 用）；rc_method：CBR=3/VBR=4
  - NV12 的 virgl format id = **166**；命令 ID：CREATE_VIDEO_CODEC=**53** … END_FRAME=**61**（VIRGL_CMD0(cmd,obj,len)，len=payload dword 数）
  - **版本门**：`caps.v2.host_feature_check_version ≥ 14` 时 CREATE_VIDEO_CODEC 才带第 8 个 dword max_ref（Mesa virgl_encode.c:1780 同款）
- **M1 关卡 `vgl_wire_check()`**：✅ 已实现（offset 抽查 260/324/620/632/1008/2548 + 编译期尺寸 pin），CI 常跑。

### 3.5 码流交付

host 合成 in-band SPS/PPS/AUD（Annex-B）→ L1 原样转交 L2 → FFmpeg packet。
**不生成 avcC/extradata**（与 Linux VAAPI 输出同形态，RustDesk 管线已验收，F13 双源坑天然规避）。

---

## 4. L2 FFmpeg h264_vgl encoder

### 4.1 文件与注册

- `ffmpeg-h264-vgl/vglenc.c`（完整源码在本仓库，M2 时移植进 FFmpeg 树生成正式 patch）；
- 集成：基于 rustdesk-org/externals 的 FFmpeg 构建脚本追加 `libvgl`（pkg-config 静态链接），产物替换 RustDesk 捆绑 dll（版本与 hwcodec externals 对齐）。

### 4.2 行为契约

- 输入：**仅 AV_PIX_FMT_NV12**（无 D3D11/DXVA 设备依赖，比 h264_nvenc 的 nv12→d3d11 桥更少一步）；
- options：profile(high/main→wire 11/9)、bitrate、gop、rc_mode（CBR/VBR/QP）、zerolatency 默认开启（bf=0）；
- packet：IDR 标 AV_PKT_FLAG_KEY；keyframe 首包自带参数集；
- caps：AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_DR1；hwdevice：无（hwcodec vaapi 分支按名字判断，h264_vgl 不进该分支）。

### 4.3 验收工具即测试工具

`ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=30 -c:v h264_vgl -b:v 4M out.mp4` → ffprobe 通过 = M2 完成（对照交底 §4.4）。

---

## 5. L3 hwcodec patch

### 5.1 变更点（fork 分支 `vgl-support`）

`src/ffmpeg_ram/encode.rs` → `available_encoders()`：

```rust
#[cfg(windows)]
{
    // vgl: virtio-gpu 透传编码（Unraid host VCN），仅在通道探测可用时上报
    if vgl_available() {
        codecs.push(CodecInfo {
            name: "h264_vgl".to_owned(),
            format: H264,
            priority: Priority::Good as _,   // 真本地 GPU(nvenc/qsv/amf)优先, VM 内自然兜底
            ..Default::default()
        });
    }
}
```

- `format_from_name("h264_vgl")` 含 "h264" → 自动归 H264 ✓；
- 运行时探测（720p 哑帧试编、key==1、超时约束）自动完成可用性验证，无需伪造 GPU 厂商检测；
- `vgl_available()`：调用 `vgl_probe()`（通过弱依赖/进程内 DLL 检查驱动接口存在 + 试会话），失败返回 false。
- 完整 diff 见 `hwcodec-patch/0001-support-h264_vgl-on-windows.patch`。

### 5.2 RustDesk 集成（零源码改动）

根 `Cargo.toml` 追加：

```toml
[patch."https://github.com/rustdesk-org/hwcodec"]
hwcodec = { git = "https://github.com/haol666/hwcodec", branch = "vgl-support" }
```

---

## 6. CI / 编译策略（GitHub Actions）

| Workflow | Runner | 内容 | 触发 |
|---|---|---|---|
| `build-vgl.yml` | windows-latest | CMake+MSVC 编译 libvgl（transport 层无设备可编）+ ctest（wire 单测） | push/PR |
| `build-hwcodec.yml` | windows-latest | submodule init + cargo check（fork 含 patch） | push/PR（paths 过滤） |
| `build-ffmpeg.yml` | windows-latest | externals 构建脚本 + vglenc 集成产物 | M2 后启用（workflow_dispatch） |

驱动（KMDF）CI 依赖 EWDK，M1 后评估（本地编译优先，Actions 挂 ISO 次之）。

---

## 7. 里程碑（对照交底 §4.4）

| 阶段 | 内容 | 验收 |
|---|---|---|
| M1 | vgvdo + vgl 库 + 双设备 XML；发首个 create_codec | 宿主日志 `DBG create_codec: pipe_profile=11` |
| M2 | vglenc 集成 + 定制 FFmpeg 构建 | `B1 SPS`+seg 日志；ffprobe 通过 |
| M3 | hwcodec 分支 + RustDesk Cargo patch | 探测过 h264_vgl；`HWRAM h264_vgl` 不撤回 |
| M4 | 端到端远控 | Mac 主控（VideoToolbox）不降级 |

---

## 8. 坑规避清单（继承交底 §5 + 本侧新增）

| # | 坑 | 规避 |
|---|---|---|
| W-A1 | 枚举数值错位（F17） | wire 值表写死常量，单测断言（`test_wire.c`） |
| W-A2 | packed headers 双源（F13） | in-band 直出，无 extradata；MF/MFT 二期才需处理 |
| W-A3 | host 强制 LINEAR（F1） | 系统内存 PIN 即线性 |
| W-A4 | level=0（F9） | 传 0，host 推导 |
| W-A5 | max_num_ref_frames（F18） | 能力枚举不承诺超 level DPB |
| W-A6 | 宿主 VA 争用红线（§2） | 一切诊断走 QEMU 日志 |
| W-B1 | viogpudo 设备占用 | 双 virtio-gpu 设备（§1.1） |
| W-B2 | 无显示实例上的视频命令 | M1 首测项 |
| W-B3 | VM 挂起/恢复页失效 | WAIT 超时会话重建 + 探测自动回软编 |
| W-B4 | FFmpeg dll 版本漂移 | 严格对齐 externals 构建脚本与 ABI 版本 |

---

## 9. 仓库与提交纪律

- 本仓库：`haol666/virgl-video-windows`（设计/驱动/库/patch/XML/CI/WORKLOG）；
- fork：`haol666/hwcodec`（分支 vgl-support）、`haol666/externals`（M2 起追加 libvgl 构建）；
- **工作日志**：`docs/WORKLOG.md`，Windows 侧条目 W 系列编号（与 host 侧 F 系列区分）；
- **提交纪律**：每完成一个可编译/可验证单元立即 commit+push；WORKLOG 同步记录（发现/决策/教训），一工作单元一条，含宿主日志摘录。

---

## 10. 能力枚举口径（对上层）

- H.264：Main（9）/ High（11）；1080p 起步（VCN 2.0 实测上限按 host 探针再收敛）；
- HEVC：本期不做（host 未激活，wire 15/16 未实测）；
- 帧率上限：30/60 由 create_codec 实测确定，不虚报。

---

## 11. 二期方向（本期不投入）

- MF/MFT 系统注册（覆盖浏览器 WebRTC / 媒体应用，在 vgl 库之上包壳）；
- HEVC（待 host 侧 HEVC 工作包）；
- 驱动 WHQL/正式签名（如需对外分发）。

---

## 12. M1 前置缺口（对外依赖，OPEN）
 
| # | 项 | 来源 | 阻塞 | 状态（2026-10-12） |
|---|---|---|---|---|
| G1 | 权威 `virgl_video_hw.h` 副本 | 交底 §8 | M1 硬前置（wire 逐字段校对） | ✅ **已关闭**：用户移交 4 文件，且补齐 Mesa 侧全套（reference/ 入库），vgl_wire.h 已权威化 |
| G2 | `virgl_video.c`（B1 实现源码） | virgl-video-project | M1 参考实现 | ✅ 已关闭（user 提供，host 行为契约全解读） |
| G3 | `docs/WORKLOG.md`（F1–F19+§6-19） | virgl-video-project | 联调背景 | ✅ 已关闭（user 提供；重要更新见下） |
| G4 | guest Linux Mesa 版本 | Linux 测试 VM | 命令序列对照 | ✅ 已关闭（26.2.4；Mesa 实现参考文件入库 reference/） |
| G5 | GitHub PAT | haol666 | 远程提交 | ✅ 已关闭（gitconfig extraHeader token，REST API 路线） |
| G6 | host 生产基线勘误 | WORKLOG §17-19 | 无（信息项） | ✅ 交底文档"冻结于 r8"已被 WORKLOG 推进：**当前生产基线 v12.5（16f8e79/载入 5ee5a73a），HEVC 编码 r9/r10/v12.5 已全链闭环**。Windows 侧仍按 M1–M4 只做 H264（交底范围），但 HEVC wire 值 15/16 已从"推断"升级为"已验证"，M5 随时可启动 |

**注意**：host 协议冻结声明依然有效——wire 面（命令+结构）自 v12.3h 以来未变，演进全部在 host 端 B1 合成/对账逻辑（r8→v12.5 只是 bugfix 流），Windows 侧无需跟进任何 host 变更。

---

*本仓库一切 host 侧问题走 issue 附 QEMU 日志，不自行改宿主（交底 §3.5/§6）。*
