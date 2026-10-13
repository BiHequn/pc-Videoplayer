# Native Player

Windows x64 原生播放器与本地传输演示，C++17 / Win32 / Dear ImGui / FFmpeg，未使用 Qt。

## 功能

| 功能 | 当前实现 |
| --- | --- |
| 本地播放 | 文件选择与拖放、暂停、停止、跳转预览、音量、全屏、置顶、重新播放 |
| 网络播放 | 直接 HLS / RTMP / HTTP 地址；缓冲状态、恢复出帧、停止中断网络读取 |
| 视频渲染 | OpenGL 3.3 三平面 YUV 纹理、着色器颜色转换、VBO/VAO/FBO；亮度、90°旋转、缩放、灰度与反色 |
| 音画同步 | WaveOut 声卡时钟；WOM_DONE 回调通知可复用缓冲区；视频早到等待、迟到丢帧 |
| 账号 | 本地 TCP JSON 服务，注册、登录、令牌会话、退出；PBKDF2-HMAC-SHA256 加盐存储 |
| 上传 | 256 KiB 分块、块和整文件 SHA-256、服务端落盘确认、暂停恢复、断线续传 |
| 连接 | 两秒心跳、五秒请求超时、1/2/4 秒重连、请求标识关联及重试、自动恢复会话 |
| 下载 | HTTP FLV 分块及 MPEG-TS HLS 分片并发；进度、速度、暂停、取消保存、进程退出后续传 |
| 边下边播 | FFmpeg 读取本机缓存 HTTP 端点；未缓存区间等待并提升下载优先级，不读取文件空洞 |
| 可视化 | 播放器、最近打开、设置、网络播放、账号与连接、传输中心；进度与错误显示 |

默认桌面渲染器为 OpenGL。`--renderer d3d11` 使用兼容渲染器，`--pip` 保留第一层画中画入口。Auto 解码在初始化或首帧失败时回退 FFmpeg；网络演示推荐指定 FFmpeg。MF 后端不保证实际使用硬件解码。

## 运行与本地演示

主程序：`bin/desktop/Release/native-player.exe`。服务程序：`bin/desktop/Release/native-service.exe`。

```powershell
& .\bin\desktop\Release\native-player.exe
& .\bin\desktop\Release\native-player.exe --source 'C:\Videos\sample.mp4' --decoder ffmpeg
& .\bin\desktop\Release\native-player.exe --renderer d3d11
& .\bin\desktop\Release\native-player.exe --pip --source 'C:\Videos\sample.mp4'
```

固定演示使用代码生成的视频和本机 HTTP 源，不依赖公网测试地址：

```powershell
.\scripts\run-second-layer-demo.ps1 -Build
```

脚本生成 FLV 与八分片 HLS，打开网络播放页并填入本机地址。点击“本地 FLV 示例”或“本地 HLS 示例”，再选择直接播放、边下边播或下载完成后播放。“账号与连接”页点击启动本地服务，注册后登录；“传输中心”选择文件上传，测试暂停和恢复。演示步骤见 [操作说明](docs/demo/second-layer.md)。

界面启动的 LocalServer 在后台线程运行，退出播放器后关闭；也可独立启动服务：

```powershell
& .\bin\desktop\Release\native-service.exe --port 18760 --data .\build\manual-service-data
& .\bin\desktop\Release\native-demo-http.exe .\build\demo-fixtures 18761
```

普通启动默认写入 `%LOCALAPPDATA%\NativePlayer`，包括账号、上传文件和缓存；`--data-root` 可修改数据目录。演示脚本使用工作区 `build/demo-data`。下载取消保留进度；上传取消删除未完成数据。自动重连保留会话；主动断开后需要重新登录。

快捷键：Ctrl+O 打开；空格暂停／继续；左右键跳转十秒；F11 全屏；Esc 退出全屏。

## 构建

需要 Visual Studio 2022 C++ 桌面组件、Windows SDK 和 CMake 3.21+。Dear ImGui v1.91.9b 核心及 Win32、DX11、OpenGL 后端已随源码提供并保留 MIT 许可证。FFmpeg 6.1.1 与 nlohmann-json 使用仓库 vcpkg 清单的 x64 静态依赖。

```powershell
.\setup-vcpkg.ps1
& .\.tools\vcpkg\vcpkg.exe install --triplet x64-windows-static
cmake -S . -B build/desktop -G 'Visual Studio 17 2022' -A x64
cmake --build build/desktop --config Release --parallel 4
```

CMake 自动识别正常与历史嵌套的 `vcpkg_installed`。其他布局用 `-DNP_DEPENDENCY_ROOT='C:/deps/x64-windows-static'`。也可打开 `Project1/Project1.sln`，选择 Release / x64；Visual Studio 主程序输出为 `bin/x64/Release/native-player.exe`，服务和固定媒体生成工具由 CMake 构建。

## 验证

```powershell
ctest --test-dir build/desktop -C Release --output-on-failure
cmake --build build/desktop --config Debug --parallel 4
ctest --test-dir build/desktop -C Debug --output-on-failure
```

测试使用真实 FFmpeg 解码、TCP/HTTP 请求、磁盘文件、音频设备及 OpenGL 像素读回。覆盖本地控制、YUV 生命周期、直接 HLS/RTMP、边下边播首帧、部分分片续传、下载进程强制退出、资源变化、连接释放、账号与上传恢复。桌面流程会自动完成播放与传输并保存程序自身的截图，不截取其他应用。

架构与接口见 [设计](docs/superpowers/specs/2026-10-13-second-layer-design.md)、[通信协议](docs/protocol/service-v1.md)。测试证据见 [第二层验证记录](docs/validation/2026-10-13-second-layer.md)。

## 边界

- 缓存模式支持有限、未加密、单音轨 MPEG-TS HLS 点播和可 Range 读取的 HTTP FLV。源必须提供强 ETag 或 Last-Modified；源变化使旧缓存失效。同一缓存目录只允许一个写任务。
- 每任务最多四个下载工作线程、512 KiB 分块检查点，进程强制终止后每个未完成工作块的重下载量小于 1 MiB。该验证覆盖进程退出，未验证机器断电或磁盘损坏。
- HLS 选择首个变体；多音轨、加密、fMP4、BYTERANGE、直播缓存、DRM 未实现。直接网络播放使用 FFmpeg 能力，兼容性仍受实际源限制。
- 0.5x/1.5x/2x 使用视频时钟并静音；1.0x 恢复音频。未实现音频变速。未增加 HDR 色调映射。
- 账号服务仅绑定本机，协议未增加 TLS、令牌过期和公网部署治理。账号与会话 JSON 是本地演示存储。
- 最近打开仅保留当前会话；中文字体使用 Windows 微软雅黑；底层仍为单播放实例。
- 直接 HLS 在跳转时可能输出 FFmpeg 中断/损坏包警告；已验证能恢复控制和出帧，未承诺所有源跳转无瞬时画面瑕疵。
- 第三层的发布打包、性能基准、长期压力测试和毕业设计文档尚需后续完成。
