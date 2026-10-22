# FFmpeg n9.0.2 ProRes 硬解调研页

日期：2026-10-22。页面是单文件 `index.html`。事实以 tag `n9.0.2` 源码为准，本机 Windows 未跑过 VideoToolbox。

## 设计

调色室里的一条片卷，不是产品介绍页。近黑暖底、象牙纸两种配色各自设计；琥珀只标「正在片门里的那一档」。六档 profile 排成一条片子，点进片门后改写流水线末端的像素格式。第三个问题收在一张未曝光的空画格上：没有能直接看 FFmpeg 硬解画面的网站。换一个题目，片卷、片门和空画格就都不成立。

## 范围

只写接到 `prores` 解码器上的官方 hwaccel。VideoToolbox 是正文。Vulkan 是旁注。不比较 NVDEC、QSV、VAAPI、AMF、D3D 的厂商细节；`configure` 里与 ProRes 相关的 hwaccel 只有这四行：

- `prores_videotoolbox`（依赖 `videotoolbox`）
- `prores_vulkan`（依赖 `vulkan spirv_compiler`）
- `prores_raw_videotoolbox`
- `prores_raw_vulkan`

ProRes RAW 是 `AV_CODEC_ID_PRORES_RAW`，不进六档片卷，页面底部一条旁注。

## 版本

稳定版 tag `n9.0.2`（GitHub tag 日期 2026-09-18）。Changelog 里相关条目的首次出现版本：

- 5.0：VideoToolbox ProRes hwaccel（`prores_videotoolbox`）
- 8.0：ProRes RAW 软解，以及 `prores_raw_vulkan`
- 8.1：`prores_vulkan`
- 9.0：`prores_raw_videotoolbox`

n9.0.2 源码同时包含 `prores_videotoolbox` 与 `prores_vulkan`。

## 六档

`libavcodec/proresdec.c` 的 `decode_init` 与 `libavcodec/videotoolbox.c` 的 `videotoolbox_start` 使用同一组 FourCC。未知 tag 会打警告，然后仍然 `av_bswap32(codec_tag)` 交给 VideoToolbox。

| 档 | FourCC | 色度 | 解码位深 | 规范上的 alpha |
| --- | --- | --- | --- | --- |
| Proxy | apco | 4:2:2 | 10 | 不带 |
| LT | apcs | 4:2:2 | 10 | 不带 |
| 422 | apcn | 4:2:2 | 10 | 不带 |
| 422 HQ | apch | 4:2:2 | 10 | 不带 |
| 4444 | ap4h | 4:4:4 | 12 | 可选 |
| 4444 XQ | ap4x | 4:4:4 | 12 | 可选 |

422 与 444 由帧头 `buf[12] & 0xC0 == 0xC0` 判定，不单靠档名。

## Alpha 的两层

帧头始终先解析，硬解发生在这之后。`decode_frame` 调用 `decode_frame_header`，再在 `avctx->hwaccel` 存在时走 `start_frame` / `decode_slice` / `end_frame`，否则走软解 `decode_picture`。

码流层（源码已核对）：

- `alpha_info = buf[17] & 0xf`。大于 2 判非法。`skip_alpha` 把它清零，后面的软解和硬解请求都当成没有 alpha。
- 0 无 alpha。1 按 8-bit 解 alpha。2 按 16-bit 解 alpha（`decode_slice_alpha`）。
- 有 alpha 时软解像素：10-bit 为 `yuva444p10le` 或 `yuva422p10le`；12-bit（4444 / XQ）为 `yuva444p12le` 或 `yuva422p12le`。无 alpha 时去掉 a，成为 `yuv444p12le` / `yuv422p10le` 等。
- Apple 的这六档里，只有 4444 与 4444 XQ 会带 alpha。422 四档页面写「不适用」。若 422 帧头异常标了 alpha，软解会出 `yuva422p10le`，这不是正常文件。

VideoToolbox 表面（源码已核对请求，本机未复现输出）：

- `videotoolbox_best_pixel_format`：软件像素格式带 `AV_PIX_FMT_FLAG_ALPHA` 且深度大于 8 时，请求 `ayuv64le`，对应 `kCVPixelFormatType_4444AYpCbCr16`。
- 无 alpha 的 4:2:2、10-bit，在 SDK 提供 `kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange` 时请求 `p210le`。
- 无 alpha 的 4:4:4、12-bit，在 SDK 提供 16-bit 4:4:4 biplanar 格式时请求 `p416le`。
- 这些宏缺失时函数会落到 `nv12`。页面要写上这条条件编译。
- ProRes、ProRes RAW、HEVC 使用 `EnableHardwareAcceleratedVideoDecoder`。其余 codec 使用 `RequireHardwareAcceleratedVideoDecoder`。Enable 允许 VideoToolbox 回落到 Apple 的软件解码器。源码在 macOS 上还会调用 `VTRegisterProfessionalVideoWorkflowVideoDecoders()`。
- 因此「FFmpeg 向 VT 要了 AYUV64」不等于「这块芯片的媒体引擎吐出了 alpha」。页面把这两句分开。

Vulkan 旁注（源码已核对）：

- `libavcodec/vulkan_prores.c` 把 `alpha_info` 放进 push constant。
- 有 alpha 时 dispatch 的第三维是 `3 + 1`，注释写明清图时跳过 alpha 平面。
- 输出像素格式是 `AV_PIX_FMT_VULKAN`，软件侧格式仍是上面的 yuv / yuva。这是计算着色器，不是固定功能 ProRes 芯片。

## 演示站

- Apple WWDC20 session 10090：`https://developer.apple.com/videos/play/wwdc2020/10090/`。讲的是 AVFoundation / VideoToolbox / Afterburner / Metal，不是 FFmpeg 的像素对比。
- TurboRes 交互演示：`https://turbores.pages.dev/`。浏览器里的播放器是软解。仓库 README 含作者自己机器上的 FFmpeg 硬解帧率，页面不转载这些数字。
- 空画格：没有找到能在网页里直接观看 FFmpeg ProRes 硬解画面的站点。浏览器不能播 ProRes。VideoToolbox 只存在于编进 Apple 平台的 FFmpeg。

## 页面结构

1. 题头：一句话结论，徽章 `FFmpeg n9.0.2`。
2. 片门：帧头解析后分叉到软解、VideoToolbox、Vulkan 旁注。点选档位时改写末端像素格式，4444 / XQ 点亮 alpha。
3. 六格片卷，键盘左右切换。片门下写 FourCC、色度、位深、软解像素、VT 请求的表面、两行 alpha。
4. Alpha 专节：棋盘格上的色块对照「只有彩色」和「彩色加 alpha」。
5. 三格接触印样：WWDC、TurboRes、未曝光。
6. 片边来源，以及 ProRes RAW 旁注。脚注写明 Windows 撰写、未在 Apple Silicon 上复现。

单文件，CSS 与脚本内联，无 CDN。系统字体。跟随系统深浅色，右上角按钮可切换并记住。`prefers-reduced-motion` 时不做入场动画。

## 自检

- 无未定结论。条件编译和「未在 Mac 上复现」已写明，不把 VT 请求格式说成实测画面。
- 码流能解析与硬解表面带 alpha 没有写成同一句话。
- 范围停在官方 hwaccel。Vulkan 保持旁注。ProRes RAW 不进入六档。
