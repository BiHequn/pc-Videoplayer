# Native Player

Windows x64 原生桌面播放器。仓库：https://github.com/BiHequn/pc-Videoplayer

## 第一层功能

- Win32 + Dear ImGui 中文深色主界面，D3D11 视频绘制。
- 打开/拖放本地视频，最近打开列表（当前会话）。
- 播放/暂停、停止、重新播放、时间轴跳转、音量、亮度、全屏和窗口置顶。
- Auto / FFmpeg / Media Foundation 解码选择；Auto 初始化或首帧失败时回退软件解码。
- 后台播放线程；显示准备、打开、播放、暂停、结束和失败状态。错误后可重新选择文件。
- 播放技术信息：分辨率、解码后端、位置、同步差值、丢帧和界面帧率。
- 原有画中画入口通过 `--pip` 保留。

## 运行

CMake 输出：`bin/desktop/Release/native-player.exe`。
Visual Studio 输出：`bin/x64/Release/native-player.exe`。

```powershell
& .\bin\desktop\Release\native-player.exe
& .\bin\desktop\Release\native-player.exe --source 'C:\Videos\sample.mp4' --decoder ffmpeg
& .\bin\desktop\Release\native-player.exe --pip --source 'C:\Videos\sample.mp4'
```

快捷键：Ctrl+O 打开文件；空格播放/暂停；左右键跳转 10 秒；F11 全屏；Esc 退出全屏。

## 构建

需要 Visual Studio 2022 的“使用 C++ 的桌面开发”、Windows SDK 和 CMake 3.21+。
Dear ImGui v1.91.9b 的核心和 Win32/DX11 后端已放入 `third_party/imgui`，保留 MIT 许可证。
FFmpeg 6.1.1 和 nlohmann-json 由仓库的 vcpkg 清单管理，使用 x64 静态库。

```powershell
.\setup-vcpkg.ps1
& .\.tools\vcpkg\vcpkg.exe install --triplet x64-windows-static
cmake -S . -B build/desktop -G 'Visual Studio 17 2022' -A x64
cmake --build build/desktop --config Release --parallel 4
```

CMake 自动识别根目录下正常和历史嵌套的 `vcpkg_installed` 布局。其他位置可设置：

```powershell
cmake -S . -B build/desktop -A x64 -DNP_DEPENDENCY_ROOT='C:/deps/x64-windows-static'
```

也可打开 `Project1/Project1.sln`，选择 Release / x64。依赖已安装时可用：

```powershell
msbuild Project1/Project1.sln /p:Configuration=Release /p:Platform=x64 /p:VcpkgEnableManifest=false
```

## 测试

```powershell
ctest --test-dir build/desktop -C Release --output-on-failure
```

测试生成 4 秒的 MPEG4 + PCM 视频及无音频视频，使用较长关键帧间隔，覆盖真实解码、时长、绝对跳转、暂停预览、尾部跳转、音量、取消、重开、Auto 回退、无音频时钟、倍速切换和播放结束。测试会短暂调用实际音频设备。
`desktop-smoke` 使用真实 Win32/D3D11/ImGui 窗口完成播放、暂停、跳转预览、停止和错误状态，保存程序自身渲染画面到 `build/desktop/screenshots`，不截取其他桌面窗口。

本轮构建、测试结果和边界说明见 `docs/validation/2026-10-13-first-layer.md`。

## 当前范围

第一层使用 D3D11；OpenGL、账号服务、分片上传、心跳重连和下载中心属于第二层。
0.5x/1.5x/2x 沿用视频时钟与静音策略，1.0x 恢复声音，尚未实现音频变速。
最近打开记录仅保留当前会话；中文字体使用 Windows 自带微软雅黑。
播放线程使用现有单播放实例后端。界面不宣称当前 MF 解码器一定是硬件解码器。
旧画中画路径保留 stdin/stdout JSON；桌面窗口的页面 IPC 尚未扩展。
