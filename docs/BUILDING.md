# 构建与 ABI 兼容策略 / Building and ABI compatibility

## 兼容边界

插件按目标 OBS/libobs API、FFmpeg ABI、操作系统和架构构建并验证。不要把
`buildspec.json` 的插件版本号当作运行时依赖版本；它继续由项目维护者管理。
macOS/Windows CI 的依赖版本和哈希以该文件为准，Linux 以实际安装的开发包为准。
当前仓库没有一个覆盖所有 OBS/FFmpeg 版本的已验证兼容矩阵。

FFmpeg 始终直接链接，所有发现路径通过 imported targets 传递头文件与库。
`libavcodec`、`libavutil` 的头文件与二进制必须来自配套 SDK，各库 ABI major
必须与目标运行库对应；较旧运行库即使 major 相同，也可能缺少构建时使用的符号。
同 major 是必要条件，不代替目标 OBS 上的加载、编码和 SEI 验证。
更换 ABI/架构后使用新构建目录重新配置，不通过重命名 DLL/SONAME 绕过版本约束。

`SEI_TIMESTAMP_DYNAMIC_FFMPEG=ON` 在 bootstrap 前报错；旧的
`src/ffmpeg-bind.*` 未编译、未调用，不构成跨版本支持。默认 OFF 保持直接链接。
插件加载时记录编译时 OBS API、FFmpeg 头文件版本和实际 FFmpeg 运行库版本；
若 avcodec 或 avutil major 不匹配，在注册编码器前拒绝加载。
动态加载器仍可能在该检查之前因缺库或缺符号而失败。

配置日志记录 libobs package/手动库路径、FFmpeg pkg-config 版本与库路径，
或显式 SDK 的头文件和库路径。发布验证应保留 configure/build/OBS 日志和
`CMakeCache.txt`；不改变现有 CI 产物命名或向 buildspec 添加未定义字段。

## CI：保持模板路径

`SEI_TIMESTAMP_BOOTSTRAP_OBS=ON` 是默认值。继续使用平台 compiler/defaults/helpers、
`find_package(libobs REQUIRED)` 和 `OBS::libobs`。
macOS/Windows 的 obs-deps/bootstrap、presets、workflow 和打包规则不变。
Linux 模板查找已安装的 libobs；干净 Linux 环境需要实际提供开发包
（例如匹配版本的 `libobs-dev`、`libavcodec-dev`、`libavutil-dev`、SIMDe）。
本次未修改 CI 的包安装列表，也未声称已验证干净 Linux runner。

现有构建入口：`.github/scripts/build-macos`、
`.github/scripts/build-ubuntu --target ubuntu-x86_64`、
`.github/scripts/Build-Windows.ps1`。这些入口可能安装依赖并修改构建号。
直接使用 preset 时，平台条件和依赖准备要求仍然适用。

## 本地：优先导出的 SDK package

要求 CMake >= 3.28。以下路径均为占位路径，应指向同一目标 OBS 的配套 SDK。

```sh
cmake -S . -B build-local -DSEI_TIMESTAMP_BOOTSTRAP_OBS=OFF \
  -DOBS_SDK_PREFIX=/path/to/obs-sdk \
  -DFFMPEG_SDK_PREFIX=/path/to/ffmpeg-sdk
cmake --build build-local --config RelWithDebInfo
ctest --test-dir build-local -C RelWithDebInfo --output-on-failure
```

三平台都可以用 `CMAKE_PREFIX_PATH`（多个前缀用分号分隔）或直接设置
`libobs_DIR=/path/to/directory-containing-libobsConfig.cmake`。
`OBS_SDK_PREFIX` 加入本地搜索前缀；成功发现 package 后只使用其导出的
`OBS::libobs`，不混入仓库 `_obscfg`/`_simde`。package 自身的依赖也必须可发现。

FFmpeg 未指定 `FFMPEG_SDK_PREFIX` 时，先使用 pkg-config 的 imported targets，
再查找 `CMAKE_PREFIX_PATH` 中 SDK 的 `include/` 和 `lib/`（或 `lib64/`）。
显式设置 `FFMPEG_SDK_PREFIX` 会跳过 pkg-config，避免误选 Homebrew/系统版本。
该手动 SDK 路径面向共享库/导入库；需要额外私有依赖的静态 FFmpeg SDK 不在本次验证范围。
在 Linux 可通过 `PKG_CONFIG_PATH` 指向匹配的 `.pc` 文件。

