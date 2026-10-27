# 开发说明

## 做什么

Windows 上的简易播放器。标准 ProRes 和 ProRes RAW 可以走 FFmpeg 9.0.2 的 Vulkan 计算着色器（`prores_vulkan` / `prores_raw_vulkan`）。这不是 NVDEC 或 D3D11VA。DX11 只负责把画面画出来。VideoToolbox 不在本机使用。

打开或拖入素材后，解码线程先按当前显卡探测，界面只列出成功的方式：

- 软解：CPU 解码，再上传 8-bit BGRA。
- Vulkan 回读：GPU 解出帧后读回内存，再上传。这是 GPU→CPU→GPU。
- Vulkan 零拷贝：同一块显卡上把帧写成 `R16G16B16A16` 共享纹理，画面线程直接采样，像素不回 CPU。导入共享纹理失败就不提供这一项。

默认优先零拷贝，其次回读，最后软解。主窗口的「解码」下拉框可以改方式，并从当前时间重新解码。设置里更换显卡后会重新探测。软解状态不把解码算到显卡上。

## 线程

界面线程拥有全部 `QObject`，包括 `QAudioOutput` 和 `QTimer`。解码使用 `std::thread`，不使用 `QThread`。

解码线程读包、解码、把 BGRA 帧和 PCM 放进队列。界面线程的定时器从队列取 PCM 写入声卡，并按 `processedUSecs()` 决定该画哪一帧。

暂停用条件变量挡住解码线程，并 `suspend()` 声卡。停止先置标志、唤醒、`join`，再释放 FFmpeg 和 DX11。

## 画面

Vulkan 回读经 `av_hwframe_transfer_data` 拷回内存，`sws_scale` 成 8-bit BGRA，再上传到 DX11 动态纹理。零拷贝则由 Vulkan 计算着色器写入共享纹理。像素着色器在 alpha 有效时把画面叠到棋盘格上。播放区窗口区域是圆角。

零拷贝的 LUID 对齐、共享纹理、槽位与 GPU 管线见 [开发文档.md](开发文档.md) 中「Vulkan 零拷贝设计」。

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
