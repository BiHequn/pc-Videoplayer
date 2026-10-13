# 第一层播放器验收记录

日期：2026-10-13。范围：用户选择的 B，不限一小时，完成第一层桌面播放器。

## 实现与入口

采用 Windows x64 / C++17 / Win32 / Dear ImGui v1.91.9b / D3D11，复用现有 FFmpeg 6.1.1 和 Media Foundation 播放链路。

默认启动中文深色桌面窗口。主界面包括播放器、当前会话最近文件和播放设置；支持文件选择、拖放、暂停/继续、停止、重播、时间轴、音量、视频倍速、亮度、全屏、置顶和诊断信息。通过后台工作线程操作媒体后端，窗口显示打开、播放、暂停、正常结束和失败状态，失败后仍可重新打开文件。

运行文件：

- CMake Release：`bin/desktop/Release/native-player.exe`
- CMake Debug：`bin/desktop/Debug/native-player.exe`
- Visual Studio Release：`bin/x64/Release/native-player.exe`

```powershell
& .\bin\desktop\Release\native-player.exe
& .\bin\desktop\Release\native-player.exe --source 'C:\Videos\sample.mp4' --decoder auto
& .\bin\desktop\Release\native-player.exe --pip --source 'C:\Videos\sample.mp4' --decoder ffmpeg
```

## 最终验证结果

| 检查 | 结果 |
|---|---|
| CMake Release 完整构建 | 成功 |
| CMake Debug 完整构建 | 成功 |
| Visual Studio Release / x64 | 成功 |
| Release CTest | 4/4 通过，28.83 秒 |
| Debug CTest | 4/4 通过，31.23 秒 |
| 真实 1920×1080 H.264 MP4，Auto 窗口演示 | 退出码 0，五个状态通过，实际后端 Media Foundation |
| 原有 `--pip`，真实 MPEG4 + PCM 文件 | 退出码 0，收到 ready 和 ended |
| 项目源码空白检查 | 通过；暂存的原版 ImGui `imstb_rectpack.h:4` 含上游注释行尾空格，保留上游文件原样 |
| 可执行文件依赖检查 | FFmpeg / ImGui 已静态链接，未依赖其外部 DLL |

CTest 四个入口为媒体生成器、媒体控制、播放会话和桌面演示。媒体控制内部包含 22 项真实后端检查，使用四秒 MPEG4 / PCM 和无音频 AVI，25 FPS、GOP 75，覆盖音频/无音频时钟、绝对跳转、暂停预览、尾部跳转、PCM 裁剪、音量、取消重开、倍速结束和恢复正常速度、Auto 回退、不可寻址媒体的失败处理。会话检查包括新帧发布、错误恢复、暂停、尾部位置、快速 open/stop/open 和 2x 正常结束。

最终 Release 和 Debug 的恢复音频检查均记录：

```text
1x resync: anchor=2.013, returned clock=2.013, first audio clock=2.013
```

桌面演示运行真实 Win32/D3D11/ImGui 窗口，依次验证播放、暂停、暂停跳转、停止和打开失败。捕获程序自身 back buffer，检查中文文字、比例、控制栏和错误信息；不捕获其他程序。文件选择窗口另经实际窗口检查。

本机验证证据保存在忽略目录：

- `build/checks/release-ctest.log`、`build/checks/debug-ctest.log`
- `build/checks/pip-stdout.log`、`build/checks/pip-stderr.log`
- `build/desktop/screenshots/*.png` 和 `result.json`
- `build/desktop/h264-screenshots/*.png` 和 `result.json`

独立代码复查发现的工作线程初始化、取消丢失、未来帧阻挡错误、暂停预览、seek preroll、倍速音频背压、尾帧及恢复音频顺序问题均已处理。相应媒体和会话回归检查通过；最后的音频输出门用于先停止旧输出，再恢复音量、重建位置和允许新线程输出。

## 复现

```powershell
cmake -S . -B build/desktop -G 'Visual Studio 17 2022' -A x64
cmake --build build/desktop --config Release --parallel 4
ctest --test-dir build/desktop -C Release --output-on-failure
cmake --build build/desktop --config Debug --parallel 4
ctest --test-dir build/desktop -C Debug --output-on-failure
```

环境需要 Visual Studio C++ 工具、Windows SDK、静态 vcpkg 依赖和可用音频设备。测试会短暂播放合成音频并创建窗口。构建配置兼容正常与现有嵌套 vcpkg 安装目录。

## 当前界限

- 第一层渲染采用 D3D11，未实现第二层的 OpenGL、账号、分片上传、心跳重连和传输中心。
- 0.5x / 1.5x / 2x 使用视频时钟并静音；1.0x 恢复声音。未实现音频变速或保调。
- 最近文件只保留当前会话，中文字体来自 Windows 微软雅黑；桌面 UI 使用单播放实例后端。
- Media Foundation 为有效后端名称；本轮不据此保证硬件解码，也未做性能基准。
- 原画中画 stdin/stdout 事件入口保留；桌面窗口的页面 IPC 未扩展。
- 错误页面和正常结束分别验证；未做完整损坏文件矩阵、网络中断演示或安装包交付，后者属于后续层级。

代码在现有 `dev` 分支完成，既有项目修改保留。用户于 2026-10-13 授权提交并推送本轮工作；origin 已确认对应用户提供的 BiHequn/pc-Videoplayer。
