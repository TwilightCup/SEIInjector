#!/usr/bin/env python3
"""Diagnose why the SEI gateway sees cfg=0 for a live RTSP stream.

Pulls the SAME stream the gateway pulls (same ffmpeg command), then scans the
raw Annex-B byte stream for 00 00 [00] 01 start codes and tallies every NAL
type -- independently of the gateway's AU-splitter, so we can see whether
SPS(7)/PPS(8)/SEI(6) are even present in what the gateway receives.

Differentiates the two failure modes:
  *  ffmpeg produced nothing at all  -> connectivity / ffmpeg problem (stderr shown)
  *  bytes arrived but SPS/PPS absent -> upstream really has no in-band parameter sets

Usage:
  python3 tools/dump_nal.py rtsp://192.168.2.201:8554/test [max_mb]
"""
import collections
import subprocess
import sys
import threading

NAL_NAMES = {1: "SLICE", 5: "IDR", 6: "SEI", 7: "SPS", 8: "PPS", 9: "AUD", 24: "SEI24?"}


def scan_nals(buf):
    """Yield the NAL type byte right after every start code in buf."""
    i, n = 0, len(buf)
    out = []
    while i < n:
        p = buf.find(b"\x00\x00\x00\x01", i)
        q = buf.find(b"\x00\x00\x01", i)
        cands = [x for x in (p, q) if x != -1]
        if not cands:
            break
        pos = min(cands)
        sc = 4 if (p != -1 and p == pos) else 3
        # avoid mistaking the tail of a 4-byte start code as an overlap
        if pos + sc + 1 <= n:
            out.append(buf[pos + sc] & 0x1F)
        i = pos + sc
    return out


def main():
    rtsp = sys.argv[1] if len(sys.argv) > 1 else "rtsp://192.168.2.201:8554/test"
    max_mb = int(sys.argv[2]) if len(sys.argv) > 2 else 8

    cmd = ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
           "-rtsp_transport", "tcp", "-i", rtsp,
           "-map", "0:v:0", "-c", "copy", "-f", "h264", "pipe:1"]

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    # mirror ffmpeg's stderr back to the terminal so connectivity errors are visible
    def drain_err():
        if proc.stderr:
            for line in iter(proc.stderr.readline, b""):
                sys.stderr.write("[ffmpeg] " + line.decode("utf-8", "replace"))
    threading.Thread(target=drain_err, daemon=True).start()

    print(f"pulling: {rtsp}  (capture up to {max_mb} MB)", flush=True)
    tally = collections.Counter()
    total = 0
    window = b""
    seen = {"sps": False, "pps": False, "sei": False, "au_boundary": False}
    while total < max_mb << 20:
        chunk = proc.stdout.read(1 << 20)
        if not chunk:
            break
        total += len(chunk)
        window += chunk
        # keep a rolling tail so a NAL straddling a chunk boundary isn't missed
        if len(window) > 1 << 22:
            window = window[-(1 << 22):]
        for t in scan_nals(window):
            tally[t] += 1
            if t == 7:
                seen["sps"] = True
            elif t == 8:
                seen["pps"] = True
            elif t == 6:
                seen["sei"] = True
            elif t in (1, 5):
                seen["au_boundary"] = True
        window = window[-12:]  # keep overlap only for next find()
    proc.kill()

    print("\n--- NAL type histogram over received bytes ---")
    for t, c in sorted(tally.items()):
        print(f"  type {t:<4} {NAL_NAMES.get(t, 'other'):<8} : {c}")
    print(f"\n  total bytes captured: {total:,} ({total / (1 << 20):.2f} MB)")
    if total == 0:
        print("RESULT: ffmpeg produced NO bytes (see [ffmpeg] stderr above) -> connectivity/ffmpeg issue")
        return
    print(f"  SPS(type7) seen : {seen['sps']}")
    print(f"  PPS(type8) seen : {seen['pps']}")
    print(f"  SEI(type6) seen : {seen['sei']}")
    print(f"  slice/IDR seen  : {seen['au_boundary']}   (confirms video frames reached the gateway's ffmpeg)")
    if not seen["sps"] or not seen["pps"]:
        print("\nRESULT: SPS and PPS not both present in-band -> upstream issue "
              "(OBS plugin 'inline parameter sets' build / MediaMTX stripping "
              "out-of-band SPS/PPS).")
    else:
        print("\nRESULT: SPS+PPS ARE present in-band -> stream is healthy.")
        print("        If cfg stays 0 despite this, pair them per-AU (gateway's _analyze)")
        print("        or check the gateway's ffmpeg pull actually reached the server.")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)