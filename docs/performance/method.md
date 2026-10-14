# 播放性能测量方法

本基准比较同一 `PlaybackSession`、FFmpeg 软件解码器和 `OpenGLRenderer` 的两种像素输出路径：`native-yuv` 原生 YUV420P 与 `bgra` 解码器内 CPU BGRA 转换。未切换 D3D 渲染器，未修改生产代码。测量结果由实际 JSON 生成 CSV、SVG 和 HTML；没有预设优化幅度。

正式采样已完成并由父任务接受：**18/18 次、exit 0、每组 3 次、环境 completed=true**。720p 中位 CPU 降低 **13.514%**；1080p 为 **0%**；4K 因 observer 漏见帧为 **inconclusive**，不作优化结论。中断时间、原始来源与完整限制见末节。当前不再运行测试、preflight 或性能测量。

## 构建契约

父任务在根 CMake 的测试目标区域加入以下目标即可；公开依赖会传递媒体头文件、静态 FFmpeg、ImGui、OpenGL 和 Windows 库。

```cmake
add_executable(make-benchmark-media tests/make-benchmark-media.cpp)
target_link_libraries(make-benchmark-media PRIVATE player_media)
add_executable(player-benchmark tests/player-benchmark.cpp)
target_link_libraries(player-benchmark PRIVATE player_opengl psapi)
```

两个目标是普通 console executable，C++17、Windows x64，运行库与现有静态依赖一致（Release `/MT`、Debug `/MTd`）。不需要第三方 JSON、Python、ffmpeg.exe 或额外下载。`player_opengl` 的公开依赖含 `player_session` 和 `imgui`。正式运行使用 Release。独立 smoke 构建位于忽略目录 `build/third-layer/benchmark-build`，仅链接已有 `build/desktop/Release` 静态库；父任务集成目标后应重新构建，并让脚本记录最终可执行文件 SHA256。

基准不注册默认 CTest 性能测试。性能运行应显式进行；不得与并行构建混用。

## 夹具

```powershell
.\build\desktop\Release\make-benchmark-media.exe .\build\third-layer\benchmark-results\fixtures --seconds 4 --fps 30
```

生成 `720p.avi`（1280×720）、`1080p.avi`（1920×1080）、`4k.avi`（3840×2160），默认 4 秒、30 fps、120 帧，可设置 3–6 秒和 1–60 fps。真实容器分辨率由 FFmpeg 重开、探测并校验，不能只修改文件名或缩放同一解码输出。

使用本地 FFmpeg 内建 MPEG4 Part 2 编码器、AVI、YUV420P、固定量化系数 4、单编码线程、固定 GOP、无 B 帧、bitexact 标志。图像是按归一化坐标生成的移动梯度、条纹与棋盘格，使用确定整数计算，未使用外部素材、时间、音频或填充。固定参数与同一 FFmpeg 构建可重现文件哈希；不承诺不同 FFmpeg 版本产生相同编码字节。容器探测、文件长度和 SHA256 均保留。实际颜色元数据以探测和解码路径为准，AVI 不保证保留所有编码色彩标记。

这是一种合成负载，不能代表所有实际视频、码率和编码复杂度。每个分辨率使用不同的真实源尺寸，纹理也以源尺寸创建。

## 运行

父任务等待所有构建完成，再执行正式采样（默认每个分辨率每种模式各 3 次，总共 18 个新进程）。脚本在启动与每次播放前检查 `cl`、`link`、`MSBuild`、`cmake` 或 `ninja` 活动进程，发现后拒绝正式运行，不会停止其他进程；检查不能阻止其他进程稍后启动，最终运行负载仍需父任务保证。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run-performance.ps1 `
  -BuildDirectory .\build\desktop\Release `
  -OutputDirectory .\build\third-layer\benchmark-results -Repeats 3 -Seconds 4 -Fps 30
