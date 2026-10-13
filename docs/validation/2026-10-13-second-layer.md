# 第二层完整功能验收记录

日期：2026-10-13。范围：Windows x64 原生可视化播放器第二层，以及用户追加批准的 FLV／HLS 边下边播、缓存和断点续传。本轮以作品集／毕业设计本地演示为目标。

## 实现范围

采用 C++17、Win32、Dear ImGui v1.91.9b、FFmpeg 6.1.1、OpenGL 3.3、WaveOut、WinHTTP、Winsock 和 BCrypt，未使用 Qt。

| 核心功能 | 实现与验证 |
| --- | --- |
| 播放与同步 | 本地文件、直接 HTTP FLV／HLS／RTMP；FFmpeg 解封装与解码；以 WaveOut 设备时钟同步视频，WOM_DONE 回调通知已完成音频缓冲区；缓冲、恢复及音频尾部正常结束 |
| 账号与协议 | 本机 TCP 长连接，四字节长度加 JSON；注册、登录、会话恢复、退出、请求标识和异常响应；密码使用随机盐及 PBKDF2-HMAC-SHA256 |
| 分片上传 | 256 KiB 块，块及整文件 SHA-256 校验，刷盘后确认偏移，暂停／恢复、服务重启和新客户端续传 |
| OpenGL | 三平面 YUV 纹理、着色器 YUV→RGB、VBO／VAO／FBO；旋转、缩放、亮度、灰度和反色；真实 GPU 像素读回 |
| 稳定性 | 两秒心跳、五秒请求期限、1／2／4 秒重连；断线保留会话、上传恢复；业务失败与连接失败分别显示 |
| 下载与缓存 | HTTP FLV 分块及 MPEG-TS HLS 分片并发；四个工作线程、512 KiB 检查点；暂停、取消保存、进程退出后续传 |
| 可视化 | 播放器、最近打开、设置、网络播放、账号与连接、传输中心；三种网络播放模式、下载与上传进度、错误和连接状态 |

默认桌面渲染为 OpenGL，`--renderer d3d11` 保留兼容入口；原有 `--pip` 使用原画中画入口。界面所拥有的 LocalServer 在后台运行并在播放器退出时关闭，另外提供独立 `native-service.exe`。

## 最终构建与测试

在 `C:/hequnbi/workspace/pc-Videoplayer` 执行：

```powershell
cmake --build build/desktop --config Release --parallel 4
cmake --build build/desktop --config Debug --parallel 4
ctest --test-dir build/desktop -C Release --output-on-failure --output-log build/second-layer/release-final.log --output-junit C:/hequnbi/workspace/pc-Videoplayer/build/second-layer/release-final.xml
ctest --test-dir build/desktop -C Debug --output-on-failure --output-log build/second-layer/debug-final.log --output-junit C:/hequnbi/workspace/pc-Videoplayer/build/second-layer/debug-final.xml
& 'C:/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe' Project1/Project1.sln /p:Configuration=Release /p:Platform=x64 /p:VcpkgEnableManifest=false /m:4 /verbosity:minimal
```

首次配置用 `cmake -S . -B build/desktop -A x64`，依赖安装见 README。JUnit 文件使用绝对路径，避免 CTest 将相对路径解析到测试构建目录下。

| 检查 | 最终结果 |
| --- | --- |
| CMake Release 完整构建 | 成功，退出码 0 |
| CMake Debug 完整构建 | 成功，退出码 0 |
| Visual Studio Release／x64 | 成功，退出码 0 |
| Release CTest | 17／17，通过，185.13 秒 |
| Debug CTest | 17／17，通过，232.67 秒 |
| Release 桌面完整演示 | 15 阶段通过，36.44 秒 |
| Debug 桌面完整演示 | 15 阶段通过，36.73 秒 |
| 一键演示脚本 | 实际生成 FLV 和八分片 HLS、打开网络页窗口，正常关闭 |
| 原画中画入口 | 实际 MPEG4／PCM 文件，退出码 0，分别收到 ready 与 ended |