Windows PowerShell 示例：

```powershell
cmake -S . -B build-local -A x64 -DSEI_TIMESTAMP_BOOTSTRAP_OBS=OFF `
  -DOBS_SDK_PREFIX=C:/SDK/obs -DFFMPEG_SDK_PREFIX=C:/SDK/ffmpeg
cmake --build build-local --config RelWithDebInfo
```

## 手动 fallback：没有导出 package 的 SDK

`OBS_INCLUDE_DIR` 指向含 `obs-module.h` 的目录；`OBS_LIB_DIR` 提供库搜索目录。
`find_library(NAMES obs libobs)` 由 CMake 选择平台后缀，不拼接 `.dylib`。
也可将 `OBS_LIBRARY` 设为实际库/framework 路径。
必须使用匹配的源码头文件、生成的 `obsconfig.h` 和库；源码树本身不是完整 SDK。
必要时提供 `OBS_CONFIG_DIR` 和 `SIMDE_INCLUDE_DIR`。

macOS framework 示例（仓库 helper 仅适用于已经准备这些目录的开发环境）：

```sh
cmake -S . -B build-local -DSEI_TIMESTAMP_BOOTSTRAP_OBS=OFF \
  -DOBS_INCLUDE_DIR="$PWD/obs-studio-master/libobs" \
  -DOBS_LIB_DIR=/Applications/OBS.app/Contents/Frameworks \
  -DOBS_CONFIG_DIR="$PWD/_obscfg" -DSIMDE_INCLUDE_DIR="$PWD/_simde" \
  -DFFMPEG_SDK_PREFIX=/path/to/matching/ffmpeg-sdk
```

手动搜索可发现 `/Applications/OBS.app/Contents/Frameworks/libobs.framework`。
自动发现 framework 不证明其与所给头文件版本一致。Homebrew FFmpeg 也不自动等同
于 OBS 内置 FFmpeg，必须检查真实链接与目标机器可用的依赖。

Linux 示例（开发 SDK 应提供可链接的 `.so`，仅有版本化运行库可能不足）：

```sh
cmake -S . -B build-local -DSEI_TIMESTAMP_BOOTSTRAP_OBS=OFF \
  -DOBS_INCLUDE_DIR=/path/to/sdk/include/obs \
  -DOBS_LIB_DIR=/path/to/sdk/lib \
  -DFFMPEG_SDK_PREFIX=/path/to/matching/ffmpeg-sdk
```

Windows 手动模式必须同时找到导入 `.lib` 和运行 `.dll`：

```powershell
cmake -S . -B build-local -A x64 -DSEI_TIMESTAMP_BOOTSTRAP_OBS=OFF `
  -DOBS_INCLUDE_DIR=C:/SDK/obs/include/obs -DOBS_LIB_DIR=C:/SDK/obs/lib `
  -DOBS_RUNTIME_DIR=C:/SDK/obs/bin/64bit -DFFMPEG_SDK_PREFIX=C:/SDK/ffmpeg