```

每轮交替先测 YUV/BGRA，偶数轮逆序分辨率；每个观测使用新进程和新 session，顺序执行。默认不显示 Win32 窗口。`-Visible` 可测显示窗口，但必须单独记录并保持比较条件一致。窗口 client area 固定 1280×720，生产 renderer 的视频处理纹理保持源分辨率，再由 ImGui 画到固定窗口。尝试将 swap interval 设为 0，并记录成功状态与实际值；不可用时值为 -999。

正式重复次数至少 3。小型验证：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run-performance.ps1 `
  -BuildDirectory .\build\third-layer\benchmark-build\out\Release `
  -OutputDirectory .\build\third-layer\benchmark-smoke -Smoke
```

Smoke 固定 720p、3 秒、10 fps、每模式 1 次，并在计时后生成截图。Smoke 标记为 harness validation，不能当作优化结论。`-ReuseFixtures` 复用文件，实际源属性仍以探测为准；请求参数和实际源参数可能不同。`-ChartsOnly` 从已存在的 `results.json` 与 `environment.json` 重建图表，不播放、不制造新数据。

单次运行接口：

```powershell
.\build\desktop\Release\player-benchmark.exe source.avi run.json native-yuv --repeat 1 --timeout 30
.\build\desktop\Release\player-benchmark.exe source.avi run.json bgra --repeat 1 --timeout 30
```

基准强制视频无音轨，避免与父任务音频 soak 争用音频设备。背景 soak 和其他负载仍可能影响 CPU/调度；如果正式采样时 soak 尚未结束，必须在结果解释中记录这一限制。正式性能采样与小型 smoke 的用途分开。

可通过 `-BackgroundNote 'Concurrent soak PID 40348: tiny video, muted WaveOut, 60-minute run'` 写入环境 JSON 与 HTML；脚本也读取启动时 `player-soak-test`、`native-player` 和已有 `player-benchmark` 的进程名/PID，不停止其他进程。

## 测量边界与指标

测试创建真实 Win32 `CS_OWNDC` window、WGL/OpenGL 3.3 context 和 ImGui context。先完成源探测、renderer 初始化和 ImGui 字体资源准备，再开始计时。计时从创建 `PlaybackSession` 前开始，包含 session 构造、open、软件解码、像素转换或原生拷贝、session 发布、renderer 上传和转换、ImGui 绘制、`SwapBuffers`、逐帧 `glFinish`、等待以及 session 销毁清理。计时后截图与 JSON 写入不计入 CPU/elapsed。

每个新的 session frame serial 调用一次真实 `updateVideo` 与 present，不重复计数旧帧。`glFinish` 使提交的 GL 工作完成，CPU 侧耗时包括驱动、swap 和等待；它不是 GPU timer query，也不是显示器呈现延迟。隐藏窗口的 SwapBuffers 无法证明屏幕上的帧实际显示。

| 字段 | 定义 |
| --- | --- |
| `process_cpu_seconds` | `GetProcessTimes` 的 kernel+user 增量，覆盖该进程所有线程 |
| `cpu_core_equivalents` | CPU 秒 / 实际墙钟秒；1 表示平均占用一个逻辑核 |
| `cpu_one_core_percent` | 上述数值 ×100，可能超过 100% |
| `cpu_machine_percent` | CPU 秒 / 墙钟秒 / `GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)` ×100 |
| `peak_rss_bytes` | 进程生命周期的 `PeakWorkingSetSize`，包含初始化；物理驻留工作集峰值 |
| `peak_private_commit_bytes` | `PeakPagefileUsage`，进程提交量峰值，包含初始化；不是物理 RSS |
| `peak_sampled_private_bytes` | `PrivateUsage` 采样最大值；约 10ms 采样并在新帧和结束取样，可能漏掉很短的峰值 |
| `first_frame_ms` | 计时起点到首次 update+present+glFinish 完成；含打开和 session 初始化，排除 WGL 初始化 |
| `unique_delivered_frames` | harness 观察并完成渲染的唯一 serial 数量 |
| `session_published_frames` | 新 session 的最大 serial；session 发布总帧数量 |
| `session_dropped_frames` | session 记录的迟到帧丢弃；不是 observer 漏见 |
| `observer_missed_frames` | serial 缺口，表示 frame latest-slot 在 harness 读取前已被更新 |
| `unaccounted_source_frames` | source_frames − published − session_dropped，非负；用于暴露计数边界差异 |
| `delivered_fps` | unique_delivered / 完整 elapsed，受首帧与清理影响 |
| `presentation_span_fps` | (unique_delivered−1) / 最后与首次完成 present 的时间差 |
| `sync_signed/abs_p50/p95_ms` | 每个唯一 observed frame 的 session `syncDifference` 有符号/绝对值分位数 |
| `render_complete_p50/p95_ms` | updateVideo 起点至 present+glFinish 完成的耗时 |
| `present_complete_p50/p95_ms` | updateVideo 结束至 present+glFinish 完成，含 ImGui 绘制 |

