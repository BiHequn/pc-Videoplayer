# 交付验收与脚本契约

第三层交付包括便携 Windows x64 ZIP、使用／架构／固定演示文档，以及真实验证与测量记录。打包成功只表示明确输入已复制并生成清单和 ZIP；运行通过必须有 `verify-release.ps1` 的实际退出结果与 JSON。本文不把待执行流程写成完成结论。

## 打包与验证命令

从仓库运行以下 PowerShell 命令。PowerShell 是维护人员打包／验证工具，正常演示用户双击 CMD 即可。

```powershell
.\scripts\package-release.ps1 -Version 3.0.0
.\scripts\verify-release.ps1 -Version 3.0.0 -RunId prepare -PrepareOnly
# 父任务完成最终构建、稳定性和性能测量后，再执行：
.\scripts\verify-release.ps1 -Version 3.0.0 -RunId final
# 正式性能数据已经完成时，在首次打包该版本前改用：
# .\scripts\package-release.ps1 -Version 3.0.0 -IncludePerformance
```

发布路径固定为 `build/release/native-player-<Version>-win-x64` 和同名 `.zip`。每个版本只允许创建一次，已有目录、ZIP 或 `.claim` 都拒绝覆盖；失败保留证据，没有自动递归删除。使用新的版本号重新打包，例如 `3.0.1`，不要复用已占用输出。版本限制为 `3.<数字>.<数字>`，可带小写预发布后缀；没有任意输出路径参数。

`package-release.ps1` 的输入固定为 `bin/desktop/Release/{native-player,native-service,native-demo-http}.exe`、`build/demo-fixtures/stream.flv`、有限八分片 HLS、四份 delivery 文档、协议和已有一／二层验证记录、实际许可证。`docs/validation/2026-10-13-third-layer.md` 与 `docs/performance/method.md` 若已经存在，按这两个确切路径复制其 Markdown。项目通过静态依赖与静态 MSVC runtime 构建，系统 DLL 和 Windows 字体不复制。

显式 `-IncludePerformance` 从 `build/third-layer/benchmark-results` 复制到包内 `docs/performance/results`，只接受测量环境 JSON 的 `completed=true`、正式测量用途和恰好三次重复。该选项可省略；一旦启用，以下 32 个文件全部必需：`environment.json`、`results.json/csv`、`summary.json/csv`、`index.html`、`cpu.svg`、`cores.svg`、`memory.svg`、`private.svg`、`first-frame.svg`、`fps.svg`、`sync.svg`、`render.svg`，以及 `raw/<720p|1080p|4k>-<native-yuv|bgra>-<1|2|3>.json` 的 18 个确切路径。缺失、部分结果或四次以上重复均拒绝，不截取前三次冒充完整数据。不会递归复制 fixtures、测试账号或其他 build 内容，也不会把 `benchmark-smoke` 当作正式结果。

性能核对包括原始记录与聚合 JSON／CSV、源和基准程序 SHA256、分辨率／编码／窗口／实际像素路径、串行时间范围、帧 serial 与计数、内存样本、CPU／首帧／分位数派生值、摘要中位数和比较条件，以及八张图的柱与散点坐标／标签和 HTML 数值。完整观察存在丢帧或漏帧时可以交付，但摘要必须真实标记 inconclusive，CPU reduction 为 null；只有源、程序、GL 条件一致且完整帧条件满足时才允许比较数值。包内副本在路径规范化后再执行相同核对。

测量 JSON／CSV 内已知素材和程序路径字段的绝对工作区路径改为 `workspace/<仓库相对路径>` 标签，数值和观察保持原样；越出工作区的路径或凭据字段拒绝。构建预检中的工具路径／命令行文本省略，保留时间、CPU 和进程标识，并记录原证据文件 SHA256。清单同时记录副本 SHA256、原始源文件 SHA256 和规范化说明，原始数据留在本机。SVG／HTML 原样复制，不制造测量值。

素材缺失时默认失败。只有显式 `-GenerateFixturesIfMissing` 且目标素材目录完全不存在时，才运行现有 `build/desktop/Release/make-stream-media.exe`；部分素材或已占用目录拒绝重写，避免改变其他演示和测试的源身份。脚本不运行构建命令。

可选 `-IncludeCommitMetadata` 记录 HEAD 和工作区是否有修改，不保存作者、远程 URL、个人目录或完整 Git 状态。它不能证明 EXE 由 HEAD 的纯净源码构建，尤其本轮明确不提交。schemaVersion=3 的 `manifest.json` 含包版本、平台、Release 标记、UTC 创建时间以及每个二进制／媒体／文档／启动器／许可证的相对路径、大小和 SHA256；复制前后核对源及副本，发现输入变化则失败。清单不包含自己哈希，其身份由验证报告记录，ZIP SHA256 由打包输出和验证报告记录。