17 个 CTest 条目：cache-robustness、generate-fixtures、audio-callback、cache-controller、generate-stream-fixtures、network-session、service-integration、native-yuv、opengl-renderer、frame-copy、cache-integration、cache-playback、media-controls、playback-session、desktop-smoke、desktop-d3d11-smoke、second-layer-desktop。素材生成是其中两个条目；部分测试包含多个场景，不能把条目数量当作全部用例数量。

最终日志保存到 `build/second-layer/{release,debug}-final.log`、对应 `.xml` 和 `*-final-detail.log`；画中画日志为 `pip-final-stdout.log`、`pip-final-stderr.log`。这些本机生成文件不提交到 Git。

## 边下边播与恢复证据

固定素材为代码生成的 320×180、25 fps、12 秒视频；FLV 含 FLV1 视频与 44.1 kHz PCM 音频，20,837,875 字节；HLS 为静音 MPEG-2 TS 点播，八个分片、14,471,300 字节。测试 HTTP 源限速并记录真实 Range 请求。

| 场景 | Release 观测 | Debug 观测 |
| --- | --- | --- |
| FLV 缓存首帧 | 1,984 ms；已下载 2,097,152／20,837,875 字节 | 2,043 ms；同一下载量 |
| HLS 缓存首帧 | 1,494 ms；已下载 1,567,848／14,471,300 字节 | 1,470 ms；已下载 1,502,312 字节 |
| FLV 强制退出恢复 | 持久化 2,097,152 字节，4 个重放块，单块最大重下载 90,112 字节 | 持久化 2,097,152 字节，4 个重放块，最大 98,304 字节 |
| HLS 强制退出恢复 | 持久化 2,614,228 字节，3 个重放块，单块最大重下载 270,336 字节 | 持久化 2,354,280 字节，4 个重放块，最大 262,144 字节 |
| 直接 FLV 音频尾部 | 最后一帧位置 11.96；释放尾部后 93 ms 进入 Ended，回调 299→300 | 126 ms，回调 299→300 |
| 活跃上传重连 | 恢复并完成 25,165,865 字节，2.418 秒 | 9.660 秒 |

这些时间是测试运行的观测值，不是性能基准。下载强制退出针对测试所创建并持有句柄的子进程；重启时比对持久化偏移及 origin Range 日志，最后逐字节核对 FLV 与所有 TS 分片。最大重下载量按源已经成功写入套接字的重叠字节计算，包含可能仍在套接字缓冲区内的数据。

Release 崩溃证据位于 `build/desktop/cache-playback/28632-103976843/{crash-flv,crash-hls}/crash-proof.json`，Debug 位于 `build/desktop/cache-playback/32568-104214359/{crash-flv,crash-hls}/crash-proof.json`。这验证进程异常退出后每个活跃工作块小于 1 MiB 的重下载量；未验证机器断电、磁盘损坏或文件被外部修改。

其他真实场景覆盖 HLS 片内偏移恢复、缺失区间等待和取消释放、完整文件重新打开后的 300 帧播放结束，以及暂停跳转预览。缓存读取只暴露已写入的区间，预分配文件空洞不当作可播放内容。

## 界面及渲染验证

完整桌面演示覆盖 FLV 首帧早于下载完成、下载暂停／取消／恢复、注册登录、上传暂停／完成、服务重启／会话恢复、HLS 完整下载后自动播放。两个配置的 `result.json` 均报告 `passed=true`、`stages=15`、`segments=8`、`authenticated=true`，上传完成量 20,837,875 字节，HLS 缓存完成量 14,471,300 字节。

截图位于 `build/desktop/second-layer-artifacts/{Release,Debug}`。已检查播放器、账号、传输完成、HLS 下载中和完成后播放截图的中文布局与状态显示；素材画面使用合成彩色图案。OpenGL 实际驱动为 Intel Iris Xe，GL 3.3.0，Build 32.0.101.7085。

