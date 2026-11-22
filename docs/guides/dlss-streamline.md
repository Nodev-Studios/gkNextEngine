---
title: "NVIDIA Streamline / DLSS / DLSS-G"
category: guide
status: 现行
owner: NextStreamline
created: 2026-06-18
last_updated: 2026-11-22
---

# NVIDIA Streamline / DLSS / DLSS-G 指南

gkNextEngine 在 Windows x86_64 上通过 NVIDIA Streamline 集成 DLSS。Streamline 没有 Linux 版本，
Linux x86_64 上同一个 `NextStreamline` 模块改为直接调用 NGX SDK，提供 DLSS-SR 与 DLSS-RR，见下文
[Linux：通过 NGX 直接集成](#linux通过-ngx-直接集成)。其余平台编译相同的 renderer 路径，但 DLSS 处于禁用状态。

## SDK 版本与模型预设

当前随附 Streamline SDK **2.14.16**。版本在两处锁定，升级时一起改：

- `gnb.toml` 的 `[external.streamline]`：`version` 与 `url`，决定 `gnb setup` / `gnb deps fetch streamline`
  解压到 `external/streamline-<version>/`。
- `cmake/SetupExternalLibs.cmake` 的 `GK_STREAMLINE_VERSION`：CMake 只认这个目录，`external/` 下残留的旧版 SDK
  不会被选中；目录缺失时配置直接报错并提示先 fetch。

升级流程：改上面两处 → `gnb deps fetch streamline` → `gnb build --reconfigure`（配置阶段会把新 SDK 的
`bin/x64` 拷到输出目录）。

模型预设在 `StreamlineIntegration.cpp` 的 `SetSuperResolutionOptions` 里显式指定：

| 功能 | DLAA / Quality / Balanced | Performance | Ultra Performance |
|---|---|---|---|
| DLSS-SR | K | M | L |
| DLSS-RR | F | F | F |

M / L 是 DLSS 4.5 的第二代 transformer，只在低输入分辨率档位上用；K 在高档位上质量余量够、开销更低。
RR 的 F 是 2.14 新增的 transformer，也是 SDK 默认值。

DLSS 5（3D-Guided Neural Rendering，`sl.dlss_nr`）尚不可用：2.14 的公开头文件只预留了
`kFeatureDLSS_NR` 和 `kBufferTypeUplift*` buffer 类型，公开包里没有该 plugin、options 头文件和编程指南。

## 运行时 CVar

- `r.upscaler.type 1`：选择 DLSS Super Resolution。
- `r.upscaler.type 2`：选择 DLSS Ray Reconstruction；这是独立的 upscaler type。
- `r.frameGeneration 1`：在当前 type 支持 Frame Generation 时启用。
- `r.frameGeneration.multiplier 2`：帧倍增系数，目前限制在 `2..4`。
- `r.frameGeneration.frameLimitFps 0`：Frame Generation 开启时的 base-frame 限帧；0 表示不限制。
- `r.upscaler.jitterFrames 16`：provider 未给出 phase count 时的 projection jitter 序列长度。
- `r.upscaler.jitterInvertY 0`：只用于验证 temporal upscaler jitter 符号的诊断开关。
- `r.upscaler.qualityMode 0..5`：Quality、Balanced、Performance、Ultra Performance、Native/DLAA、Auto。Auto 在输出不超过 1920×1080 时关闭超分并使用原生分辨率，更高分辨率使用 Quality。

修改 DLSS、DLSS-RR、DLSS-G、帧倍增系数或超分辨率模式会重建 swapchain。

无需编辑 `cvar_user.json`，也可以通过启动参数覆盖：

```bash
./gnb.sh run gkNextRenderer --load-scene=assets/models/playground.glb --cvar "r.upscaler.type 1" --cvar "r.frameGeneration 1"
```

对于会反复创建和销毁 engine instance 的测试进程，使用 `--disable-streamline`。Windows
桌面应用会同时注册 Streamline 与 FidelityFX；二者通过 composite `IUpscaler` 和可叠加的
swapchain interposer layer 共存，选择 FSR 不再要求 `--disable-streamline`。

## 预期启动日志

在受支持的 NVIDIA 硬件上，启动时应报告 Streamline device 就绪状态和各 feature 支持情况：

```text
Streamline Vulkan proxy device ready. DLSS=true, RR=true, DLSS-G=true, Reflex=true, PCL=true
DLSS-G enabled
DLSS-G state OK: presented=2, maxGenerate=3, vram=0 MB
```

`presented=2` 是默认 2x 模式下的预期 active 值。预热期间，或驱动临时抑制生成时（例如窗口非活动/被遮挡，或存在不兼容 overlay），运行时可能报告 `1`。

启用 DLSS-G 后，renderer 会在 UI 渲染前标记 depth、motion vector、专用的无 HUD 快照以及 backbuffer。它还会用 PCL 标记 simulation、submit、present，并且每帧调用一次 Reflex sleep。

## DLSS-G 要求

DLSS-G 需要：

- Windows，且已启用硬件加速 GPU 调度。
- 可执行文件旁存在 Streamline DLSS-G plugin 和 Reflex plugin。
- `slIsFeatureSupported` 报告 NVIDIA 硬件和驱动支持该功能。
- swapchain format 被 DLSS-G 接受。HDR 输出只允许 10-bit HDR format；DLSS-G 会禁用 FP16/scRGB HDR。
- VSync 关闭。启用 DLSS-G 时，Vulkan renderer 会强制使用 immediate present mode。

Streamline 初始化时，engine 会要求 SDL 将 `sl.interposer.dll` 作为 Vulkan loader 加载；如果失败，则回退到系统 Vulkan loader。主 renderer swapchain 会在可用时通过 Streamline proxy entry points 路由 create、image query、acquire、present 和 wait-idle 调用。

已禁用下载/OTA Streamline plugin，因此运行时始终使用仓库随附的 plugin 版本。

## UI/HUD 处理

对于每个 proxy swapchain image，renderer 都持有一张输出分辨率的无 HUD image。ImGui 渲染前，它会把 resolved scene color 复制到这张 image，并将其标记为带 `eValidUntilPresent` 的 `kBufferTypeHUDLessColor`。ImGui 继续原地渲染到最终 backbuffer，因此生成帧使用的是冻结的场景 image，而不是后续被 UI 渲染修改过的 backbuffer。

当前 ImGui backend 不会生成独立的 premultiplied `kBufferTypeUIColorAndAlpha` target。无 HUD 集成是 SDK 支持的 fallback，可避免 UI 像素进入 optical-flow 输入；专用 UI target 仍是复杂 world-space HUD 的可选质量增强项。

swapchain 重建或切换到非 Streamline upscaler 时，DLSS-SR、RR、DLSS-G 都会关闭并释放 feature
resources 与 persistent tag；重新激活后由首帧 evaluate 按需创建。

engine 状态为 `Loading` 时不会启用 DLSS-G。这可以避免 `slDLSSGSetOptions` 与场景加载期间的 swapchain 替换发生竞争。

## Linux：通过 NGX 直接集成

Linux 上没有 Streamline，`NextStreamline` 模块里的 `NgxIntegration.cpp` 直接面向 NGX SDK
（[NVIDIA/DLSS](https://github.com/NVIDIA/DLSS)）实现同一个 `IUpscaler` 接口。运行时 CVar、质量档位、
模型预设（SR：K / M / L，RR：F）与上文完全一致，引擎核心不区分两条路径。

| 功能 | Windows（Streamline） | Linux（NGX） |
|---|---|---|
| DLSS Super Resolution（`r.upscaler.type 1`） | ✅ | ✅ |
| DLSS Ray Reconstruction（`r.upscaler.type 2`） | ✅ | ✅ |
| DLSS-G Frame Generation | ✅ | ❌ |
| Reflex / PCL | ✅ | ❌ |

Frame Generation、Reflex 和 PCL 依赖 Streamline 的 present 代理层，NGX 本身不提供，因此 Linux 上
`r.frameGeneration` 不生效。

### SDK 与构建

- 版本同样两处锁定：`gnb.toml` 的 `[external.dlss]` 与 `cmake/SetupExternalLibs.cmake` 的
  `GK_DLSS_NGX_VERSION`（当前 **310.9.16**）。
- `gnb deps fetch dlss`（Linux 上的 `gnb setup` 会一并执行）把头文件、`libnvsdk_ngx.a` 以及
  `libnvidia-ngx-dlss.so.*` / `libnvidia-ngx-dlssd.so.*` 逐个文件下载到 `external/dlss-<version>/`，约 100 MB。
  SDK 唯一的整包归档带着全部平台，体积是它的数倍，所以不走归档。
- CMake 按 SDK 是否存在决定 `WITH_DLSS_NGX`：缺失时只打印提示并照常构建（没有 DLSS），不会让配置失败。
  取回 SDK 后执行 `gnb build --reconfigure`，配置阶段会把两个模型库拷到输出目录。
  `-DENABLE_DLSS_NGX=OFF` 可显式关闭。
- 运行时需要 NVIDIA 专有驱动：NGX 核心（`libnvidia-ngx.so`）来自驱动，随引擎分发的只有模型库。
  没有加载 NVIDIA 内核模块、或选中的不是 NVIDIA 显卡时，provider 不安装，日志会说明原因。
- `--disable-streamline` 在 Linux 上同样关闭这条路径。

### 预期启动日志

```text
DLSS provider installed through NGX (Super Resolution, Ray Reconstruction)
NGX Vulkan device ready. DLSS=true, RR=true
DLSS active for PathTracing: 960x540 -> 1920x1080
NGX DLSS feature created: 960x540 -> 1920x1080 (HDR input)
```

### 实现上的约束

- **Vulkan 扩展用 `NVSDK_NGX_VULKAN_RequiredExtensions` 查询，不用按 feature 的 discovery API。**
  `NVSDK_NGX_VULKAN_GetFeatureRequirements` 与两个 `GetFeature*ExtensionRequirements` 会在
  `NVSDK_NGX_VULKAN_Init` 之前加载 feature 库；当目录里同时有 SR 与 RR 两个库（驱动还自带一份 RR）时，
  NGX 核心随后会把 SR 的槽位绑到 RR 库上——创建 DLSS feature 成功，evaluate 却在
  `libnvidia-ngx-dlssd` 里崩溃。feature 是否可用改在 Init 之后通过 capability parameters 查询。
- **SR 与 RR 共用一个 parameter block。** 创建 RR 会在其中留下 `DLSS.Denoise.Mode = DLUnified`，而 SR 库
  创建时会读取该参数，所以创建 SR 时显式写回 `Off`。
- NGX 采样输入、以 storage 方式写输出，provider 在 evaluate 前后自行做 layout 转换并还原，
  renderer 侧不需要为 DLSS 额外插 barrier。

### 验证

`gnb shot` / `gnb validate` 带 `--agent-validation`，会把 DLSS 换成 Native TAAU，无法证明 DLSS 生效。
Linux 上隐藏窗口可以正常跑 DLSS，用下面的命令即可（不弹窗，跑完自动退出）：

```bash
out/build/linux/bin/gkNextRenderer --load-scene=assets/models/playground.glb --hidden-window \
    --cvar "r.upscaler.type 1" --exit-after-frames 300
```

确认日志出现上面的 `NGX ... feature created`，且没有 `NGX ... evaluation failed`。注意已归档的用户设置
（`~/.local/share/gkNext/<app>/assets/configs/cvar_user.json`）里若已有同名 CVar，会覆盖 `--cvar`。

## 故障排查

- 如果 `r.frameGeneration 1` 没有增加 presented frames，检查 renderer settings 面板中的 DLSS-G state 行。非零 status mask 表示 Streamline runtime 拒绝了当前状态。
- 如果 status 为 `0` 但 presented count 仍为 `1`，确认游戏窗口处于活动状态，并在重新测试前禁用 RivaTuner Statistics Server 等 present-hook overlay。
- 不要用 `--agent-validation` 或 `--hidden-window` 验证 Streamline。Agent validation 为确定性
  会禁用 Streamline；`gnb shot` 只能验证进入 DLSS 前的 scene color/depth/motion 资源链。
  DLSS-SR/RR/DLSS-G 必须使用正常可见窗口验证。
- Linux 上日志出现 `DLSS unavailable through NGX` 时，先看是否提示驱动需要更新；出现
  `DLSS disabled because device extension ... is unavailable` 说明驱动缺少 NGX 需要的 Vulkan 扩展。
- 如果启动日志显示请求的 Vulkan extension 不可用，该 adapter 的 DLSS 功能会被禁用，renderer 会回退到普通 resolve。
- 如果启用了 HDR 但 DLSS-G 拒绝运行，强制使用 SDR，或切换到 10-bit HDR swapchain format。
- 如果 validation 报告 swapchain layout 问题，确认 resolve 路径在 DLSS/blit 前调用 `SwapChain::InsertBarrierToWrite`，并在 present 前调用 `InsertBarrierToPresent`。