许可证从 `third_party/imgui/LICENSE.txt` 与 CMake 实际选中的安装依赖目录 `share/{ffmpeg,nlohmann-json}/copyright` 复制，支持本项目历史嵌套安装布局。当前加载布局为 `vcpkg_installed/x64-windows-static/x64-windows-static`，FFmpeg 6.1.1 port 11 的 ABI features 为 avcodec、avformat、core、swresample、swscale；CMake 的 Release 链接输入为该目录下五个 `.lib`，运行库 `/MT`。每次包清单记录实际选中的相对依赖目录、ABI 文件 SHA256；从 EXE 或该 Release `avutil.lib` 中读取实际保留的配置字符串，只将 prefix、pkg-config 和 extra-ldflags 的历史构建机路径替换为明确省略标签，保留原配置字符串及所在文件 SHA256。找不到唯一字符串时明确记录未取得，不根据版本猜配置开关。

文件缺失即失败，不以自写摘要替代。包内 NOTICE 明确：当前包含实际许可证正文，但没有 FFmpeg 对应源代码、已履行的源码提供承诺或重新链接对象文件；后续分发者需处理适用的 LGPL 静态链接源码／重新链接要求及相应源码交付或提供途径。本轮报告这一交付边界，不声称公开分发法律义务已完成。

稳定性原始证据只接受 `docs/validation/stability/result.json`、`trajectory-verification.json`、`samples.csv`、`process-exit.json` 这四个确切文件。全部不存在时省略；存在任意一个就要求四个完整非空文件。打包与验证均要求长测 `passed=true`、`elapsedSeconds>=3600`、轨迹验证 `passed=true` 和实际 `exitCode=0`。父任务从完成的实际长测生成这些文件，将 fixture 路径标为 `workspace/<相对路径>`；脚本原样复制并记录 SHA256，不递归复制长测目录，也不生成或修改观测数据。校验这些完成条件与文件身份，不再次执行轨迹数值分析。

## 路径与数据安全

脚本将目标解析为当前脚本所属工作区的固定子路径，拒绝根路径、`..`、驱动器／ADS 名称、非法组件、Windows 设备名和 reparse point，包括已存在的祖先目录。输出采用 CreateNew 和版本／运行标识保留声明，拒绝占用；不接受强制覆盖或任意目录删除。输入采用文件白名单，既不遍历复制 build 数据目录，也不复制开发工具、个人媒体、测试账号、会话和 token。

验证解压前检查 ZIP entry 的相对路径、包名前缀、重复项、链接属性、条目数量和总展开大小，逐文件 CreateNew。暂存与解压目录都核对 manifest、SHA256 和大小，未列出的文件拒绝。测试前后再核对包身份，防止验证期间修改二进制而仍引用旧结果。验证进程只持有和终止自己启动的子进程句柄。

三个 EXE 用脚本只读解析 PE32+，必须是 AMD64，普通和延迟导入都核对显式 Windows 系统 DLL 白名单。MSVCP、VCRUNTIME、FFmpeg `av*.dll` 等未声明依赖拒绝。`manifest.json` 的 `peImports` 记录每个程序的实际导入名称；暂存与解压包重新解析并核对这些记录，无需 dumpbin、SDK 或构建工具。该检查覆盖静态导入表，不将动态加载的系统驱动和组件依赖当作已验证的干净机器环境。

## 隔离验证的证据定义

输出位于 `build/third-layer/package-verify/<Version>-<RunId>`，与发布暂存目录分开。`RunId` 只接受小写字母、数字和连字符；已有目录或声明拒绝复用。`-PrepareOnly` 解压并核对两个包，不启动应用；输出 `prepared=true`、`runtimeVerified=false`、`passed=false`，只能说明已准备。

正式运行对子进程清空继承环境，只提供 Windows 系统路径和验证专用的 TEMP、LOCALAPPDATA、APPDATA、USERPROFILE。PATH 仅为 Windows System32，cwd 同样为 System32，位于源码工作区外。可执行文件、媒体、数据与截图用完整路径传入。父脚本不依赖 CMake／CTest／FFmpeg CLI，子程序不依赖 PowerShell 或构建工具。

| 每个包副本的检查 | 必需证据 |
| --- | --- |
| `native-service --port 0 --data <隔离目录>` | 最多 30 秒启动与探测；读取实际随机端口，真实长度前缀 JSON heartbeat 与 BAD_VERSION 响应核对 version／id／op／code／body |
| `native-demo-http <包内media> 0` | 最多 30 秒启动与探测；实际随机端口，对 FLV、HLS 清单、TS 分片发 Range 请求，核对 206、Content-Length／Content-Range／ETag 和逐字节正文 |
| `native-player --source <包内FLV> --smoke-test <独立目录>` | 窗口实际运行，退出码 0，应用 JSON `passed=true`，五个命名阶段和五张 PNG |
| `native-player --demo-fixtures <包内media> --second-layer-smoke <独立目录>` | 窗口实际运行，退出码 0，JSON `passed=true`、`stages=15`、`segments=8`、`authenticated=true`，上传／缓存字节等于包内实际素材，十二张 PNG |
| 包身份复查 | 暂存与 ZIP 解压内容的清单／文件 SHA256 一致，测试前后 ZIP 和清单身份不变 |