```

`OBS_RUNTIME_LIBRARY` 也可直接指定 `obs.dll`/`libobs.dll`；target 使用
`IMPORTED_IMPLIB` 和 `IMPORTED_LOCATION` 分别表示导入库与 DLL。
FFmpeg SDK 同样需要 avcodec/avutil 导入库；运行 DLL 必须与目标 OBS 配套。

## 产物与验证

| 平台 | CI 模板产物 | 本地 OFF |
| --- | --- | --- |
| macOS | 模板 plugin bundle 及现有归档/安装包 | MODULE 放在 `build-local/plugin`，不自动生成模板 bundle |
| Linux | 插件 `.so`、现有 DEB/归档 | 插件 `.so`，链接目标发行版的依赖 |
| Windows | 插件 `.dll`、现有归档/安装包 | DLL 位于所用生成器的构建/配置目录 |

本次不调整本地 install/package 布局；OFF 产物不等同于完整发布包。
安装到测试 OBS 后，验证模块和 locale 加载、实际编码、SEI 和停止尾帧。
检查动态依赖可用 `otool -L`（macOS）、`ldd`/`readelf -d`（Linux）、
`dumpbin /DEPENDENTS`（Windows）。禁止把本机编译成功当作三平台 ABI 验证。

CMake 模板默认会更新 `cmake/.CMakeBuildNumber`。需要保持源码不变的验证应传入
`-DPLUGIN_BUILD_NUMBER=<当前构建号>`，并将构建目录放在源码外。

依赖发现回归检查：`python3 -B tests/test_dependency_discovery.py`。
该测试使用临时 SDK fixtures 验证 CMake 路由与 target 属性，不替代 Linux/Windows 真机编译。

## P0/P1 编码与 SEI 契约

当前输入范围是 **8-bit CPU NV12**。OBS 负责将其视频输入转换为 NV12；插件不直接
接收 BGR、P010/Main10/HDR 或 DMA-BUF/CUDA/D3D11/QSV 原生硬件帧。

| FFmpeg 后端 | 插件提交路径 | 发布前要求 |
| --- | --- | --- |
| `h264_nvenc` / `hevc_nvenc` | CPU NV12；由 FFmpeg 后端处理上传 | Linux/Windows 对应 FFmpeg、驱动、NVIDIA GPU 真机验证；macOS 不承诺 NVENC |
| `h264_amf` / `hevc_amf` | CPU NV12，不设置 VAAPI `hw_frames_ctx` | Windows AMF runtime/驱动及 AMD GPU 真机验证；不承诺 macOS 或 Linux AMF |
| `h264_qsv` / `hevc_qsv` | CPU NV12，不设置 VAAPI `hw_frames_ctx` | Linux/Windows QSV backend、Intel 驱动/设备真机验证；不承诺 macOS QSV |
| `h264_vaapi` / `hevc_vaapi` | CPU NV12 → VAAPI surface upload | Linux VAAPI backend、render node 权限、驱动及 AMD/Intel 设备真机验证 |

候选列表只证明 FFmpeg 注册了编码器，不证明硬件存在或参数可用。创建时检查所选
H.264/HEVC 与实际 codec ID 一致，日志显示实际 encoder 名称和上传路径；只有准确匹配
`h264_vaapi` / `hevc_vaapi` 才配置 VAAPI frames context。profile、B 帧和码率参数
仍受各后端/设备能力限制，打开失败应保留 FFmpeg 和插件日志。
保留既有 VAAPI 策略：配置 B>0 时警告并强制改为 B=0，尚未按设备探测 B 帧能力。

### PTS、延迟与停止

每次提交按 PTS 保存墙钟时间和当时的 NTP 同步标记；输出按 PTS 查找，允许 B 帧
重排。最多保存 4096 个未完成提交，不再覆盖旧记录。缺失 PTS、`AV_NOPTS_VALUE`、
重复提交或容量耗尽会记录明确错误并返回编码失败，OBS 停止该输出；不会伪造当前时间。
这是完整性优先的行为，不是无声丢帧策略。

`send_frame` 遇 EAGAIN 时先完整 drain，再以同一仍有效的输入帧重试；每次成功提交后
也 receive 到 EAGAIN。全部输出先进入有界队列（64 包、合计 64 MiB），每次 OBS encode
回调最多交付一个包；空队列是正常延迟，队列超限/FFmpeg 错误则可诊断失败。
仍要求**一个 packet 对应一个完整 AU/一项输入 PTS**。一个 AU 含多个 slice 可以；
跨 packet 拆分 AU、一个 packet 合并多幅图像没有完整重组支持，不能宣布兼容。

当前本地 OBS `obs_encoder_info.encode` 只返回一个 packet，destroy 没有交付尾包的入口。
本轮没有发明 NULL-frame OBS 回调或在 destroy 中发送包。停止会报告尚未交付的提交和
队列包数量，然后释放资源。B 帧/lookahead/硬件延迟可能留下尾帧；即便 B=0 也不能
保证没有延迟。完整停止 flush 需要后续 OBS 生命周期/输出层设计。独立 libx264 测试
单独调用 FFmpeg flush，仅用于证明这一差异，不代表插件已经实现停止尾帧交付。

### Packet 格式与下游

在注入 SEI **之前**逐包检查 NAL framing。合法 Annex-B 保持原字节；avcC/hvcC 明确
声明长度字段时，验证整包长度及 NAL 头，将所有 NAL（包括参数集和多个 slice）转换为
四字节起始码，再计算插入位置。未声明的长度前缀、截断、零长度、歧义和纯头部包
拒绝输出并记录错误，不混合 Annex-B SEI 与长度前缀 packet。检测只验证 framing、
NAL 头和存在 VCL，不是完整码流语义/AU 边界解析；运行中 extradata/framing 变更未经验证。

保留 H.264 type 6 / HEVC prefix SEI type 39、UUID、RBSP 防竞争字节及关键帧处理。
输入/输出 PTS 与 DTS 不改写；`frame_seq` 是交付/编码顺序，B 帧时不能要求
`media_pts` 或捕获墙钟在该顺序单调。应按 PTS 匹配验证 SEI，不以 DTS 替代捕获时间。

原生 OBS SRT/RIST（当前本地源码的 `ffmpeg_mpegts_muxer`）只承诺 H.264。
RTMP/FLV、MPEG-TS/SRT 是否保留 SEI 必须对**接收端**码流检查；能启动编码不等于端到端
正确。HEVC 的 RTMP/FLV 支持依赖 OBS 版本与接收端协议能力，本轮未实测。
OBS 原始视频重新编码路径不使用本插件的编码包；下游重新编码也不保证继承 SEI。

`tools/sei_gateway.py` 当前仅解析普通逐行 H.264：同一 AU 的非首 slice 不再提前切断，
后续 AUD/参数集/前缀 SEI 或下一首 slice 才构成边界；任意网络 read 分片不构成 AU。
隔行/FMO/ASO/MVC 等完整 picture-boundary 语义未实现，分区/扩展 slice 拒绝处理；
超过 AU 上限时明确失败，不凭缓冲区大小制造边界。HEVC 不在该网关范围内。

### NTP

内部时间仍是 UTC Unix epoch 微秒，payload 二进制布局、PTS 映射及输入帧提交时采样语义不变。
每个编码会话创建一次 UTC 校准锚点（初值取系统墙钟），之后使用 monotonic 经过时间推进；
关闭 NTP 也使用这条路径。Linux 优先用计入休眠的 `CLOCK_BOOTTIME`，避免休眠冻结可信期。
运行中墙钟前跳/回拨不会直接改变每帧时间戳。重建编码器会建立
新锚点，不承诺不同会话之间无缝延续。缺失/倒退的 monotonic 读数冻结上次值并降为未知，
不会退回可跳变墙钟；初始化无法获得 monotonic 时明确创建失败，这不同于 NTP 失校。

SNTP 的 `t4` 在 `recvfrom` 返回后立即采集，并附带 monotonic 接收锚点。拒绝 RTT >250 ms
的样本，以及往返墙钟增量与 monotonic 增量相差 >50 ms 的样本；网络过程不持编码线程
需要的状态锁。响应秒字段按距本地参考最近的 NTP era 展开，要求参考与 UTC 相差小于
68 年；零时间戳拒绝。尚未实现认证、可取消 DNS 或独立闰秒/leap-smear 策略。

校准目标与当前连续时间相差最多 4 秒时才接受；修正按 monotonic 经过时间的最多 ±2%
（20 ms/s）渐进 slew，负修正只减速、不倒退，发布单次样本不会立即改变时间戳。超过
4 秒的异常样本拒绝并写诊断日志，不改变修正目标或延长可信期。大偏差持续存在时也不
突然跳时；应先校准系统/核查时间源，并在允许的会话边界重新建立锚点。

`clock_ntp` 表示最近 180 秒内有被接受的校准样本，不再表示“曾经同步过”。失败时继续
使用连续时间，过期降为 false/未知并告警，不阻断编码；恢复后仍通过 slew 校准。
NTP 配置间隔大于 180 秒时，两次校准之间会有未知区间。诊断写入 stderr，未知状态沿
原有 SEI 标志传递；无新增 payload 字段，不改 Gateway、前端或测试时钟页面。

**5 秒误差预算的适用条件**：接受门限 4 秒，为网络与采样误差、振荡器漂移保留余量。
在服务端时间正确、参考 era 正确、墙钟采样误差不超过上述 50 ms、单调时钟漂移不超过
1000 ppm 的假设下，180 秒内保守预算为 4 s + RTT/2（最多 0.125 s）+ 0.05 s +
0.18 s = 4.355 s。可信位是客户端的新鲜度/门限判断，不是认证的精度证明；服务器自身
误差、启动时错误时钟、长时间离线以及采集/播放链路的额外误差无法由插件单独约束。
失校仍持续输出，但不能声称未知状态或完整端到端链路无条件满足总误差 ≤5 秒。

离线测试注入墙钟和 monotonic 输入，覆盖成功/失败、正负 slew、孤立异常、墙钟前跳/
回拨、180 秒过期与恢复、era 回绕及网络回调不持锁。不修改真实系统时间，也不冒充公网
NTP/设备漂移实测。销毁仍需等待进行中的 DNS/网络调用。

## 可复现验证矩阵

准备目标平台 SDK 后，使用上面的 OFF 配置命令，再运行：

```sh
cmake --build build-local --config RelWithDebInfo --parallel
ctest --test-dir build-local -C RelWithDebInfo --output-on-failure
python3 -B tests/test_dependency_discovery.py
python3 -B tests/test_gateway.py
git diff --check
```

CTest 包含核心 SEI、PTS/队列/packet conversion、真实 libx264 NV12+B 帧+双 slice、
POSIX 时钟逻辑、依赖发现和网关分片测试。libx264 不存在时真实编码测试明确标为
Skipped（退出 77），不能记为硬件或软件编码通过；时钟离线测试当前仅注册于 UNIX。
Windows 运行这些 FFmpeg 测试前，必须将配套 SDK 的运行 DLL（及其依赖）目录加入
测试进程 PATH；仅有导入 `.lib` 不足以运行测试。核心测试仍可以不用 OBS/FFmpeg 独立编译：

```sh
cc -std=c11 -Isrc tests/test_sei.c src/sei-payload.c src/h26x-util.c -o /tmp/sei-core-test
/tmp/sei-core-test
```

Linux VAAPI 真机首先记录环境（以下命令不代表已经执行）：

```sh
ffmpeg -version
ffmpeg -encoders
vainfo --display drm --device /dev/dri/renderD128
ls -l /dev/dri/renderD*
ffmpeg -vaapi_device /dev/dri/renderD128 -f lavfi -i testsrc2=size=1280x720:rate=30 \
  -vf 'format=nv12,hwupload' -c:v h264_vaapi -bf 0 -t 10 -f h264 /tmp/vaapi-smoke.h264
