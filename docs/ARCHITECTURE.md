# Architecture (handover index)

The authoritative handover document for the **multi-player live → frame-level
alignment → director-view rebroadcast** pipeline is:

👉 [docs/ARCHITECTURE.zh-CN.md](ARCHITECTURE.zh-CN.md) (Chinese, detailed spec)

Quick summary:

```
Players (OBS + SEIInjector, NTP-synced)
   │  per-frame SEI: realtime_us / media_pts / seq   (H.264 or H.265)
   │  SRT publish
   ▼
MediaMTX   (one path per player: playerA, playerB, ...)
   │  RTSP/SRT read by "SEI Gateway" bridge (thin, no re-encode)
   ▼
Web director console  (this repo's plugin is NOT this part)
   │  WebSocket AU+SEI feed (§6 protocol) → WebCodecs decode
   │  frame-lock by realtime_us (§5) → multi-view / switching
   │  composite H.264 → WHIP back to MediaMTX path "director"
   ▼
ffmpeg → YouTube / Twitch / Bilibili (external always H.264)
```

Key contract points for the front-end agent are in the Chinese document:
§5 alignment algorithm + jitter budget, §6 WebSocket protocol v1 (frame/SEI
format, cfg/tick messages), §9 acceptance checklist. An alternative
server-side alignment engine is described in §8 if the browser should not
own composition.
