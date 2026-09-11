# SEIInjector — OBS plugin: frame-level real-time stamps in pushed video

An [OBS Studio](https://obsproject.com) plugin that stamps **every encoded
video frame** with the frame's **real-world time** (NTP-corrected epoch
microseconds) inside an H.264 / H.265 **SEI** message. A pulling application
(playback server, viewer, multi-view tool, …) can then read those stamps and
align the timelines of **several independent streamers frame-by-frame**.

This project was inspired by, and is architecturally derived from,
[SEI-Stamper](../SEI-Stamper) (which stamps only *keyframes* and focuses
on OBS-internal NTP receivers). SEIInjector is a smaller, sender-side-only
plugin that stamps **every frame**, keeps a strict wire format, and ships
verification / alignment tooling for your own pull side.

> Status: source complete for OBS ≥ 31 (validated syntactically against OBS
> 31.0.1 + FFmpeg 6.1 headers; the H.264 path matches the layout proven by
> SEI-Stamper on OBS 32.1.x). Build & test it against the OBS version you
> ship to — see [Building](#building).

---

## Why SEI?

OBS does not expose the encoded bitstream of its built-in encoders to
plugins. The only reliable way to inject per-frame data into the pushed
stream is to **be the encoder**: SEIInjector registers two custom video
encoders that internally wrap an FFmpeg encoder that is already bundled with
OBS (libx264 / NVENC / AMF / QuickSync / VideoToolbox / MediaFoundation),
rewrite every returned access unit, and hand the modified packet to OBS for
streaming/recording. SEI travels in-band inside the H.264/H.265 elementary
stream, so it survives RTMP/FLV, SRT/MPEG-TS, WebRTC gateways and recordings
unchanged.

---

## Features

* **Per-frame stamps** — every coded frame (I/P/B) carries one SEI NAL
  (~60 bytes) with `realtime_us`, the frame's `media_pts`, a stream `frame_seq`
  and flags. Overhead ≈ 30 kbit/s at 60 fps — negligible.
* **Absolute time base** — an optional background SNTP client (default
  `pool.ntp.org`) keeps the sender's clock aligned to UTC epoch; stamps then
  mean the same instant on every machine. Falls back to the local wall clock
  gracefully when NTP is off or unreachable.
* **Two codecs** — `SEI Timestamp (H.264)` (recommended, maximum
  compatibility) and `SEI Timestamp (H.265/HEVC)`.
* **Any underlying encoder present in OBS** — the encoder picker probes the
  running OBS's FFmpeg and only lists what exists.
* **Container-safe headers** — codec sequence headers (avcC/hvcC) are given
  to OBS for RTMP/FLV/mp4, and SPS/PPS/(VPS) are re-inserted inline on
  keyframes so MPEG-TS/SRT receivers that join mid-stream can decode.
* **Zero extra runtime dependencies** — only the DLL/`.so` plus locale files;
  FFmpeg, sockets and threading come from OBS / the OS.

---

## Install

1. Download/`git clone` this repository and build (below).
2. Copy the produced module to OBS:
   * Windows: `obs-plugins\64bit\sei-timestamp.dll`
   * macOS:   `obs-studio.app/Contents/PlugIns/sei-timestamp.so`
   * Linux:   `/usr/lib/obs-plugins/sei-timestamp.so` (distro-dependent)
3. Copy `data/locale/*` to `data/obs-plugins/sei-timestamp/locale/` next to the
   OBS data directory.
4. Restart OBS.

No additional DLLs are needed: FFmpeg symbols are resolved from the FFmpeg
that OBS Studio already loads.

## Usage (sender)

1. **Settings → Output → Output Mode: Advanced**.
2. Pick **Streaming encoder** = `SEI Timestamp (H.264)` (or H.265).
3. In the encoder properties:
   * **Encoder**: software (libx264) or your hardware encoder (NVENC/AMF/…).
     If a listed encoder fails to start, the OBS log contains the reason —
     usually a preset/profile combination the driver rejects; try `VBR` or
     another preset.
   * **Timestamp Settings** (group): leave *NTP time correction* on for
     cross-machine alignment; pick a reachable **NTP server**.
4. Stream to your usual destination (RTMP/SRT server). Every video frame now
   carries its real-world capture time.

> Everyone whose streams must be aligned should use the same NTP reference
> (or well-synced OS clocks). NTP corrections are applied by a background
> thread and never block "Start Streaming".

## Pull side

Read the SEI out of each received stream (see `tools/` for reference
implementations):

1. **Demux** to Annex-B (any ffmpeg can do this; also preserves in-band SEI
   from SRT/RTSP/FLV pulls).
2. **Find SEI NALs** whose 16-byte UUID matches
   `7e57c2ee-0dd2-4b53-9b35-93edf97a12c1`.
3. **Decode** the payload (see below) → per-frame `realtime_us`.
4. **Align**: sort frames of every stream by `realtime_us`; at presentation
   time `T` show, per stream, the frame closest to `T`. With a shared NTP
   epoch all senders' frames at the same `T` were captured at the same wall
   instant, so the multi-stream picture is frame-locked. Keep a smoothing
   buffer of ~200–500 ms to absorb network jitter.

### SEI payload format

Payload type `5` (user data unregistered), payload starts with the UUID:

```
offset  size  field            meaning
------  ----  ---------------  --------------------------------------------
0       16    UUID             7e57c2ee-0dd2-4b53-9b35-93edf97a12c1
16      1     version          = 1
17      1     flags            bit0 keyframe; bit1 clock NTP-corrected
18      4     frame_seq        coded-frame counter since encoder start (BE)
22      8     media_pts        PTS of this frame (BE, OBS timebase units)
30      8     realtime_us      capture wall time, microseconds since epoch (BE)
```

All multi-byte fields big-endian. `realtime_us` is NTP-corrected when
`flags & 0x02`, otherwise the sender's local wall clock.

### Tools

* `tools/verify_sei.py <capture.mp4|ts|flv|mkv|h264>` — demuxes with ffmpeg
  and prints a per-frame stamp report (interval, fps, NTP coverage). Great for
  QA after recording.
* `tools/align_streams.py <a.mp4> <b.mp4> ...` — measures pairwise median
  offset, jitter (MAD), clock drift (ppm) and the required smoothing buffer,
  and prints the pull-side alignment recipe. Two streams recorded at totally
  different times are reported as "no temporal overlap".

### End-to-end pipeline design

For the "game-match players → frame-level alignment → director view → public
live" chain, see **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** /
**[docs/ARCHITECTURE.zh-CN.md](docs/ARCHITECTURE.zh-CN.md)** — roles,
MediaMTX layout, the alignment algorithm & jitter budget, the SEI Gateway
WebSocket contract for a WebCodecs-based web director console, and the
front-end acceptance checklist.

---

## Building

You build against an OBS source tree (same approach as SEI-Stamper). At the
top of this repo, either clone OBS into `obs-studio-master` or point CMake at
an existing one.

Requirements:
* CMake ≥ 3.20, a C11 compiler.
* OBS Studio **sources** (headers + built `libobs`) — version ≥ 31.
* FFmpeg headers + libs (avcodec, avutil): from OBS's bundled deps
  (`obs-deps-*`), `pkg-config`/`--enable-libav*`, or Homebrew on macOS.

### Windows (from source, mirroring SEI-Stamper)

```bat
git clone https://github.com/obsproject/obs-studio.git obs-studio-master
:: ... build OBS Studio itself (or download a prebuilt dev environment)
cmake -S . -B build -DOBS_DIR=%CD%\obs-studio-master\build\rundir\Release ^
                     -DOBS_DEPS_DIR=%CD%\obs-studio-master\.deps\obs-deps-2025-08-23-x64
cmake --build build --config Release
cmake --install build
```

### macOS

```sh
# build OBS first (or use an OBS dev prefix) then:
cmake -S . -B build -DOBS_SOURCE_DIR=/path/obs-studio/libobs \
                     -DOBS_LIB_DIR=/path/obs-studio/build/libobs \
                     -Dlibobs_DIR=$(find /path/obs-studio/build -name 'libobsConfig.cmake' -exec dirname {} \;) \
                     -DFFMPEG_INCLUDE_DIR=/opt/homebrew/include \
                     -DFFMPEG_LIBRARY_DIR=/opt/homebrew/lib
cmake --build build
```

Link the resulting `sei-timestamp.so` against the OBS-bundled FFmpeg
dylibs (`@rpath`-based, inside `obs-studio.app`) or statically vendored ones,
matching how OBS itself links `obs-ffmpeg`.

### Unit tests (no OBS / FFmpeg needed)

```sh
cc -std=c11 -Wall -Wextra -Isrc -o sei_core_tests tests/test_sei.c \
   src/sei-payload.c src/h26x-util.c
./sei_core_tests
# or, once configured:  ctest --test-dir build
```

The tests verify H.264/H.265 SEI NAL construction (incl. emulation-prevention
escaping), round-trip parsing, avcC/hvcC→Annex-B conversion and prefix-NAL
scanning. The realtime-clock SNTP path can be smoke-tested directly:
`tools/` plus a tiny driver calling `realtime_clock_create("pool.ntp.org",…)`.

---

## Design notes / limitations

* **You must select this plugin as the *Streaming* encoder.** It is a normal
  OBS encoder, so it also works for recording and for OBS's native SRT output
  (Custom → `srt://…`).
* **Capture-time approximation**: stamps are recorded when OBS submits a
  frame to the encoder (~one frame behind capture, constant per machine),
  then attributed to the correct packet by PTS. Residual bias is a constant
  per stream — it does not hurt alignment between streams.
* **Clock discipline** is what makes alignment absolute. With NTP enabled the
  residual error is typically a few ms to tens of ms (< 1 frame at 30 fps).
  With NTP disabled, alignment quality depends on how close the OS wall
  clocks are (Windows Time / `timed`-style services are usually < 100 ms).
* **RTMP servers** and players tolerate in-band SEI and duplicate parameter
  sets; MPEG-TS/SRT need the inline keyframe headers, which are re-inserted.
  If you hit a picky ingest, check the OBS log: the plugin logs the FFmpeg
  encoder, presets and any `avcodec_open2` failure.
* **H.265 over classic RTMP** is not universally supported by servers —
  prefer H.264 for maximum reach; H.265 is fully usable with SRT/TS and the
  newer FLV/HEVC-capable ingests.
* Video-only stamping: audio needs no stamps — A/V sync inside each stream is
  OBS's job; the SEI gives you cross-stream alignment.

## Layout

```
src/
  sei-timestamp-plugin.c     module entry, encoder registration
  sei-timestamp-encoder.{h,c} custom H.264/H.265 encoder (FFmpeg wrap + SEI)
  sei-payload.{h,c}          SEI wire format, builder & parser (pure C)
  h26x-util.{h,c}            Annex-B scanning, avcC/hvcC -> Annex-B (pure C)
  realtime-clock.{h,c}       epoch clock + background SNTP (pure C)
tests/test_sei.c             standalone core-logic tests
tools/verify_sei.py          stamp report for captured media
tools/align_streams.py       multi-stream alignment measurement
data/locale/*.ini            OBS locale data
```

## License

GPL-2.0-or-later, in line with OBS Studio. See [LICENSE](LICENSE).
Portions of the architecture follow SEI-Stamper (GPL-2.0).
