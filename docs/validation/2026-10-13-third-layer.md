# 第三层验证记录（2026-10-13）

范围为用户批准的 A/A：现有五项核心功能的本地毕业设计交付、持久化与诊断、异常恢复、性能测量、便携包和 Markdown 文档。新增硬件解码保证、启动器页面集成、论文和 PPT 不在本轮。长测和正式发布包隔离验证已完成，下述已取得结果均有实际运行证据。

## 桌面状态与下载恢复

实际启动 Release 桌面两次，分别保存并重新加载设置；两次五阶段窗口测试退出码均为 0。亮度 1.25、旋转 90°、缩放 1.5、灰度、音量 0.4、下载完成后播放模式以及最近本地路径跨进程保留。原始状态与截图在 `build/third-layer/desktop-relaunch-1`。

新增 `desktop-history` 通过独立媒体服务保持同一 URL：真实桌面边下边播后取消、退出，下一进程读取历史、恢复播放并清理所选缓存。第一次已下载 1376256 / 20837875 字节；第二次载入该历史字节数并出帧，确认清理后任务目录消失，最终状态没有复活已删除历史。针对性 CTest 连同素材生成 2/2 通过，8.49 秒。原始结果在 `build/desktop/desktop-history/Release/20261013-120602-*`。取消快照曾丢失进度，该流程先失败，再以保留现有快照修复后通过。

状态文件包含类型、范围、数量和大小校验；带 userinfo／query／fragment 的最近网络地址省略，下载历史标记需要原始源，并保留独立的 SHA256 `taskKey` 供清理使用。日志脱敏且有大小上限。缓存清理拒绝任务锁与目录链接，确认后停止当前播放及下载。最终文件测试共 13 组，覆盖并发读取／替换、共享冲突、非法字段、凭证、长根目录、junction、任务锁及按键清理。完整套件中 Release 1.89 秒、Debug 18.90 秒通过。损坏配置的读取诊断在首次自动保存后仍显示，实际窗口结果位于 `build/third-layer/desktop-corrupt-state`。

最终历史测试还对照实际 HTTP Range 与复制的 `state.json` 分块检查点。例如恢复请求从 327680、851968、1376256、1900544 等块内部偏移继续；删除检查点的负例被拒绝。再注入一个显示地址相同、taskKey 不同的历史与缓存 sentinel，验证只删除所选任务，另一任务记录与 sentinel 保留。最后的定向 Release／Debug CTest 各 2/2 通过（含素材生成，6.80／6.50 秒），日志为 `build/third-layer/history-final-{release,debug}.log`。这次定向测试在完整套件之后，验证最后的历史移除修正。

用户要求提交推送后，提交前 Release 复测出现历史测试误报：seed 恰好保留四个完整块，共 2,097,152 字节，没有部分块；resume 载入相同字节数，HTTP Range 全部从未完成块开始，清理成功。旧断言仅接受块内部偏移，因而该轮 20/21，197.43 秒。修正仅涉及测试脚本：每个恢复 Range 仍必须精确匹配 durable offset，并要求至少一次实际请求以及部分块续传或跳过完整块。保留的部分块与完整块日志都通过新判定，从零开始的伪恢复都被拒绝；真实 `-DiscardCheckpoint` 负例同样被拒绝。播放器二进制及已验证 ZIP 未改变。

修正后的提交前 Release 完整复测 **21/21 通过，194.82 秒**，日志与 JUnit 在 `build/third-layer/precommit-release.{log,xml}`，去除主机名与 stdout／stderr 的 JUnit 副本为 [precommit-release.xml](test-results/precommit-release.xml)。稳定性原始 CSV／JSON 用 `.gitattributes` 禁止换行转换，暂存 Git blob 与本机文件逐字节一致，防止检出后证据 SHA256 失效。

## 真实故障

`delivery-fault` 使用实际文件与 HTTP socket：损坏 AVI 进入可见错误，重新打开有效视频恢复播放；把缓存根路径设为普通文件阻止创建目录，得到错误；8 MiB 确定数据源在部分传输后中断，离线后恢复并逐字节核对完整结果。Release 针对性测试 2/2 通过（含素材生成，7.07 秒），Debug 连同短长测 3/3 通过（13.71 秒）。详细 JSON 位于 `build/desktop/delivery-fault/{Release,Debug}`。

路径阻塞测试不等同于拒绝访问 ACL，也不等同于磁盘写满。原有服务目录独占、账号、上传断点、校验标识变化与下载进程退出测试由最终完整套件再次验证。

## 60 分钟媒体长测

