#!/usr/bin/env python3
"""
SEIInjector - demonstrate timeline alignment across several independent
streams using the per-frame real-time stamps.

Each sender runs OBS with the "SEI Timestamp (H.264/H.265)" encoder and (for
true cross-machine alignment) NTP correction enabled, so every frame carries
epoch-microsecond real-time stamps on a common clock. A pulling application
can then present frames of all streams at the same absolute time; this script
implements the measurement half of that: given N captured streams it reports
for each pair:

  * median relative offset (how much later/earlier one stream started, or the
    standing clock disagreement if NTP was off),
  * drift rate (ppm) between the senders' clocks over the captured window,
  * jitter / required smoothing buffer.

Usage:
    python3 tools/align_streams.py <stream1> <stream2> [...] [options]

Options:
    --codec h264|hevc   codec for all inputs (default: guess per file)
    --raw               treat every input as an Annex-B elementary stream

Example (after capturing two mp4s that were recorded simultaneously):
    python3 tools/align_streams.py person_a.mp4 person_b.mp4

Any media file ffmpeg understands works (mp4/ts/flv/mkv/rtmp pull pipes via
ffmpeg stdin are out of scope; pull them to disk first).
"""

import argparse
import math
import statistics
import struct
import subprocess
import sys

UUID = bytes.fromhex("7e57c2ee0dd24b539b3593edf97a12c1")
FIELD_SIZE = 22


def find_start_codes(data):
    i, n = 0, len(data)
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


def unescape(seg, want):
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


def read_var(data, pos, end):
    value = 0
    while pos < end and data[pos] == 0xFF:
        value += 0xFF
        pos += 1
    if pos >= end:
        return None, pos
    value += data[pos]
    return value, pos + 1


def parse_stream(codec, data):
    """Return sorted list of dicts with realtime_us / media_pts / seq / ntp."""
    frames = []
    starts = list(find_start_codes(data))
    for k, (pos, sc) in enumerate(starts):
        ns, ne = pos + sc, starts[k + 1][0] if k + 1 < len(starts) else len(data)
        nal = data[ns:ne]
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
        ptype, p = read_var(nal, p, len(nal))
        msize, p = read_var(nal, p, len(nal)) if ptype is not None else (None, p)
        if ptype is None or msize is None or msize < 16 + FIELD_SIZE:
            continue
        unesc = unescape(nal[p:p + msize * 2 + 8], msize)
        if len(unesc) < 16 + FIELD_SIZE or not unesc.startswith(UUID):
            continue
        if unesc[16] != 1:
            continue
        flags = unesc[17]
        seq = struct.unpack(">I", unesc[18:22])[0]
        pts = struct.unpack(">q", unesc[22:30])[0]
        rt = struct.unpack(">q", unesc[30:38])[0]
        frames.append({"seq": seq, "media_pts": pts,
                       "realtime_us": rt,
                       "ntp": bool(flags & 0x02)})
    frames.sort(key=lambda f: f["realtime_us"])
    return frames


def demux(path, codec):
    bsf = "h264_mp4toannexb" if codec == "h264" else "hevc_mp4toannexb"
    fmt = "h264" if codec == "h264" else "hevc"
    cmd = ["ffmpeg", "-v", "error", "-i", path, "-map", "0:v:0",
           "-c:v", "copy", "-bsf:v", bsf, "-f", fmt, "pipe:1"]
    proc = subprocess.run(cmd, capture_output=True)
    if proc.returncode != 0:
        raise SystemExit(f"ffmpeg demux failed for {path}\n"
                         + proc.stderr.decode(errors="replace"))
    return proc.stdout


RAW_EXTS = {"h264", "264", "hevc", "h265", "265"}


def guess_codec(path):
    ext = path.rsplit(".", 1)[-1].lower() if "." in path else ""
    if ext in RAW_EXTS:
        return "h265" if ext in ("hevc", "h265", "265") else "h264"
    return None


def is_raw_path(path):
    ext = path.rsplit(".", 1)[-1].lower() if "." in path else ""
    return ext in RAW_EXTS


def load(path, codec, raw):
    if raw or is_raw_path(path):
        c = codec or guess_codec(path) or "h264"
        data = open(path, "rb").read()
    else:
        c = codec or "h264"
        data = demux(path, c)
    frames = parse_stream(c, data)
    if not frames:
        sys.stderr.write(f"WARNING: {path}: no stamped frames found\n")
    return c, frames


def linreg(xs, ys):
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    num = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    den = sum((x - mx) ** 2 for x in xs)
    if den == 0:
        return 0.0, my
    slope = num / den
    return slope, my - slope * mx