两份副本共四次播放器窗口测试，按顺序执行。普通 smoke 最多等待 40 秒，十五阶段最多 125 秒；超时只结束本脚本创建的进程并保留记录。输出包括顶层 `result.json`、每次进程的 stdout／stderr、应用结果、截图和各自测试数据。只有所有项目实际完成后，顶层才写 `runtimeVerified=true` 和 `passed=true`。应用测试在第二层模式下会把数据根改为其 artifacts 下 `data`，该位置仍位于独立验证目录。

辅助服务探测成功后，脚本主动终止自己持有句柄的服务并等待实际退出，记录 `terminatedAfterProbe=true` 和退出码；这与播放器自行完成并退出码 0 的要求不同，不把服务探测后的终止写成优雅关闭通过。辅助探测不注册账号、不复制会话或凭据；测试服务数据留在隔离目录。

运行目录使用 `st/zp`、`ds/sl` 和 `a` 等短子目录，报告仍标记 staging／zip 与完整测试名称。账号上传路径会在数据根后附加 154 字符的 `\server\uploads\<64位账号标识>\<64位任务标识>.json.tmp`；建议实际数据根绝对路径不超过 90 字符、避免深层解压位置。经典 Win32 上传接口没有全应用扩展路径保证。rc3 首轮 `runtime` 在 stage 8 自行 exit 1，日志为 `IO_ERROR: Cannot open upload partial`，partial 完整路径 272 字符；缩短目录的 `short` 运行实际生成的 metadata 为 247 字符、`.tmp` 为 251 字符。该隔离验证覆盖这些实际短路径，不证明任意深度数据根可用。

打包诊断：rc1 在 PowerShell 5.1 遍历空对象属性时失败，rc2 将顶层性能摘要数组序列化为 Count／Length／SyncRoot 对象后被一致性检查拒绝。两者保留部分目录和声明，没有 manifest 或 ZIP。修复为逐属性取名、先识别数组并使用 `ConvertTo-Json -InputObject` 保留数组形状；rc3 完整 32 文件证据检查接受 4K inconclusive 和 null reduction。候选包只用于验证脚本与运行入口，正式发布须重新打包最终文档。

本机隔离目录验证继续使用原机器的 Windows、显卡驱动和音频设备，不能声称干净 VM、另一台电脑或全新 Windows 安装已验证。真实辅助请求覆盖本机协议与媒体 Range 入口，不替代人工固定地址演示或完整服务兼容矩阵。

## 验收矩阵与结果归属

| 项目 | 证据与判定范围 |
| --- | --- |
| 第一／二层播放、缓存、协议、上传和桌面回归 | 见 [第一层记录](../validation/2026-10-13-first-layer.md) 与 [第二层记录](../validation/2026-10-13-second-layer.md)，为已有历史证据 |
| 第三层 Release／Debug／VS 构建和完整 CTest | 由父任务最终验证记录给出命令、日期、退出码和日志，不能从打包成功推定 |
| 桌面状态、诊断和安全清理 | 由父任务集成后真实重启／文件异常／目录边界验证确定；接口实现不能替代最终窗口证据 |
| 至少 100 次操作与 60 分钟连续运行 | 使用真实 PlaybackSession，原始样本、开始／结束墙钟、实际持续时间和二进制身份；测试运行中不宣布完成 |
| 720p／1080p／4K YUV 与 BGRA 对照 | 同一媒体核心和 GPU 路径的实际 JSON／CSV／图表，记录素材、设备、重复次数和干扰条件；不预设哪条路径更快 |
| 真实故障场景 | 缓存根不可写、服务目录冲突、损坏媒体恢复、下载断连与继续，各自提供独立证据 |
| 便携暂存／解压运行 | 对应版本 `package-verify` 的顶层与应用 JSON、退出码、日志和截图 |
| 五至八分钟人工流程 | 见 [固定演示](demo.md)，实际操作后记录启动器、布局、数据根和恢复现象 |
| 干净机器、磁盘写满、断电或磁盘损坏 | 没有实际构造和执行的项目不列入通过项；干净 VM／另一台机器属于外部验收 |

最终验收由实际产物和相应验证记录决定。论文、PPT、新增硬解和启动器页面集成不在第三层 A/A 的交付范围。