Release `player-soak-test` SHA256：`252AD2464D4ED2F3BCB8BD60206B46217AE602916BB392D1759999641FD2363A`，结束后再次核对未变化。进程 PID 40348，2026-10-13 19:43:34（北京时间）启动；100 轮打开／暂停／跳转／继续／停止共 500 操作，耗时 28.920 秒。连续循环从 19:44:04 至 20:44:04，实际持续 **3600.100 秒**，最终 `result.json passed=true`，stdout 有对应 PASS 行。

测试使用 320×180 / 25fps MPEG4 和 48kHz 单声道 PCM，真实 PlaybackSession 与 WaveOut 回调，音量设为 0。采集进程 CPU、RSS、private commit、句柄、线程、观察帧、循环次数与同步差值。它没有创建完整桌面或 OpenGL 窗口。其他构建及测试在本机并行进行，会影响 CPU、RSS 和调度。

独立 `verify-soak.ps1` 校验 3561 条样本覆盖 3600.02 秒，帧和循环单调持续推进：观察 78481 帧、851 次循环，最大帧进度间隔 1.185 秒，最大循环间隔 5.060 秒。最终与 CSV 的计数一致；无进度、采样长缺口和计数错配会被拒绝。私有内存起始 29,351,936、峰值 29,425,664、末尾 29,093,888 字节；句柄 322→319，线程 16→12。RSS 起始 24,240,128、峰值 103,723,008、末尾 14,716,928 字节，存在暂态波动，未观察到持续增长。进程累计 CPU 460.047 秒，20 逻辑核归一化占用约 0.639%。同步快照最大绝对差 0.08 秒，不能作为精密音画测量。

监控脚本于 20:44:04 观察到进程退出，但 `Get-Process` 对象的 `ExitCode` 返回 null，没有保留实际数字退出码。因此这里只确认最终报告和持续进度通过、进程已结束，**不声称观察到退出码 0**。打包接受带明确原因的 unavailable 记录，仍拒绝实际非零退出码、无说明的 null、未满时长或证据哈希不符。

原始 `launch.json`、`samples.csv`、stdout／stderr、`result.json` 和 `process-exit.json` 位于 `build/third-layer/soak-60min`。[入库样本](stability/samples.csv)逐字节保留；[结果](stability/result.json)只把素材绝对路径转换为 workspace 相对表示，保留 `originalResultSHA256`；[轨迹验收](stability/trajectory-verification.json)对入库结果重新计算哈希；[退出观察](stability/process-exit.json)保留 null 与原始哈希并补充限制说明。

## 性能对照

真实 720p／1080p／4K 合成 MPEG4 视频，每源 4 秒、30fps、120 帧，无音轨；同一 FFmpeg 软件解码与 OpenGL Renderer 对照 native YUV 和 CPU BGRA，每模式每尺寸三次。正式 18/18 轮完成，播放 elapsed 总和 72.119 秒。前三次于北京时间 20:09:38–20:10:01 完成；预检发现空闲 MSBuild 节点 CPU 活动后中止，保留结果。构建停止后，剩余 15 次于 20:19:36–20:20:43 执行，程序和素材哈希未变化。约九分钟中断不计入播放耗时。

| 真实源 | YUV CPU 中位秒 | BGRA CPU 中位秒 | YUV／BGRA RSS 中位 MiB | 结论 |
| --- | ---: | ---: | ---: | --- |
| 1280×720 | 0.500000 | 0.578125 | 117.008／147.352 | 可比工作量，CPU 中位耗时低 13.514% |
| 1920×1080 | 0.843750 | 0.843750 | 153.316／227.258 | 可比工作量，CPU 中位耗时相同 |
| 3840×2160 | 3.187500 | 4.890625 | 330.938／605.438 | 不可下优化结论，保留观察值 |

全部 session 发布 120 帧，session drops 为零；4K 的三次运行各有一个 observer missed，实际完成渲染 119 帧，其余为 120。对照门控要求相同源／程序哈希、GL／窗口／swap、完整帧计数和零遗漏，故 4K reduction=null。未删掉异常值，也未通过重跑挑选最佳结果。背景 320×180 音频长测仍在运行，短素材、三次重复和机器调度限制外推，不表示统计显著或所有视频的提升。

实际测量 GL 使用 Intel Iris Xe，OpenGL 3.3.0、驱动 32.0.101.7085；CPU i7-12700H，20 逻辑处理器；系统 Windows 11 10.0.26200。机器另有 NVIDIA GPU，但本次 GL context 没有使用它。正式 JSON／CSV／8 张 SVG／HTML 与 18 个 raw 位于 `build/third-layer/benchmark-results`，发布包含完整副本。

