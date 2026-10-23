# 功能清单

手工测试用例还没有执行，见 [test-cases.md](test-cases.md)。

- [x] CMake 工程：C++17、Qt 5.12 Widgets 与 Multimedia、FFmpeg 9.0.2、MediaInfo DLL
- [x] 圆角主窗口：暖黑底，拖入视频
- [x] 打开文件；打开文件夹后列出 mov / mxf / mp4 / mkv 再播放
- [x] 「文件信息」用 MediaInfo 显示容器、Profile、FourCC、分辨率、色彩、音频和时间码
- [x] 解码线程：ProRes 优先 Vulkan 硬解，失败标明软解
- [x] DX11 上屏；带 alpha 时底下是棋盘格
- [x] 播放、暂停、停止、快进、快退、进度条
- [x] PCM 经 QAudioOutput 播放，画面按音频时钟送出
- [x] 文件信息窗口可拉大、可最大化，正文 16px，普通 MediaInfo 报告
- [x] 画面队列被时钟卡住时继续出画，暂停能唤醒解码线程，Present 不堵住界面
- [x] 循环播放按钮
- [x] 进度条拖动和点击都会 Seek，不再因为队列满了冲到结尾
- [x] 设置窗口可选择显卡，选择会记住并交给 Vulkan
- [x] 解码与 DX11 渲染按同一块显卡的 LUID 对齐

- [x] 打开素材时按当前显卡探测能否硬解，播放沿用探测结果

Release 可执行文件：`build/Release/ProResPlayer.exe`。
