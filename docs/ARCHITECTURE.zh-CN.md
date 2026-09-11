# 比赛多玩家直播 → 帧级对齐 → 导播视角对外直播（架构交接文档）

> 本文件是给 **web 前端/导播台开发（另一 Agent）** 与后端协作的交接规格。
> 时间戳来源插件见仓库根 `src/` 与 `tools/`（SEIInjector）。本文档 v1，2026。

---

## 1. 总览与链路

```
 选手 A / B / C            (每台选手机: OBS + SEIInjector 插件)
   │  视频帧逐帧打 SEI: realtime_us(同一 NTP 历元) + media_pts + seq
   │  内部链路协议: SRT  编码: H.265/HEVC 或 H.264（内部建议 HEVC）
   ▼
 ┌────────────────────────────────────────────────────────────┐
 │ MediaMTX (中间件/中继)                                      │
 │   path/playerA  path/playerB  path/playerC   ← SRT 发布     │
 │   path/director(可选回灌: 浏览器/引擎 WHIP/RTSP 发布)        │
 └────────────────────────────────────────────────────────────┘
   │  A: 网页导播台自行拉流并合成(走本方案“SEI Gateway”桥)
   │  B: (替代/备选) 服务端对齐引擎输出 director path, 网页只监看控制
   ▼
 导播视角流(director)
   │  对外一律 H.264(兼容性), 内部 HEVC 不对外
   ▼
 最后一级: ffmpeg(或平台 SRT ingest) → YouTube / Twitch / B 站 …
```

**本交接文档的立场**：你们已确定“拉流与画面合成在 web 前端进行”，
因此**推荐链路 = A**：增加一个很薄的 **SEI Gateway 桥服务**（挂在 MediaMTX
旁边，读各 player path，解出“每一帧 + 该帧 SEI”，用 WebSocket 推给前端），
前端用 **WebCodecs** 解码、按 SEI 的 `realtime_us` 逐帧排期呈现与合成。
B（服务端对齐引擎）作为备选列于 §8，供取舍。

---

## 2. 角色与职责边界

| 角色 | 负责方 | 协议/接口 | 责任 |
|---|---|---|---|
| 选手端 OBS + 插件 | 已交付(本仓库) | SRT → MediaMTX | 每帧打 SEI，时钟 NTP 校准 |
| MediaMTX | 已部署 | 见 §4 | 多路汇聚/分发，低延迟读取 |
| **SEI Gateway 桥** | 后端(可另行实现) | SRT/RTSP←MTX; WS→前端 | 帧级透传 + SEI 解析（协议 §6） |
| **Web 导播台** | **本交接对象** | WS + WebCodecs | 解码、帧锁、多画面/切换合成、输出 |
| 对外推送 | 后端/运维 | ffmpeg→RTMP 或 SRT | director → 各平台 H.264 |

> 需要前端 Agent 明确实现的交付物（建议验收标准见 §9）：
> 1. WS 客户端按 §6 协议订阅多路流并解析；
> 2. WebCodecs 解码 + 按 realtime 排帧的帧锁播放器；
> 3. 多画面网格 / 单画面切换（导播 UI）；
> 4. 将合成后的 director 画面上行（WebRTC WHIP 回 MediaMTX 或交给后端引擎）。

---

## 3. SEI 时间戳语义（前端必须理解）

插件把 SEI 打在每个**编码帧(Access Unit)**里。每帧载荷（在 H.264 SEI
type5 / H.265 prefix-SEI 中）：

```
UUID: 7e57c2ee-0dd2-4b53-9b35-93edf97a12c1 (16B)
version=1 | flags | frame_seq(4B BE) | media_pts(8B BE) | realtime_us(8B BE)
flags: bit0=关键帧  bit1=realtime_us 经 NTP 校准
realtime_us = 该帧真实采集时刻，Unix 历元微秒（各机同基准才可比）
```

- **帧锁语义**：同一 `realtime_us` 在各条流里 = 同一真实时刻各自拍到的画面。
  把流 S 的帧按其 `realtime_us` 排序，目标时刻 T 取“最接近 T 的那一帧”。