OpenGL 测试读取实际 GPU 结果，校验 BGRA 和 YUV、有限／全范围、BT.601／709、奇数尺寸、填充步长、旋转、缩放、滤镜、亮度、截图方向、清屏及上下文重新初始化。原生 YUV 测试使用真实 FFV1 包检查像素和帧生命周期；队列与会话复制测试覆盖负步长、色彩元数据和持有帧不变性。

音频测试需要可用声音设备，并验证实际 WOM_DONE 回调与设备时钟。网络音频测试保持 1×，网络停顿时检查播放位置和回调停止，恢复后检查音频及视频继续；最后视频结束后只释放尾部音频，也能自然结束。

## 复查与修正

独立分模块复查和真实回归处理了以下问题：

- 旧取消任务收尾可能清除新任务的准备状态；现在按任务 generation 核对后清理。隔离副本移除修复会在真实阻塞网络场景失败。
- 任务 generation 与缓存快照分开读取可能打开旧播放地址；现在在同一锁内获取。
- 取消尚在准备阶段的任务不能只中断已发布实例；待启动缓存也保存取消信号。
- 资源长度或校验标识变化必须在读取响应正文前拒绝，避免旧缓存混入新资源字节；503 等临时错误保留重试。
- 已完成 HTTP 连接需要回收线程；等待缓存的读者主动断开后也应及时清除。
- 服务数据目录需跨端口、跨进程独占，避免账号和会话记录互相覆盖；使用操作系统独占文件句柄。
- 上传收到 UNAUTHORIZED 后清除账号状态；取消响应保留服务端 Completed 或 IO_ERROR。
- 视频最后一个缓冲停顿可能留下暂停的音频尾部；视频结束后清除缓冲暂停并等待音频完成。隔离副本移除修复会卡在预期的 EOF 回归断言。
- 帧拷贝保留三平面和合法负步长，尾帧预览采用同一复制路径。

缓存健壮性测试的九个真实场景在 Release／Debug 均全部通过：资源长度变化、ETag 变化、503 重试、短时间元数据读者、尾部 Range 与非法 Range、读者中断、连接回收、同缓存目录排他和缺失可靠校验标识拒绝。

此前一次 Debug 全量测试在旧 35 秒上传完成等待处失败，随后专项及最终全量均未复现，原始原因尚未确认。现在测试区分终止失败、20 秒无确认进度和 75 秒总期限，并输出连接／认证／重连次数／上传偏移／速率／错误诊断；生产请求与重连期限没有放宽。原始日志保留在 `build/second-layer/debug-suite-before-service-fix.log`。

## 可运行入口与边界

```powershell
.\scripts\run-second-layer-demo.ps1
& .\bin\desktop\Release\native-player.exe
& .\bin\desktop\Release\native-player.exe --renderer d3d11
& .\bin\desktop\Release\native-service.exe --port 18760 --data .\build\manual-service-data
```

具体操作及固定端口跨进程续传演示见 `docs/demo/second-layer.md`，协议见 `docs/protocol/service-v1.md`。普通演示 HTTP 源使用随机端口；跨进程续传应保持相同 URL、相同媒体源和数据目录，使用独立固定端口源。再次生成素材会改变源标识，旧缓存不再有效。

缓存范围为未加密、有限、单音轨 MPEG-TS HLS 点播与可靠 Range HTTP FLV，源需要强 ETag 或 Last-Modified。HLS 选择首个变体；加密、多音轨、fMP4、BYTERANGE、直播缓存、DRM 尚未实现。直接网络播放兼容性受 FFmpeg 和实际源影响；直接 HLS 跳转可产生包损坏／中断警告，已验证后续恢复，未保证所有源跳转无画面瑕疵。RTMP 验证素材为视频流，未将它扩展为 RTMP 音频兼容矩阵。

倍速仍以视频时钟播放并静音，1×恢复音频，未增加音频变速或 HDR 色调映射。Media Foundation 后端不保证实际硬解。账号服务仅绑定本机，没有 TLS、令牌过期或公网生产部署机制。

第三层的安装包、性能基准、长期压力测试、毕业设计论文及完整异常矩阵不属于本次完成范围。第一层原有画中画事件入口保留，桌面页面 IPC 没有在本轮扩展。