```

该 smoke 命令只验证 FFmpeg/驱动上传，不经过插件，不会生成本项目 UUID SEI。
随后在测试 OBS 选 **SEI Timestamp** 与实际 FFmpeg codec，矩阵至少覆盖：

- 各后端 H.264/HEVC、B=0/2（VAAPI 记录强制降为 0；其他后端记录实际支持或拒绝）、驱动支持的双 slice、
  初始延迟、关键帧、持续编码及停止重启；记录真实 profile、像素格式和 encoder 名。
- 本地录制、H.264 RTMP/FLV、原生 H.264 SRT/RIST 接收端；HEVC 单列为条件验证。
  对源/接收端按 PTS 比较 UUID、时间、帧序号、关键帧标记和缺失率，不只检查日志。
- 用 `ffmpeg -i recording.mkv -map 0:v:0 -c:v copy -bsf:v h264_mp4toannexb -f h264 /tmp/result.h264`
  导出后运行 `python3 tools/verify_sei.py /tmp/result.h264`；HEVC 对应使用
  `hevc_mp4toannexb`、`-f hevc` 和验证器 `--codec hevc --raw`。导出只做 copy。
- NTP 关闭、可用与不可达、采样期间编码；比较编码延迟并确认不会等候 DNS/recv 的状态锁。
  停止延迟、未交付数量单独记录，不能把 flush 缺口计为传输丢包。

本轮本地验证环境是 macOS/AppleClang 与本机 OBS framework、Homebrew FFmpeg，
仅验证编译和离线/软件编码行为。Linux/Windows CI、真实 NVENC/AMF/QSV/VAAPI、
OBS 实际加载和端到端推流均须另行执行，不在本轮通过声明内。