def pair_report(a, b, name_a, name_b):
    """a/b: sorted lists of {realtime_us}. Estimates b's stamp offset and
    clock drift relative to a during their temporal overlap."""
    if not a or not b:
        return None

    # Only pair frames that plausibly refer to the same wall-clock instant.
    # Real concurrent streams sit well inside this window (network + buffer
    # jitter); captures from unrelated sessions produce no pairs and are
    # reported as "no overlap" instead of a bogus alignment.
    W_US = 2_000_000

    a_rt = [f["realtime_us"] for f in a]
    b_rt = [f["realtime_us"] for f in b]

    pairs = []  # (b frame realtime, b_realtime - nearest_a_realtime)
    ia_lo = 0
    na = len(a_rt)
    for bv in b_rt:
        lo = bv - W_US
        hi = bv + W_US
        while ia_lo < na and a_rt[ia_lo] < lo:
            ia_lo += 1
        ia = ia_lo
        best, best_abs = None, None
        while ia < na and a_rt[ia] <= hi:
            d = bv - a_rt[ia]
            ad = abs(d)
            if best_abs is None or ad < best_abs:
                best, best_abs = d, ad
            ia += 1
            # once the distance starts growing again, stop early
            if best_abs is not None and ad > best_abs + 25000:
                break
        if best is not None:
            pairs.append((bv, best))

    if len(pairs) < 8:
        return {"n": len(pairs), "overlap": False}

    diffs = sorted(d for _, d in pairs)
    n = len(diffs)
    median = diffs[n // 2]
    mad = statistics.median(abs(d - median) for d in diffs)
    lo, hi = diffs[n // 8], diffs[n - 1 - n // 8]
    xs = [bv for bv, d in pairs if lo <= d <= hi]
    ys = [d for bv, d in pairs if lo <= d <= hi]
    slope, intercept = linreg(xs, ys)

    return {
        "n": n,
        "overlap": True,
        "median_us": median,
        "mad_us": mad,
        "range_us": diffs[-1] - diffs[0],
        # slope is us of disagreement per us of wall time -> ppm = slope * 1e6
        "drift_ppm": slope * 1e6,
        "intercept_us": intercept,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="+")
    ap.add_argument("--codec", choices=["h264", "hevc"])
    ap.add_argument("--raw", action="store_true")
    args = ap.parse_args()
    if len(args.inputs) < 2:
        raise SystemExit("need at least two streams")

    streams = []
    for p in args.inputs:
        codec, frames = load(p, args.codec, args.raw)
        streams.append((p, codec, frames))
        if frames:
            rt0 = frames[0]["realtime_us"]
            rt1 = frames[-1]["realtime_us"]
            ntp = sum(1 for f in frames if f["ntp"])
            print(f"{p}: {len(frames)} stamped frames over "
                  f"{(rt1 - rt0) / 1e6:.2f} s  (NTP-stamped {ntp}/{len(frames)})")

    # sanity: reject obviously-not-simultaneous capture windows when every
    # stream reports NTP stamps and the windows do not overlap at all.
    print("\npairwise alignment (stream B vs stream A):")
    print("  'median' > 0 means B's stamps run later than A's at the same")
    print("  instant; drift > 0 means B's clock runs fast relative to A.\n")
    row = "{:<18} {:>12} {:>10} {:>12} {:>12} {:>12}"
    print(row.format("pair", "median(ms)", "MAD(ms)", "range(ms)",
                     "drift(ppm)", "pairs"))
    ref = streams[0]
    for other in streams[1:]:
        r = pair_report(ref[2], other[2], ref[0], other[0])
        if not r:
            print(f"{other[0]} vs {ref[0]}: no stamped frames")
        elif not r["overlap"]:
            print(f"{other[0]} vs {ref[0]}: no temporal overlap "
                  f"({r['n']} candidate pairs) - capture the streams "
                  f"simultaneously to align them")
        else:
            print(row.format(f"{other[0]} vs {ref[0]}",
                             f"{r['median_us'] / 1000.0:.2f}",
                             f"{r['mad_us'] / 1000.0:.2f}",
                             f"{r['range_us'] / 1000.0:.2f}",
                             f"{r['drift_ppm']:.3f}",
                             r["n"]))

    print("""
How to use this at the pull side:
  1. Sort each stream's frames by realtime_us.
  2. Choose a common presentation target time T (e.g. the newest frame
     among all streams minus a safety buffer of 200-500 ms).
  3. For each stream, present the frame whose realtime_us is closest to T.
     If all senders share the NTP-corrected epoch, all streams are then
     frame-locked; MAD values above show the residual mismatch.
  4. The range/column tells you how large the smoothing buffer must be to
     never underrun while waiting for the slowest stream.""")
    return 0


if __name__ == "__main__":
    sys.exit(main())
