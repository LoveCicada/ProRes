# 开发说明

## 做什么

Windows 上的简易播放器。ProRes 硬解走 FFmpeg 9.0.2 的 Vulkan 计算着色器（`prores_vulkan`）。DX11 只负责把画面画出来。VideoToolbox 不在本机使用。

硬解设备创建或 `avcodec_open2` 失败时，同一条解码线程改走软解，界面写「软解」。成功走 Vulkan 时写「硬解 Vulkan」。

## 线程

界面线程拥有全部 `QObject`，包括 `QAudioOutput` 和 `QTimer`。解码使用 `std::thread`，不使用 `QThread`。

解码线程读包、解码、把 BGRA 帧和 PCM 放进队列。界面线程的定时器从队列取 PCM 写入声卡，并按 `processedUSecs()` 决定该画哪一帧。

暂停用条件变量挡住解码线程，并 `suspend()` 声卡。停止先置标志、唤醒、`join`，再释放 FFmpeg 和 DX11。

## 画面

Vulkan 帧经 `av_hwframe_transfer_data` 拷回内存，`sws_scale` 成 8-bit BGRA，再上传到 DX11 动态纹理。像素着色器在 alpha 有效时把画面叠到棋盘格上。播放区窗口区域是圆角。

## 依赖位置

- Qt 前缀：`QT_DIR` 的上一级，本机为 `D:\soft\Qt5.12.12\5.12.12\msvc2017_64`
- FFmpeg：`3rd/ffmpeg`（`9.0.2-full_build-www.gyan.dev`，`ffmpeg -hwaccels` 含 `vulkan`）
- MediaInfo：运行时 `LoadLibrary(MediaInfo.dll)`，头文件在 `3rd/MediaInfo/Developers/Source/MediaInfoDLL/MediaInfoDLL.h`

构建后把 FFmpeg 的 `bin` 和 `MediaInfo.dll` 拷到可执行文件旁边，并用 `windeployqt` 带上 Qt 运行库。

## 配置

使用 Visual Studio 的 v141 工具集（与这份 Qt 一致）：

```
cmake -S . -B build -G "Visual Studio 15 2017" -A x64
cmake --build build --config Release
```

已安装更高版本的 Visual Studio 时，生成器可以换成对应年份，并加上 `-T v141`。
