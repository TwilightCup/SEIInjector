# SEIInjector — OBS 插件：向推流中注入帧级实时时间戳

一个 [OBS Studio](https://obsproject.com) 插件：把**每一帧编码视频**对应的
**现实时间（NTP 校准后的 Unix 微秒）**，通过 H.264 / H.265 的 **SEI** 消息写进码流。
拉流端（播放器 / 服务器 / 多画面合成工具等）读取这些时间戳后，就能对
**多个独立主播的推流**做逐帧时间轴对齐。

本项目参考了父目录
[SEI-Stamper](../SEI-Stamper) 的整体思路与架构（SEI-Stamper 只在关键帧打点、
并侧重 OBS 内的接收端）。SEIInjector 更小、只做发送端：**每一帧都打点**、
协议格式固定、并附带可独立使用的校验 / 对齐工具，供你自己实现拉流端。

> 状态：源码面向 OBS ≥ 31 完成（已用 OBS 31.0.1 + FFmpeg 6.1 头文件做过
> 语法校验；H.264 路径与 SEI-Stamper 在 OBS 32.1.x 上验证过的布局一致）。
> 正式发布前请针对你要分发的 OBS 版本编译测试，见 [编译](#编译)。

---

## 为什么用 SEI？

OBS 不向插件开放内置编码器的压缩码流。要往推流里逐帧注入数据，可靠的
做法是**由插件自己充当编码器**：SEIInjector 注册两个自定义视频编码器，
内部包装 OBS 自带的 FFmpeg 编码器（libx264 / NVENC / AMF / QuickSync /
VideoToolbox / MediaFoundation），改写每一帧编码输出后交回 OBS 推送/录制。
SEI 内嵌在 H.264/H.265 基本流中，可原样穿过 RTMP/FLV、SRT/MPEG-TS、
WebRTC 网关和录制文件。

## 特性

* **逐帧打点** — 每个编码帧（I/P/B）携带一条 SEI NAL（约 60 字节），包含
  `realtime_us`、该帧 `media_pts`、流内 `frame_seq` 与标志位。60fps 下开销
  约 30 kbit/s，可忽略。
* **绝对时间基准** — 内置后台 SNTP 客户端（默认 `pool.ntp.org`），使发送端
  时钟对齐 UTC 历元；各机器打出的时间戳代表同一时刻。NTP 不可用时自动回退
  本地墙钟。
* **两种编码** — `SEI Timestamp (H.264)`（推荐，兼容性最好）与
  `SEI Timestamp (H.265/HEVC)`。
* **任意底层编码器** — 编码器下拉框动态探测当前 OBS 的 FFmpeg，只列出可用项。
* **容器安全** — 向 OBS 提供序列头（avcC/hvcC）供 RTMP/FLV/mp4 使用；关键帧
  上再内联注入 SPS/PPS/(VPS)，保证中途加入的 MPEG-TS/SRT 接收端可解码。
* **无额外运行依赖** — 只需插件本体 + locale 文件；FFmpeg、socket、线程均
  来自 OBS / 操作系统。

## 安装

1. 克隆本项目并编译（见下）。
2. 把产物放入 OBS 插件目录：
   * Windows：`obs-plugins\64bit\sei-timestamp.dll`
   * macOS：`obs-studio.app/Contents/PlugIns/sei-timestamp.so`
   * Linux：`/usr/lib/obs-plugins/sei-timestamp.so`（随发行版而异）
3. 把 `data/locale/*` 复制到 OBS 数据目录下的
   `data/obs-plugins/sei-timestamp/locale/`。
4. 重启 OBS。

无需附带额外 DLL：FFmpeg 符号由 OBS 已加载的 FFmpeg 提供。

## 使用（发送端）

1. **设置 → 输出 → 输出模式：高级**。
2. 把 **串流编码器** 选为 `SEI Timestamp (H.264)`（或 H.265）。
3. 编码器属性里：
   * **Encoder**：软件（libx264）或你的硬件编码器（NVENC/AMF 等）。若列表内
     某项无法启动，OBS 日志会给出原因——通常是驱动不接受该 preset/profile，
     可换 `VBR` 或别的档位。
   * **Timestamp Settings**（分组）：跨机对齐请保持 *NTP time correction*
     开启，并选一个可达的 **NTP Server**。
4. 正常推流（RTMP/SRT 均可）。此后每个视频帧都带有真实采集时间。

> 需要对齐的所有人应使用同一 NTP 基准（或足够同步的系统时钟）。NTP 校准在
> 后台线程进行，不会阻塞“开始串流”。

## 拉流端

从收到的码流中读出 SEI（参考 `tools/` 的实现）：

1. **解复用**成 Annex-B（任何 ffmpeg 均可，SRT/RTSP/FLV 拉流中的带内 SEI 也会保留）。
2. **找到 UUID** 为 `7e57c2ee-0dd2-4b53-9b35-93edf97a12c1` 的 SEI NAL。
3. **解码**载荷（格式见下）得到逐帧 `realtime_us`。
4. **对齐**：把每条流的帧按 `realtime_us` 排序；在时刻 `T` 播放各流中
   最接近 `T` 的帧。各发送端共享 NTP 历元时，同一 `T` 对应同一真实采集时刻，
   多流画面即逐帧锁定。建议保留 200–500 ms 平滑缓冲吸收网络抖动。

### SEI 载荷格式

载荷类型 `5`（user data unregistered），载荷以 UUID 开头：

```
偏移  长度  字段            含义
----  ----  ---------------  --------------------------------------------
0     16    UUID             7e57c2ee-0dd2-4b53-9b35-93edf97a12c1
16    1     version          = 1
17    1     flags            bit0 关键帧; bit1 时间戳经 NTP 校准
18    4     frame_seq        编码开始以来的帧计数（大端）
22    8     media_pts        本帧 PTS（大端，OBS 时基单位）
30    8     realtime_us      采集墙钟时间，Unix 历元起微秒（大端）
```

多字节字段均为大端。`flags & 0x02` 置位时 `realtime_us` 为 NTP 校准时间，
否则为发送端本地墙钟。

### 工具

* `tools/verify_sei.py <capture.mp4|ts|flv|mkv|h264>` — 用 ffmpeg 解复用后
  逐帧打印时间戳报告（帧间隔、fps、NTP 覆盖率），录制后质检很方便。
* `tools/align_streams.py <a.mp4> <b.mp4> ...` — 测量两两间的中位偏移、
  抖动（MAD）、时钟漂移（ppm）与所需平滑缓冲，并输出拉流端对齐方法说明。
  录制时间完全不同的流会被正确判定为“无时间重叠”。

### 端到端架构（比赛多路→对齐→导播→对外直播）

面向“前端导播台”实现方的完整交接规格见
**[docs/ARCHITECTURE.zh-CN.md](docs/ARCHITECTURE.zh-CN.md)**
（英文索引：[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)）：包含角色职责、
MediaMTX 规划、前端对齐算法与抖动预算、SEI Gateway 桥 ↔ Web 前端的
WebSocket 协议 v1（AU+SEI 帧格式、cfg/tick）、备选服务端引擎方案，以及
可直接作为任务书的验收清单。

## 编译

需要 OBS 源码树（头文件 + 编译好的 libobs）。可把 obs-studio 克隆到本仓库
顶层 `obs-studio-master/`（与 SEI-Stamper 相同做法），或用 CMake 变量指向
已有目录。

依赖：CMake ≥ 3.20、C11 编译器；OBS Studio 源码（≥ 31）；FFmpeg
头文件与库（avcodec、avutil）——来自 OBS 的 `obs-deps-*`、`pkg-config`，
或 macOS 上 Homebrew。

### Windows（参考 SEI-Stamper）

```bat
git clone https://github.com/obsproject/obs-studio.git obs-studio-master
:: ... 先构建 OBS Studio 本身
cmake -S . -B build -DOBS_DIR=%CD%\obs-studio-master\build\rundir\Release ^
                     -DOBS_DEPS_DIR=%CD%\obs-studio-master\.deps\obs-deps-2025-08-23-x64
cmake --build build --config Release
cmake --install build
```

### macOS

```sh
# 先构建 OBS，或使用 OBS 的开发安装前缀，然后：
cmake -S . -B build -DOBS_SOURCE_DIR=/path/obs-studio/libobs \
                     -DOBS_LIB_DIR=/path/obs-studio/build/libobs \
                     -Dlibobs_DIR=$(find /path/obs-studio/build -name 'libobsConfig.cmake' -exec dirname {} \;) \
                     -DFFMPEG_INCLUDE_DIR=/opt/homebrew/include \
                     -DFFMPEG_LIBRARY_DIR=/opt/homebrew/lib
cmake --build build
```

生成的 `sei-timestamp.so` 需链接 OBS 自带的 FFmpeg dylib（`obs-studio.app`
内、`@rpath` 形式）或随 OBS 一起分发的版本。

### 单元测试（不需要 OBS / FFmpeg）

```sh
cc -std=c11 -Wall -Wextra -Isrc -o sei_core_tests tests/test_sei.c \
   src/sei-payload.c src/h26x-util.c
./sei_core_tests
# 或配置后： ctest --test-dir build
```

测试覆盖 H.264/H.265 SEI NAL 构造（含防竞争字节转义）、解析回读、
avcC/hvcC→Annex-B 转换、前缀 NAL 扫描。SNTP 路径可直接冒烟验证：
调用 `realtime_clock_create("pool.ntp.org", …)` 观察 offset 收敛。

---

## 设计说明 / 限制

* **必须把本插件选为“串流编码器”**。它也是普通 OBS 编码器，可用于录制与
  OBS 原生 SRT 输出（自定义 → `srt://…`）。
* **采集时间近似**：时间戳在 OBS 把帧交给编码器时记录（比真实采集晚约一帧，
  每台机器近似恒定），再按 PTS 归属到对应编码包。残余偏差是每条流的常数，
  不影响多流对齐。
* **对齐精度取决于时钟**：开启 NTP 时残余误差通常几毫秒到几十毫秒
  （30fps 小于一帧）。关闭 NTP 时取决于各机系统时钟的同步程度
  （Windows Time 等服务通常 < 100 ms）。
* **兼容性**：RTMP 服务器与播放器容忍带内 SEI 与重复参数集；MPEG-TS/SRT
  需要关键帧内联头（已自动注入）。遇到挑剔的收流端请查看 OBS 日志，插件会
  打印底层 FFmpeg 编码器、preset 与 `avcodec_open2` 失败原因。
* **H.265 走传统 RTMP 并非所有服务器都支持**；追求最大可达性请用 H.264，
  H.265 适合 SRT/TS 以及支持 FLV-HEVC 的新式收流端。
* 只给视频打点：音频无需时间戳——单流内音画同步由 OBS 保证，SEI 只用于
  跨流对齐。

## 目录结构

```
src/
  sei-timestamp-plugin.c     模块入口、编码器注册
  sei-timestamp-encoder.{h,c} 自定义 H.264/H.265 编码器（FFmpeg 包装 + SEI 注入）
  sei-payload.{h,c}          SEI 线上格式：构造与解析（纯 C）
  h26x-util.{h,c}            Annex-B 扫描、avcC/hvcC → Annex-B（纯 C）
  realtime-clock.{h,c}       历元时钟 + 后台 SNTP（纯 C）
tests/test_sei.c             纯逻辑单元测试
tools/verify_sei.py          抓录文件时间戳报告
tools/align_streams.py       多流对齐测量
data/locale/*.ini            OBS 本地化数据
```

## 许可证

GPL-2.0-or-later，与 OBS Studio 一致，见 [LICENSE](LICENSE)。
部分架构参考 SEI-Stamper（GPL-2.0）。
