#!/usr/bin/env python3
"""
SEIInjector - verify that a captured/pulled media file carries our per-frame
timestamp SEI and print a frame-by-frame report.

Requires ffmpeg on PATH (used only to demux the video elementary stream; the
SEI parsing itself is pure Python and mirrors src/sei-payload.c).

Usage:
    python3 tools/verify_sei.py <input.[mp4|ts|flv|mkv|h264|hevc]> [options]

Options:
    --codec h264|hevc   override detection (default: guess from extension)
    --frames N          stop after N stamped frames
    --raw               input is already an Annex-B elementary stream (no ffmpeg)

Example:
    python3 tools/verify_sei.py recording.mp4 --frames 200
"""

import argparse
import datetime as dt
import struct
import subprocess
import sys

UUID = bytes.fromhex("7e57c2ee0dd24b539b3593edf97a12c1")
FIELD_SIZE = 22  # after the 16-byte UUID
FLAG_KEYFRAME = 0x01
FLAG_CLOCK_NTP = 0x02


def find_start_codes(data):
    """Yield byte offsets of 3- or 4-byte Annex-B start codes."""
    i = 0
    n = len(data)
    while i + 3 <= n:
        if data[i] == 0 and data[i + 1] == 0:
            if data[i + 2] == 1:
                yield i, 3
                i += 3
                continue
            if i + 4 <= n and data[i + 2] == 0 and data[i + 3] == 1:
                yield i, 4
                i += 4
                continue
        i += 1


def unescape_segment(seg, want):
    """Remove EPB 0x03 bytes until `want` unescaped bytes are produced."""
    out = bytearray()
    zeros = 0
    for b in seg:
        if zeros >= 2 and b == 0x03:
            zeros = 0
            continue
        out.append(b)
        if b == 0:
            zeros += 1
        else:
            zeros = 0
        if len(out) >= want:
            break
    return bytes(out)


def read_sei_var(data, pos, end):
    value = 0
    while pos < end and data[pos] == 0xFF:
        value += 0xFF
        pos += 1
        if value > 0x1000000:
            return None, pos
    if pos >= end:
        return None, pos
    value += data[pos]
    return value, pos + 1


def parse_access_unit(codec, data):
    """Yield (info_dict) for every of our SEI NALs in an Annex-B stream."""
    starts = list(find_start_codes(data))
    for k, (pos, sc) in enumerate(starts):
        nal_start = pos + sc
        nal_end = starts[k + 1][0] if k + 1 < len(starts) else len(data)
        nal = data[nal_start:nal_end]
        if len(nal) < 2:
            continue

        if codec == "h264":
            ntype = nal[0] & 0x1F
            hdr = 1
            is_sei = ntype == 6
        else:
            ntype = (nal[0] >> 1) & 0x3F
            hdr = 2
            is_sei = ntype in (39, 40)

        if not is_sei:
            continue

        p = hdr
        ptype, p = read_sei_var(nal, p, len(nal))
        if ptype is None:
            continue
        msize, p = read_sei_var(nal, p, len(nal))
        if msize is None or msize < 16 + FIELD_SIZE:
            continue

        wire = nal[p:p + msize * 2 + 8]
        unesc = unescape_segment(wire, msize)
        if len(unesc) < 16 + FIELD_SIZE or not unesc.startswith(UUID):
            continue
        if unesc[16] != 1:  # payload version
            continue
        flags = unesc[17]
        seq = struct.unpack(">I", unesc[18:22])[0]
        pts = struct.unpack(">q", unesc[22:30])[0]
        realtime_us = struct.unpack(">q", unesc[30:38])[0]
        yield {
            "seq": seq,
            "keyframe": bool(flags & FLAG_KEYFRAME),
            "clock_ntp": bool(flags & FLAG_CLOCK_NTP),
            "media_pts": pts,
            "realtime_us": realtime_us,
        }


def demux_to_annexb(path, codec):
    bsf = "h264_mp4toannexb" if codec == "h264" else "hevc_mp4toannexb"
    fmt = "h264" if codec == "h264" else "hevc"
    cmd = ["ffmpeg", "-v", "error", "-i", path, "-map", "0:v:0",
           "-c:v", "copy", "-bsf:v", bsf, "-f", fmt, "pipe:1"]
    proc = subprocess.run(cmd, capture_output=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr.decode(errors="replace"))
        raise SystemExit("ffmpeg demux failed (is the codec correct?)")
    return proc.stdout


def guess_codec(path):
    ext = path.rsplit(".", 1)[-1].lower() if "." in path else ""
    if ext == "h264":
        return "h264"
    if ext in ("hevc", "h265", "265"):
        return "hevc"
    return "h264"  # default guess; override with --codec


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("--codec", choices=["h264", "hevc"])
    ap.add_argument("--frames", type=int, default=0)
    ap.add_argument("--raw", action="store_true")
    args = ap.parse_args()

    codec = args.codec or guess_codec(args.input)
    if args.raw:
        data = open(args.input, "rb").read()
    else:
        data = demux_to_annexb(args.input, codec)

    print(f"codec={codec}  stream_bytes={len(data)}")
    frames = []
    for info in parse_access_unit(codec, data):
        frames.append(info)
        if args.frames and len(frames) >= args.frames:
            break

    if not frames:
        print("no SEI Timestamp frames found - was this recorded with the "
              "plugin encoder?")
        return 1

    print(f"stamped frames: {len(frames)}")
    hdr = ("seq    key ntp    media_pts       realtime(UTC)          "
           "dt_ms")
    print(hdr)
    prev = None
    for f in frames:
        rt = dt.datetime.fromtimestamp(f["realtime_us"] / 1e6, tz=dt.timezone.utc)
        d = ""
        if prev is not None:
            d = f"{(f['realtime_us'] - prev) / 1000.0:9.2f}"
        print(f"{f['seq']:6d} {'K' if f['keyframe'] else '.':>3} "
              f"{'N' if f['clock_ntp'] else '-':>3} {f['media_pts']:12d} "
              f"{rt:%Y-%m-%d %H:%M:%S.%f} {d}")
        prev = f["realtime_us"]

    # simple diagnostics
    ts = [f["realtime_us"] for f in frames]
    deltas = [ts[i + 1] - ts[i] for i in range(len(ts) - 1)]
    if deltas:
        mean_ms = sum(deltas) / len(deltas) / 1000.0
        print(f"mean frame interval: {mean_ms:.3f} ms "
              f"(~{1000.0 / mean_ms:.2f} fps); "
              f"min {min(deltas) / 1000:.3f} ms max {max(deltas) / 1000:.3f} ms")
    ntp_frames = sum(1 for f in frames if f["clock_ntp"])
    if ntp_frames != len(frames):
        print(f"WARNING: {len(frames) - ntp_frames} frame(s) carry local-clock "
              "stamps (NTP not yet synced at that moment)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