- **常数偏置**：时间戳在 OBS 把帧交给编码器时记录（比采集晚约 1 帧，每台
  机器近似恒定）。若跨流对比发现有 ~几十 ms 的恒定差，用
  `tools/align_streams.py` 测出的中位偏移在帧锁层做一次每流常数修正即可。
- **时钟要求**：选手机 OBS 开启 NTP（默认 `pool.ntp.org`）；比赛内网可自建
  NTP。未开 NTP 的帧 `flags bit1=0`，视为仅本地时钟，精度打折。
- 编解码与解析实现参考：仓库 `src/sei-payload.c`（C，纯逻辑）与
  `tools/verify_sei.py`（Python，可直接移植/对拍）。

---

## 4. MediaMTX 配置要点（供核对/补全，不是前端职责）

原则：**每选手一个独立 path；延迟优先，关闭非必需协议**。

```yaml
# mediamtx.yml（片段）
paths:
  player_%s:            # playerA / playerB ...
    source: publisher   # SRT 发布者自报
    srt: yes
  director:
    source: publisher   # 前端/引擎回灌的导播画面
webrtc:
  enabled: yes          # 读取端低延迟(WHEP)；发布端(WHIP)前端上行用
srt:
  enabled: yes
hls:
  enabled: no           # 比赛对齐链路不需要高延迟 HLS
rtmp:
  enabled: yes          # 便于 ffmpeg/其他工具读取或发布
```

- 选手 OBS：服务 = `srt://<MTX-IP>:8890?mode=caller`（或 `listener`），
  流键 = `player_张三` 之类唯一 ID。
- MediaMTX 读取端：SRT、RTSP、RTMP、WebRTC(WHEP)、MoQ 均可读同一 path。
- **SEI Gateway 桥**从 MediaMTX 读的推荐方式：RTSP（`rtsp://…/player_x`，
  ffmpeg/自研均可稳定订阅）或 SRT 读取；桥只做解复用+SEI 提取，不重编码。

---

## 5. 前端时间轴与对齐算法（核心）

### 5.1 浏览器没有“可信 NTP 时刻”，用 Gateway tick 建立本地映射

`performance.now()` 单调且稳（不受系统时间调整影响），但非历元。桥每秒发一条
`tick`（§6），前端维护：

```
epochEstimate(t) = tick.epoch_us + (performance.now() - tick.perf_now_us)
                    + 单向网络偏移修正(可用 tick RTT/2 粗修)
```

同一连接内所有流的 `realtime_us` 都以此估值为“现在”。

### 5.2 播放节拍

```
目标呈现时刻 T = 当前估值时刻 + D
D  = 固定缓冲(建议 300–500 ms; 需 ≥ 最慢一条流的抖动上界)
```

逐流维护一个**已解码帧队列**（按 `realtime_us` 升序）：
- `realtime_us > T` → 属于未来帧，等待；
- `|realtime_us - T|` 最小 → **呈现**；
- `realtime_us < T - D_slack` → 判定迟到，**丢帧**并让解码器保持 GOP 内可继续
  （跨关键帧不可丢——若迟到的是关键帧之前，宁可直接跳到下个关键帧重同步）。

### 5.3 多流帧锁呈现

- 每条流一个解码器实例（WebCodecs `VideoDecoder`）+ 一个渲染图层/Canvas。
- 每 rAF（或解码器回调）取各流“最接近 T 的帧”同帧绘制 → 多画面天然同拍。
- 切换/导播画面 = 对同一组已对齐帧的**选择**，切换瞬间无跨流错位。

### 5.4 误差预算（验收目标）

| 环节 | 预期 |
|---|---|
| NTP 校准残差 | 几 ms ~ 几十 ms |
| 编码/上行/桥/WS 传递 | 常数（各流一致，靠 T+缓冲吸收）|
| 播放器排帧误差 | < 1 帧（30fps ≈ 33 ms）|
| 对外推流端到端 | 与常规直播一致（几百 ms ~ 1 s 视平台）|