分位数采用 nearest rank（排序后 ceil(p×N)），首次 frame 的 sync 值可能为初始 0。`frame()` 和 `snapshot()` 独立加锁，sync 是临近该 frame 的 session 采样，而非该帧精确 PTS 与 clock 原子配对。视频无音频，所以 sync 是视频相对 session 的墙钟播放时钟差值，不是实测音画同步。

JSON 每帧保存 serial、完成时间、sync、render/present 完成耗时，约 10ms 保存 RSS/private 样本。进程内存峰值包含计量缓冲区，帧样本预先 reserve。OpenGL 驱动/GPU 显存不在进程 RSS 指标范围内。WMI 环境探测不可用时记录 `environment_probe_errors`，保留 OS 与处理器环境变量回退值；GPU 标识仍来自真实 `glGetString`，不能填入未取得的型号、驱动或物理内存。

## 结果与解释

输出目录包含 `raw/*.json`、聚合 `results.json`、`results.csv`、`environment.json`（CPU/GPU/OS/驱动/实际日期与程序和素材哈希）、`summary.json/csv`、8 张 SVG 与 `index.html`。图中柱为重复运行中位数，散点为每次观察值；保留异常结果，不选取最好一次。

摘要的 `native_cpu_reduction_percent = (BGRA中位CPU秒 − YUV中位CPU秒) / BGRA中位CPU秒 ×100`，正值才表示该样本 CPU 更低，负值表示更高。检查相同源、帧数、丢帧、observer misses、运行负载后再解释，不因 native YUV 的设计意图宣称必然优化。完整 elapsed 的均值核占用以及 CPU/已呈现帧也保留，以便发现耗时或实际完成工作量变化。

本子任务完成 harness、小型 smoke 与父任务授权的 18 次正式串行采样。没有测硬件解码、D3D、网络、音频、实际桌面交互、GPU timer、另一台机器或干净虚拟机。

## 已完成的 harness 验证（2026-10-13）

独立 Release 两目标编译通过。小型 smoke 使用 1280×720、3 秒、10 fps 的无音轨 MPEG4：YUV 与 BGRA 各观察并完成渲染 30 个唯一帧，session 丢帧、observer misses 和未对齐源帧计数均为 0。实际 elapsed 分别为 2.909140 和 2.909080 秒；源标称 3 秒与实际区间不是同一指标，最后一帧 PTS 是 2.9 秒，session 在视频尾结束后完成清理。

两次 CPU 秒分别为 0.125000 与 0.234375；首帧完成分别为 61.8151 与 39.8125ms。这里只保留实际 smoke 观察，单次短测、先测 YUV 的顺序、初始化与背景负载都限制解释，不能从这些数值得出正式优化结论。实际 GL 为 Intel Iris Xe，OpenGL 3.3.0，swap interval 0。WMI 在 sandbox 中拒绝访问，环境 JSON 明确保留探测错误；未伪造未取得的 CPU 型号、驱动或物理内存。

忽略目录 `build/third-layer/benchmark-smoke` 保留实际原始 JSON、聚合 JSON、CSV、SVG、HTML 和两模式截图。已重新从原始聚合数据生成图表，验证 2 条 CSV 记录、30 个逐帧样本/模式、8 个可解析 SVG，并查看真实截图。另以 3 秒、10 fps 生成三种尺寸并重开验证 1280×720、1920×1080、3840×2160，各 30 帧；两次同参数 720p 文件 SHA256 相同。

