# ProRes 播放器

在 Windows 上播放 ProRes。硬解使用 FFmpeg 9.0.2 的 Vulkan 计算着色器，画面用 DX11 绘制，界面是 Qt 5.12.12 的圆角窗口。硬解不可用时会标明「软解」并继续播放。

打开方式：拖入文件、打开文件、或打开文件夹后从列表里选。播放、暂停、停止、快进、快退和进度条都在主窗口上。「文件信息」用 MediaInfo 显示素材详情。带 alpha 的画面会叠在棋盘格上。

## 依赖

- Qt 5.12.12 MSVC 2017 64 位，本机 `QT_DIR` 为 `D:\soft\Qt5.12.12\5.12.12\msvc2017_64\bin`
- `3rd/ffmpeg`：FFmpeg 9.0.2（源码标签 `n9.0.2`，2026-09-18），Windows 动态库完整包，解压后的目录名是 `9.0.2-full_build-www.gyan.dev`。源码在 https://github.com/FFmpeg/FFmpeg/releases/tag/n9.0.2 ，动态库包在 https://github.com/GyanD/codexffmpeg/releases/download/9.0.2/ffmpeg-9.0.2-full_build-shared.7z
- `3rd/MediaInfo/MediaInfo.dll`：MediaInfo 26.10（2026-10-06）的 Windows x64 DLL，运行时加载。下载页在 https://mediaarea.net/en/MediaInfo/Download/Windows ，DLL 包在 https://mediaarea.net/download/binary/mediainfo/26.10/MediaInfo_DLL_26.10_Windows_x64.zip

## Vulkan 硬解环境

硬解使用 FFmpeg 9.0.2 的 `prores_vulkan` 计算着色器，不是 NVDEC。显卡驱动必须让 Vulkan 接受素材的像素格式，否则播放器退回软解。

已在 NVIDIA RTX A4000 上测试 `e平台3.0 会展09 03 B_LT.mov`（ProRes 422 LT，12288x3840，`yuv422p10le`）：

- `595.84` 可用。安装包为 `595.84_quadrogeforce_win11_win10-dch_64bit_international.exe`。该素材可以 Vulkan 零拷贝播放。
- `474.04` 不可用。同一份素材报 `Unsupported sw format: yuv422p10le`，只能软解。

下表是 FFmpeg `n9.0.2` 的 `prores_vulkan` 在 Windows 上可以尝试的档位。硬解输出仍是这些软件像素格式，驱动必须接受对应的 Vulkan 图像格式，否则日志为 `Unsupported sw format`，播放器退回软解。

| 档位 | FourCC | 像素格式 | 色度 | 位深 | Alpha | Vulkan 格式 | 本机实测 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 422 Proxy | apco | yuv422p10le | 4:2:2 | 10 | 无 | VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM | 待测。像素格式与 422 LT 相同 |
| 422 LT | apcs | yuv422p10le | 4:2:2 | 10 | 无 | VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM | RTX A4000、驱动 595.84：Vulkan 零拷贝通过。474.04：失败 |
| 422 | apcn | yuv422p10le | 4:2:2 | 10 | 无 | VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM | 待测。像素格式与 422 LT 相同 |
| 422 HQ | apch | yuv422p10le | 4:2:2 | 10 | 无 | VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM | 待测。像素格式与 422 LT 相同 |
| 4444 | ap4h | yuv444p12le | 4:4:4 | 12 | 无 | VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM | 待测 |
| 4444 | ap4h | yuva444p12le | 4:4:4 | 12 | 有 | 同上，并多一个 alpha 平面 | 待测。595.84 上尚未复测。旧环境曾报 Unsupported sw format: yuva444p12le |
| 4444 XQ | ap4x | yuv444p12le | 4:4:4 | 12 | 无 | VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM | 待测 |
| 4444 XQ | ap4x | yuva444p12le | 4:4:4 | 12 | 有 | 同上，并多一个 alpha 平面 | 待测。595.84 上尚未复测 |

隔行和逐行走同一个着色器，隔行尚未实测。ProRes RAW 是另一个解码器，不在这张表里。

## 构建

```
cmake -S . -B build -G "Visual Studio 15 2017" -A x64
cmake --build build --config Release
```

运行 `build\Release\ProResPlayer.exe`。构建步骤会把 FFmpeg DLL、`MediaInfo.dll` 和 Qt 运行库拷到该目录。

功能完成情况见 [docs/features.md](docs/features.md)。手工测试用例见 [docs/test-cases.md](docs/test-cases.md)。结构说明见 [docs/development.md](docs/development.md)。