调试工具：选手端抓录后用 `tools/verify_sei.py` 验证逐帧连续；多路同时录播用
`tools/align_streams.py` 测中位偏移/抖动/漂移，若数值超预算先查 NTP 与网络。

---

## 6. SEI Gateway ↔ Web 前端 协议 v1（前后端并行开发的契约）

> 桥未实现前，前端可用 mock 服务器开发。**桥只透传帧+SEI，不负责帧锁**。

### 6.1 连接与订阅

```
ws://gateway-host:8765/seiv1
发送(文本): {"type":"subscribe","streams":["playerA","playerB"],"codecs":["h265","h264"]}
桥应答(文本): {"type":"ok","streams":[{"id":"playerA","codec":"h265","w":1920,"h":1080,"fps":60,"hasSei":true},...]}
可随时: {"type":"subscribe",...} 追加 / {"type":"unsubscribe","streams":[...]}
```

### 6.2 二进制消息（WS binary，长度前缀分帧）

```
[0..3]  uint32 BE: header_len L
[4..L)  JSON header (UTF-8)
[L..)   payload 字节(整条消息的 Access Unit 或配置字节)
```

header 字段（全部小写）：

| 字段 | 类型 | 说明 |
|---|---|---|
| `t` | string | `au`(一帧) / `cfg`(该流参数集) / `tick`(时钟心跳,无payload) |
| `s` | string | 流 id（= MediaMTX path 名） |
| `c` | string | `h264` \| `h265` |
| `k` | bool | au: 是否关键帧（其 AU 内含 SPS/PPS(/VPS)） |
| `seq` | int | 插件 frame_seq（丢帧检测用） |
| `pts` | int | 该帧 media_pts（容器/流内时基；桥原样透传） |
| `rt` | int | **realtime_us（帧锁主键，勿改）** |
| `n` | bool | rt 是否 NTP 校准 |
| `sz` | int | payload 字节数 |

`tick`（约每 1000 ms 一条，无 payload）：
`{"t":"tick","epoch_us":..., "perf_now_us": 发送端 performance.now()*1000}`

### 6.3 payload 字节格式（约定，前端解码器按此喂 WebCodecs）

- **au**: 一个完整 Access Unit，**Annex-B**（00 00 01 / 00 00 00 01 起始码）。
  关键帧 AU 内已含 SPS/PPS(/VPS)（插件注入保证）。非关键帧含 SEI+切片。
  桥**不做** AVCC 转换；前端若解码器要求 AVCC(length-prefixed)，在 JS 里
  按起始码切分、前加 4 字节大端长度即可（附录 A 有 15 行参考）。
- **cfg**: 该流首个关键帧后由桥补发一次，payload = WebCodecs `description`
  用字节：
  - h264 → `avcC`(AVCDecoderConfigurationRecord，插件 extra_data 原样)；
  - h265 → `hvcC`(HEVCDecoderConfigurationRecord，若有)；
  - 若桥拿不到记录，则给 Annex-B 形式的 VPS/SPS/PPS 拼接，前端自行决定
    是否解析为 description，或直接依赖关键帧带内参数集（多数解码器可行）。
- 丢帧/乱序：桥保证单流内按 pts/rt 顺序发送；跨流互不相关。
- 断线：桥重连后重发 `cfg` + 下个关键帧，前端应丢弃关键帧之前的已排队帧。

### 6.4 前端最小解码伪码（契约的消费方）

```
for msg in ws:
  if t=='cfg':   config.description = payload            # avcC/hvcC
  if t=='au':
    chunk = EncodedVideoChunk({type: k?'key':'delta',
                timestamp: rt, data: payload})           # 或转 AVCC
    decoder[ s ].decode(chunk)
  on decoder output VideoFrame vf:
     queue[s].push({rt: vf.timestamp, frame: vf})        # timestamp=rt
render loop: T = nowEstimate() + D
  for s in streams: pick f in queue[s] with |f.rt - T| minimal; draw(f)
  drop queue entries older than T - 2*frameInterval
```

### 6.5 导播画面上行（前端若负责输出）