脚本增加背景活动记录后再次运行小型验证，保留于 `build/third-layer/benchmark-smoke-final`，两模式仍各 30 帧、0 session drops、0 observer misses。该次 CPU 秒 YUV/BGRA 为 0.140625/0.500000，elapsed 为 2.904414/2.927462 秒，首帧为 37.6648/37.3607ms。背景 note 指明父任务 soak PID 40348；已验证环境 JSON、CSV、8 张 SVG、HTML 背景说明和纯图表重建。两轮短 smoke 的 CPU 波动说明本机背景调度与短测开销影响明显，两轮均不构成正式 ≥3 次性能结果。

默认 4 秒、30 fps、120 帧夹具另保存在 `build/third-layer/benchmark-results/fixtures`，均已重开验证真实尺寸和无音轨。父任务可 `-ReuseFixtures` 使用这些文件：

| 文件 | 字节数 | SHA256 |
| --- | ---: | --- |
| 720p.avi | 3713642 | DDAA6237F4E6DA71352679B0AA911BA3926931476DA4542468B9FF8D2772B2E2 |
| 1080p.avi | 8049550 | CBB468346BE42D5B461E0BC8DF7ADF450F61F78A9253079603573A6FEC8A19D8 |
| 4k.avi | 18248596 | 2CA7F86877674800C189525578EFE966C79A58594D9F8B6DD264B3B343E73BAF |

正式默认预算：4 秒 × 18 次约 72 秒播放，夹具生成、WGL 和进程启动、WMI、idle 节点取证等待与图表开销另计。此次实测 18 次 elapsed 总和为 72.1185292 秒；中途等待构建和脚本修复的空档不属于播放 elapsed。父任务的 60 分钟 soak（进程 40348，320×180 MPEG4 视频 + muted PCM WaveOut）在背景继续，基准自身素材和播放均无音轨。

## 正式结果、来源与中断（2026-10-13）

正式结果位于 `build/third-layer/benchmark-results`：18 个原始 JSON、聚合 JSON、18 条 CSV、3 条分辨率摘要、8 张 SVG 和 HTML。每个分辨率/模式各 3 次，最终环境 `completed=true`。父任务负责把小型 CSV、摘要和图表复制到仓库文档交付目录；大型素材与完整逐帧数据留在忽略 build 目录，不在此文档中声称它们已经提交。

| 分辨率 | YUV CPU 中位秒 | BGRA CPU 中位秒 | 比较门控与结论 | YUV/BGRA RSS 中位 MiB |
| --- | ---: | ---: | --- | ---: |
| 1280×720 | 0.500000 | 0.578125 | 同源、同 GL、完整帧门控通过；YUV CPU 中位值低 13.514% | 117.008 / 147.352 |
| 1920×1080 | 0.843750 | 0.843750 | 门控通过；CPU 中位值相同，未观察到中位 CPU 改善 | 153.316 / 227.258 |
| 3840×2160 | 3.187500 | 4.890625 | inconclusive；只展示观察值，reduction 为 null | 330.938 / 605.438 |

三种尺寸均为 4 秒、30 fps、120 源帧、MPEG4/YUV420P/AVI、0 音轨。全部 session 发布 120 帧，session drops 和 unaccounted frames 均为 0。4K BGRA repeat 1、YUV repeat 3、BGRA repeat 3 各有 1 个 observer missed frame，因此这些运行分别呈现 119 帧；其余运行呈现 120 帧。保留这三次异常观察，不通过删除或重跑挑选结果。4K 不作 CPU 或内存优化结论。

比较门控要求每模式至少 3 次且重复编号唯一、完整会话、相同素材 SHA256/实际源尺寸/fps/源帧数/时长/编码/字节数、相同 benchmark SHA256、实际 GL vendor/renderer/version、窗口与 swap 设置；要求全部源帧呈现且 session drops、observer misses、unaccounted frames 均为 0。任何条件不满足或证据缺失时保留原始值并标记 inconclusive，不计算 reduction。门控规则以 18 个模拟输入测试验证，包含哈希、尺寸、fps、帧数、GPU/GL/swap 不同、丢帧/漏见、少于 3 次、会话未完成和缺少哈希的拒绝；模拟测试不是额外性能测量。现有 18 个真实 raw 字段与聚合数据逐字段匹配，CSV 为 18 条、分组为 6×3、8 个 SVG 可解析，已验证源哈希和帧账目一致。

