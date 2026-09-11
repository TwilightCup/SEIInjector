# SEIInjector 跨地域帧同步 + 导播自动对齐 最小 Demo 实跑测试流程

> 目标：跑通 **两站 OBS+SEI 推流 → MediaMTX → SEI Gateway 拉流 → HTML 导播页
> 自动对齐** 的完整链路的**最小 demo**，验证"导播视角"里多路画面与计时器按
> `全局对齐时间戳(gts)` 帧锁。不是单元测试，是真实前端+真实推流的端到端联调。
>
> 版本：固定 0.1.0（OBS ≥31，构建依赖 OBS 31.1.1）。

---

## 0. 角色与拓扑

```
 玩家A OBS+SEI(RTMP)  ─┐
                        ▼
  玩家B OBS+SEI(RTMP)  ─►  MediaMTX (player_A / player_B)
                        │
                      RTSP 拉流
                        ▼
              SEI Gateway (Python 最小 demo)
                每帧: SEI realtime_us + 画面字节 → WS
                事件流: 各玩家计时器 cp 事件(realtime, cp_time) → WS
                        ▼
              HTML 导播页（最小 demo）← 自动对齐/帧锁/计时器修正在这里
```

- **玩家 A / B**：异地选手机，各一台 OBS + SEIInjector，推 `SEI Timestamp (H.264)`
  over **RTMP**（TCP:1935）；场景 = UTC 毫秒时钟画面（视觉对拍用）。
- **观测者 = 你**：站在第三个地点，跑 MediaMTX + 网关 + 打开导播页。

---

## 1. 依赖：最小 demo 的组成件（测试要能跑必须先有）

| 件 | 在仓库? | 作用 | 说明 |
|---|---|---|---|
| OBS 插件(0.1.0) | ✅ src/ | 每帧打 SEI | 发送侧 |
| test-clock.html | ✅ tools/ | UTC 毫秒时钟测试图案 | 视觉对拍 |
| verify/align 脚本 | ✅ tools/ | 离线检验 SEI 是否写对 | 交叉对拍 |
| **SEI Gateway** | ✅ tools/sei_gateway.py | RTSP→逐帧(SEI+裸AU)→WS | 需 `pip install websockets` |
| **HTML 导播页** | ✅ tools/director.html | WebCodecs 解码 + 缓冲 + gts 对齐 | 起 `http.server` 打开 |

> 协议沿用 `docs/ARCHITECTURE.zh-CN.md` §6 的 WS 契约（subscribe/au/cfg/tick），
> 最小 demo 每帧 = 二进制 `[u32 头长][JSON 头 {t,s,k,seq,rt}]` + 裸 Annex-B AU。
> 本测试**只验证两条视频流的帧对齐**，不含计时器。gts 语义见 §2。

---

## 2. 自动对齐逻辑（本测试要验证的算法，忠实还原你的描述）

### 2.1 每玩家帧队列（缓冲）
- 每个玩家一个队列，存已到帧的 `(realtime_us, 画面)`，容量上限默认 **1 分钟**
  （60s，可调；60fps≈3600 帧）。超出滚动丢最旧。

### 2.2 全局对齐时间戳 gts
```
start_rt[s]  = 流 s 队列里第一帧的 realtime_us
gts0         = max_s( start_rt[s] ) + 10s     // 慢流起点再 +10s
gts(t)       = gts0 + ( performance.now(t) - performance.now(t0) )  // 按本机单调时钟,
                                                                    // 1:1 前进,不受系统跳时
```
- 取 `max` 可以容忍两流先后起播（后起的那条 = "慢流"）。
- **已确认：+10s = 领先量** → `gts≈墙钟+10s`，live 流无未来帧，故
  `realtime ≤ gts` 在接近前沿时恒成立，画面基本=最新帧；这个 "+10s" 的真正作用
  是启动期留缓冲余量，让慢流不会一开始就吃不饱。demo 里已按此实现（可调 `lead`）。
  （若以后想要"落后 10s"的刻意延迟画面，改 demo 的 `gts0 = max(firstRt)+lead` 为减
  lead，并在取帧处让 future 帧等待，即可。）

### 2.3 画面取帧（每流，每帧显示）
```
每播放节拍(如 60Hz):
  for 每条流 s:
    pick = 队列里 realtime_us ≤ gts 的【最新】一帧   // 绝不用未来帧
    画 pick；若 pick 距 gts 超过 maxLatency(建议1s) 标记该流"滞后"
```

---

## 3. 执行步骤（最小 demo 全链路）