- 合成画布 `canvas.captureStream(fps)` + WebCodecs/`MediaRecorder` 编码
  **H.264**（对外兼容）→ **WHIP** 推 `MediaMTX path/director`
  （MediaMTX 支持浏览器 WebRTC 发布）。此后：
  `ffmpeg -re -i rtmp://MTX/director -c copy -f flv rtmp://平台/app/stream`
  （或平台支持 SRT 则更顺）。多画面合成里含多路小窗，注意各窗已是帧锁，
  合成输出无需再对齐。

---

## 7. 选手端部署与运维要点（交接给场务，非前端）

- 每位选手：OBS → 高级输出 → 串流编码器 = **SEI Timestamp (H.264)**
  （或 H.265）→ 服务 = SRT 到 MediaMTX 唯一 path；开 NTP。
- 直播开始前用 `tools/verify_sei.py 抓一个文件` 做冒烟；对齐联调用
  `tools/align_streams.py` 同时抓 A/B/C 各一小段，核对 median/MAD 在预算内。
- 掉线重进：选手重推后 seq 会重置、keyframe 后前端自动恢复对齐，无需干预。
- 若某一帧的 `n=false`（NTP 刚断）持续出现，提示该选手检查网络/NTP。

---

## 8. 备选：服务端对齐引擎（web 只做控制台/监看）——何时切这套

适用：web 前端只负责 UI 切换与监看、不希望浏览器承担解码合成/上行。

```
服务端对齐引擎(Go/Python):
  订阅 MediaMTX player_* (RTSP/SRT)
  → 逐帧解析 SEI → 帧锁(§5.2 算法同前端, 在服务端跑)
  → 合成/切换(按控制指令) → 编码 H.264 → 回灌 MediaMTX director
    或直接 ffmpeg 推平台
Web 前端: 拉 director(WHEP/MoQ) 监看 + WebSocket 发送切换指令
```

切换指令契约（无论引擎还是前端自持画面，UI 语义一致）：
```
{"cmd":"scene","type":"single","stream":"playerB"}
{"cmd":"scene","type":"grid","streams":["playerA","playerB","playerC"],"layout":"2x2"}
{"cmd":"scene","type":"pip","main":"playerA","inset":"playerB"}
```
若采用 A（前端合成），上述指令由前端 UI 直接消费；若采用 B，指令发给引擎。

**推荐决策**：导播画面若以“单画面切换 + 少量贴片”为主 → **A** 简单直接；
若存在长期多路网格推流、或未来要“多画面直接对外”→ 引擎 B 更稳（避免
浏览器编码质量/续航/上行链路的不确定）。本交接默认 A，但契约里预留 B 的
切换语义一致，切方案前端 UI 层改动小。

---

## 9. 前端验收清单（可直接作为另一个 Agent 的任务书）

- [ ] WS 订阅多路并收到 `cfg`/`au`/`tick`；单流顺序稳定、重连能恢复。
- [ ] 双路/三路同屏播放：同一时刻三路画面内容（游戏时刻）逐帧一致，
      用“画面中同一倒计时数字”人工确认 ≤1 帧偏差。
- [ ] 人为断掉选手 A 网络 5 s 再重推，A 恢复后 1 s 内重新帧锁，不影响 B/C。
- [ ] 关闭 NTP 的选手（测试机）被正确标记 `n=false` 且不阻塞他人。
- [ ] 切场景(单画面/网格)瞬时无跨流跳变；合成 H.264 上行在目标平台可播。
- [ ] 长跑 30 min 检查漂移：单流 `realtime_us` 间隔均值 ≈ 1/fps、无累积；
      跨流 median 漂移 ppm 与 `tools/align_streams.py` 量值一致。

---

## 10. 相关文件/参考

- 插件 SEI 构造与解析: `src/sei-payload.{h,c}`（协议权威实现）
- 帧报告/对齐测量: `tools/verify_sei.py`, `tools/align_streams.py`
- 编码器行为(内联参数集等): `src/sei-timestamp-encoder.c`
- MediaMTX 官方: https://mediamtx.org （发布/读取协议矩阵见其 README）