CPU 采用进程 kernel+user 秒；机器占用按逻辑核数归一化，内存峰值来自 Windows 进程指标。首次帧、观察帧、session 丢帧、observer misses、GL 完成时间及同步分位数均有原始样本。无音轨同步为视频相对墙钟，不代表音画精度。WGL 初始化排除在播放计时外，隐藏窗口的 present+glFinish 不等于显示器呈现延迟。见 [测量方法](../performance/method.md)。

## 构建、整体验证与发布

根 CMake 全部 Release／Debug 目标和 Visual Studio 解决方案 Release/x64 构建均 exit 0。完整 CTest：Release **21/21**，191.47 秒；Debug **21/21**，254.92 秒。包括五／十五阶段桌面、OpenGL 像素读回、本地／HLS／RTMP、上传与会话、下载终止／校验变化、故障、状态及短长测。日志和 JUnit 为 `build/third-layer/{release,debug}-final.{log,xml}`，详细 stdout 为 `*-final-detail.log`。最后的任务标识清理修正重新构建两配置和 VS，运行上述定向历史测试通过；未无理由重跑所有媒体测试。

仓库保留去除主机名与 stdout／stderr 的 JUnit 摘要于 `docs/validation/test-results`；测试项、状态与耗时未改变。性能 CSV 和摘要／SVG 位于 [实测摘要](../performance/results/README.md)，完整原始数据在本机和发布包中。

发布候选 `3.0.0-rc3` ZIP 于 20:38:51 完成实际隔离运行，顶层 `prepared=true`、`runtimeVerified=true`、`passed=true`，八项均通过：暂存／解压包各两个独立服务请求、五阶段及十五阶段桌面。独立服务绑定 OS 分配端口，验证 TCP JSON 协议和 HTTP Range 字节后结束所持有进程；这一终止不声称服务优雅退出。两轮十五阶段的 HLS 字节数 14471300、上传字节数 20837875，认证 true、分片八个。原始 JSON／截图／stdout 在 `build/third-layer/package-verify/3.0.0-rc3-short`。

正式 **3.0.0** 于北京时间 **20:51:21–20:53:10** 再次完成暂存与 ZIP 解压副本的 **8/8** 实际检查，顶层 `prepared=true`、`runtimeVerified=true`、`passed=true`。四次播放器窗口均自行退出，实际退出码 0；两轮十五阶段的账号认证、八分片 HLS、上传 20,837,875 字节和缓存 14,471,300 字节均核对通过。独立辅助服务在探测后由验收脚本终止，记录 -1，不能列为优雅退出。运行报告在 `build/third-layer/package-verify/3.0.0-final/result.json`，所有文件和 ZIP 在运行后重新核对身份。

正式包：`build/release/native-player-3.0.0-win-x64.zip`，**40,271,603 字节**、清单 **65 文件**，包含完整性能数据与四份稳定性证据。SHA256：`74d84a2cee312d21e1bddaaaca23d2e170fcb44338ecdcb9c5dae2d7b1a90cc5`。稳定性打包 admission 另有 12 项检查，确认实际带说明的 null 可以保留，并拒绝无说明 null、非零退出码、样本篡改、时长不足及提前退出观察。

ZIP 内的本页为打包时快照，正式包运行验收发生在打包之后。本仓库本页补充最终结果，ZIP 保持已验证字节不再修改；最终验收依据上述外部运行报告及 SHA256。

打包输入限于三个 Release EXE、生成演示素材、文档、测量结果及实际许可证，不包含账号、token 和运行数据。校验三个 EXE 的 x64 PE 普通／延迟 DLL 导入，拒绝外部 FFmpeg／MSVC runtime 依赖；静态配置来源另有构建记录。所有包运行使用 System32 cwd、仅系统 PATH 和验证独立 TEMP／LOCALAPPDATA，未使用源码 cwd 或开发工具 PATH。

首次候选运行的两个独立服务与五阶段桌面通过，十五阶段在上传初始化失败；诊断为 `Cannot open upload partial`。服务上传路径附加两段 64 字符哈希，深层验证目录超过传统 Win32 路径长度。验证目录随后改用 `st/zp`、`ds/sl`、`a` 短路径，继续使用隔离数据与 System32 cwd。应用并未增加全链路 extended-path 支持；真实使用应解压到较短路径，避免深层 `--data-root`。这一限制没有被准备验证掩盖，后续实际运行结果单独记录。

这一环境仍是开发机器原来的 Windows、显卡和音频驱动；未验证另一台电脑或干净 Windows VM。包内有 FFmpeg 实际许可证，但对应源代码／重新链接材料未交付，不能据此声称公开分发义务已满足。