### 步骤 0｜起最小 demo（观测者）
```bash
# 0.1 依赖
pip install websockets                       # gateway 用（或 python3 -m pip install websockets）

# 0.2 起 MediaMTX，安全组开 TCP 1935 / 8554

# 0.3 起 SEI Gateway（读两路 RTSP，推 AU+SEI；WS :8765）
python3 tools/sei_gateway.py \
    --rtsp player_A=rtsp://<HOST>:8554/player_A player_B=rtsp://<HOST>:8554/player_B \
    --ws-port 8765

# 0.4 起静态服务器，浏览器打开导播页
python3 -m http.server 8080
# 浏览器访问  http://127.0.0.1:8080/director.html?ws=ws://127.0.0.1:8765&streams=player_A,player_B
#   query 参数： lead(领先量s，默认10)  lag(过旧告警ms,默认2000)  fps(状态显示)
```
> 说明：gts 领先量默认 10s → 处于 `拉流→HTML` 之间无需真等 1 分钟缓冲；
> 两流各有 ≥1 帧即算起 gts，领先量充当余量。若你想要更贴近"首尾对齐"的严格
> 缓冲语义，把 lead 调小或把 gateway 起流提前即可。

### 步骤 1｜两站推流（玩家 A / B）
- A/B：OBS 高级输出 → 串流编码器 `SEI Timestamp (H.264)` → 服务 Custom →
  `rtmp://<HOST>/live`，key `player_A` / `player_B`。NTP 开启。分辨率/帧率/码率/GOP 统一。
- 画面 = UTC 毫秒时钟（test-clock.html），用于最后视觉对拍。
- 观测者在导播页看到两路画面出现。

### 步骤 2｜自动对齐观察（导播页）
- 两流各有 ≥1 帧后，导播页按 §2 算出 gts（慢流起点 + lead），开始帧锁输出两路画面。
- **判定**：任何时刻，任一流显示帧的 `realtime_us ≤ gts`（绝无未来帧）；
  两路显示帧的真实时间差 `|realtime_A − realtime_B| ≤ 1 帧`（帧锁）。
- **视觉对拍**：暂停导播页，两幅 UTC 时钟的 `SS.mmm` 应相等（差 ≤1 帧）。

### 步骤 3｜离线对拍（交叉校验 SEI 本身没写错）
用 step1/2 落盘的两份抓录 + 工具：
```bash
python3 tools/verify_sei.py alice.ts --frames 300   # NTP coverage≈100%，帧间隔≈1/fps
python3 tools/align_streams.py alice.ts bob.ts      # median≤1/2帧, MAD≤1/4帧, drift<10ppm
```
并把 align_streams 的 median offset 与导播页里实测的两流帧差比对（差≤1帧）。
（这步把"SEI 写错"和"导播页逻辑错"分开归因。）

---

## 4. 判定标准

| 指标 | 通过阈值 |
|---|---|
| 插件版本/日志 | `sei-timestamp v0.1.0` |
| 推流成功 / 导播页出两路画面 | 在线 |
| verify_sei 帧间隔 / NTP coverage | ≈1/fps 无空洞 / ≥99% |
| 任一流显示帧 `realtime` | 恒 ≤ gts（无未来帧） |
| 两流显示帧时差 | ≤1 帧（60fps≈16ms） |
| 视觉对拍（两路 UTC 毫秒） | `SS.mmm` 差 ≤1 帧 |
| 10 min 兜底 | 不累积漂移，重测 align 仍达标 |

---

## 5. 失败归因

| 现象 | 先查 |
|---|---|
| 导播页无画面 | RTSP/MTX 端口、网关订阅、RTMP 是否 up |
| 有画面但两路明显错拍/时差大 | NTP 或某站时钟未同步；先跑 verify 看 NTP coverage |
| 画面一直停滞(不前进) | gts 领先量/tick 停摆，查 performance.now 推进 |
| 离线对拍与在线帧差对不上 | SEI 写错 → 查 sei-payload.c（而非导播页） |

---

## 6. 归档
各站 OBS 日志(http 0.1.0 行)+NTP 截图；观测者存两路抓录、导播页截图/录像、
gateway 日志、align/verify 输出。

---

## 7. 相关文件
协议 `docs/ARCHITECTURE.zh-CN.md` §5/§6；画钟 `tools/test-clock.html`；
离线工具 `tools/verify_sei.py`、`tools/align_streams.py`；
最小 demo `tools/sei_gateway.py`、`tools/director.html`。