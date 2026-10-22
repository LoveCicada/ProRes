# ProRes 播放器

在 Windows 上播放 ProRes。硬解使用 FFmpeg 9.0.2 的 Vulkan 计算着色器，画面用 DX11 绘制，界面是 Qt 5.12.12 的圆角窗口。硬解不可用时会标明「软解」并继续播放。

打开方式：拖入文件、打开文件、或打开文件夹后从列表里选。播放、暂停、停止、快进、快退和进度条都在主窗口上。「文件信息」用 MediaInfo 显示素材详情。带 alpha 的画面会叠在棋盘格上。

## 依赖

- Qt 5.12.12 MSVC 2017 64 位，本机 `QT_DIR` 为 `D:\soft\Qt5.12.12\5.12.12\msvc2017_64\bin`
- `3rd/ffmpeg`：FFmpeg 9.0.2 full shared
- `3rd/MediaInfo/MediaInfo.dll`

## 构建

```
cmake -S . -B build -G "Visual Studio 15 2017" -A x64
cmake --build build --config Release
```

运行 `build\Release\ProResPlayer.exe`。构建步骤会把 FFmpeg DLL、`MediaInfo.dll` 和 Qt 运行库拷到该目录。

功能完成情况见 [docs/features.md](docs/features.md)。手工测试用例见 [docs/test-cases.md](docs/test-cases.md)。结构说明见 [docs/development.md](docs/development.md)。