实测设备为 Intel Core i7-12700H（14 物理核、20 逻辑处理器）、Windows 11 Home China 10.0.26200。系统枚举了 Intel Iris Xe 与 NVIDIA RTX 3060 Laptop，但全部实际 GL context 均为 **Intel Iris Xe**，GL `3.3.0 - Build 32.0.101.7085`、swap interval 0、固定 1280×720 隐藏窗口；不能把本次结果描述为 NVIDIA GPU 测量。背景 soak 和短素材/少量重复、跨暂停的机器温度/调度变化限制外推；门控通过只表示工作量与记录条件可比较，不表示统计显著或独占主机。

正式 benchmark Release SHA256：`3AACD098D4ACC26F29FEA8927E1D25649C809FFAE745C1DE1090EE2449CACFC8`。根构建 generator SHA256：`AC83707F322ED2F740CC100EFDA4AA4E2CE13ED83AB2C6A82ABEDEC09AB3F512`。素材哈希见上表；生成素材的独立 smoke generator 与根 target 属于不同可执行文件构建，素材本身的 SHA256 才是此次源身份。前后使用的 benchmark 二进制和素材哈希均未变化。

工具会话 59055 首先完成前三次，raw 计时分别为北京时间 **20:09:38.830–20:09:42.825**（720p YUV）、**20:09:48.048–20:09:52.041**（720p BGRA）、**20:09:57.971–20:10:01.973**（1080p YUV）。第 4 次前 preflight 检测 `MSBuild PID 8984 consumed CPU during observation; not proven idle`，脚本 exit 1。不是播放超时，前三次均正常结束；期间 CPU 从已取证的 0.562500 增至随后读取的 0.578125 秒，不能仅凭增量判断具体是构建还是节点维护。此前两次尚未启动播放的尝试也因 39472 控制台宿主 CPU 活动退出。

修复后的显式 `-IdleMSBuildIgnore '8984,39472,43564'` 只允许命令为 `/nodemode:1 /nodeReuse:true`、原父进程退出、无构建子进程、工作线程均在等待且两个观察区间 CPU 增量为 0 的节点。其原始 leaf System32 控制台宿主需单独验证身份和 CPU 静止；实际构建控制进程、cl/link/cmake/ninja、未知进程或 CIM 不可用仍会阻止采样。对已列明节点的瞬时 CPU/线程活动最多等待 45 秒后重新验证，不降低要求。11 个 preflight 规则测试和真实三节点取证验证通过。脚本不终止其他进程。

父任务完成 Debug/Release 构建并授权恢复后，工具会话 **29787** 用 `-Resume -ReuseFixtures` 验证不变的程序和源哈希，保留前三次，仅执行剩余 15 次。从第 4 次开始 raw 为 **20:19:36.848**，最后一次 raw 结束于 **20:20:43.707**；这 15 次 elapsed 总和为 **60.1298971 秒**，起末跨度 **66.859 秒**，会话最终在 **20:20:44.2369394** 写入 `completed=true` 并 exit 0。没有活动构建与这些播放区间并行；原 3 个 reusable 节点已不在采样期进程列表。原始 18 次的计时区间互不重叠。

父任务随后报告工具会话 **88437** 为“18 preserved”、exit 0；该调用只复用已有 18 次并重建报告，未新增 raw 或额外播放。其环境启动快照保留了末次 benchmark PID 46012，说明父任务控制脚本启动检查与末次播放尾部有编排重叠；未观察到两个测量子进程同时运行。最终 `environment.json` 的 `resume_utc` 已被该“全部保留”调用更新为约 20:20:44，不能作为其余 15 次的测量起点。测量来源以 raw 的 started/ended 与上述会话记录为准。整个 started_utc 到 ended_utc 跨度含 20:10–20:19 的中断，不应作为连续播放总时长。
