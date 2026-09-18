#!/usr/bin/env python3
"""
SEIInjector - minimal SEI Gateway (demo).

Reads every player's RTSP stream out of MediaMTX, splits it into H.264 Access
Units, picks the per-frame SEI (realtime_us / seq / keyframe) out of each AU,
and pushes the raw AU + SEI over a WebSocket. The browser director subscribes
to two streams and verifies they are frame-locked by their realtime stamps.

AU order produced by the plugin is [prefix][SPS/PPS (keyframe only)][SEI][video],
so the splitter keeps all slices until the next AU prefix or first slice.
It supports ordinary progressive H.264 (types 1/5), not FMO/ASO/MVC/field pictures.
Read chunk boundaries are never treated as AU boundaries.

Front-end contract (trimmed from docs/ARCHITECTURE.zh-CN.md §6):

  text  {"t":"cfg","s":id,"c":"h264","codec":"avc1...","description_b64":..}
  bin   [uint32 BE header_len][header json utf8]{"t":"au","s":..,"k":..,"seq":..,"rt":..}
        followed by the raw Annex-B AU bytes (SPS/PPS inline on keyframes).

Usage:
  python3 tools/sei_gateway.py \
      --rtsp player_A=rtsp://HOST:8554/player_A player_B=rtsp://HOST:8554/player_B \
      --ws-port 8765

Deps: ffmpeg on PATH, the `websockets` Python package.
"""
import argparse
import asyncio
import base64
import json
import struct
import subprocess
import sys
import threading
import time

try:
    import websockets
except ModuleNotFoundError:      # parse/split helpers still usable without it
    websockets = None

# name -> cached cfg text message, resent to every newly connected client
stream_cfg = {}

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


def parse_sei(nal):
    """Return {seq, realtime_us, keyframe, ntp} from one H.264 SEI(6) NAL."""
    if len(nal) < 3 or (nal[0] & 0x1F) != 6:
        return None
    p = 1
    ptype, p = read_var(nal, p, len(nal))
    msize, p = read_var(nal, p, len(nal)) if ptype is not None else (None, p)
    if ptype is None or msize is None or msize < 16 + FIELD_SIZE:
        return None
    unesc = unescape(nal[p:p + msize * 2 + 8], msize)
    if len(unesc) < 16 + FIELD_SIZE or not unesc.startswith(UUID):
        return None
    if unesc[16] != 1:
        return None
    flags = unesc[17]
    return {"seq": struct.unpack(">I", unesc[18:22])[0],
            "realtime_us": struct.unpack(">q", unesc[30:38])[0],
            "keyframe": bool(flags & 0x01), "ntp": bool(flags & 0x02)}


def build_avcc(sps, pps):
    rec = bytearray([1, sps[1], sps[2], sps[3], 0xFC | 3, 0xE0 | 1])
    rec += struct.pack(">H", len(sps)) + bytes(sps)
    rec += bytes([1])
    rec += struct.pack(">H", len(pps)) + bytes(pps)
    return bytes(rec)


def _nal_list(buf):
    """Yield (nal_bytes) for every NAL in buf."""
    starts = list(find_start_codes(bytes(buf)))
    if not starts:
        return
    buf_len = len(buf)
    for idx, (pos, sc) in enumerate(starts):
        ns = pos + sc
        ne = starts[idx + 1][0] if idx + 1 < len(starts) else buf_len
        yield bytes(buf[ns:ne])


def _analyze(au):
    """One pass over an AU -> (is_key, meta or None, sps or None, pps or None)."""
    is_key, meta, sps, pps = False, None, None, None
    for nal in _nal_list(au):
        if len(nal) < 2:
            continue
        t = nal[0] & 0x1F
        if t == 7:
            sps = bytes(nal)
        elif t == 8:
            pps = bytes(nal)
        elif t == 6:
            m = parse_sei(nal)
            if m:
                meta = m
        elif t == 5:
            is_key = True
    if meta and meta["keyframe"]:
        is_key = True
    return is_key, meta, sps, pps


def _first_mb_in_slice(nal):
    """Decode only the first ue(v); None means more bytes are needed."""
    rbsp = bytearray()
    zeros = 0
    for value in nal[1:]:
        if zeros >= 2 and value == 3:
            zeros = 0
            continue
        rbsp.append(value)
        zeros = zeros + 1 if value == 0 else 0
    bits = len(rbsp) * 8
    leading = 0
    while leading < bits and not (rbsp[leading // 8] & (0x80 >> (leading % 8))):
        leading += 1
    if leading > 31:
        raise ValueError("invalid H.264 first_mb_in_slice")
    if leading == bits or bits < 2 * leading + 1:
        return None
    value = 1
    for pos in range(leading + 1, 2 * leading + 1):
        value = (value << 1) | ((rbsp[pos // 8] >> (7 - pos % 8)) & 1)
    return value - 1


def _first_au_end(buf):
    """Keep every slice of a progressive picture; never split on read chunks.

    After VCL, AUD/SEI/parameter sets start the next AU. Without such a
    prefix, first_mb_in_slice == 0 starts the next ordinary progressive frame.
    Full H.264 picture-boundary parsing (fields/FMO/ASO/MVC) is not implemented.
    """
    starts = list(find_start_codes(bytes(buf)))
    have_vcl = False
    for idx, (pos, sc) in enumerate(starts):
        ns = pos + sc
        if ns >= len(buf):
            return None
        ntype = buf[ns] & 0x1F
        if have_vcl and ntype in (6, 7, 8, 9, 14, 15, 16, 17, 18):
            return pos
        if ntype in (2, 3, 4, 20, 21):
            raise ValueError("unsupported partitioned/extended H.264 slice")
        if ntype in (1, 5):
            end = starts[idx + 1][0] if idx + 1 < len(starts) else len(buf)
            first_mb = _first_mb_in_slice(buf[ns:end])
            if first_mb is None:
                if idx + 1 < len(starts):
                    raise ValueError("truncated H.264 slice header")
                return None
            if have_vcl and first_mb == 0:
                return pos
            have_vcl = True
    return None


def split_aus(stream, au_cap=(8 << 20)):
    """Yield raw AU bytes found after a frame boundary. `au_cap` bounds a single
    AU so a pathological run (e.g. a missed boundary) can't balloon to gigabytes
    and break the WebSocket consumers."""
    buf = bytearray()
    while True:
        chunk = stream.read(1 << 16)
        if not chunk:
            if buf:
                yield bytes(buf)
            return
        buf += chunk
        while True:
            end = _first_au_end(buf)
            if (end is None and len(buf) > au_cap) or (end is not None and end > au_cap):
                raise ValueError("AU exceeds cap without a safe picture boundary")
            if end is None:
                break
            au = bytes(buf[:end])
            del buf[:end]
            if au:
                yield au


class Streamer(threading.Thread):
    def __init__(self, name, rtsp, loop, outq):
        super().__init__(daemon=True)
        self.name, self.rtsp = name, rtsp
        self.loop, self.outq = loop, outq

    def run(self):
        cmd = ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
               "-i", self.rtsp, "-map", "0:v:0", "-c", "copy",
               "-f", "h264", "pipe:1"]
        # Respawn ffmpeg if it exits or stalls; the pull side must keep a live
        # feed regardless of RTSP/streamer hiccups.
        while True:
            sent_cfg = False
            proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE)
            try:
                for au in split_aus(proc.stdout):
                    is_key, meta, sps, pps = _analyze(bytes(au))
                    if not is_key and not meta:
                        continue  # nothing useful; skip
                    if not sent_cfg and sps is not None and pps is not None:
                        avcc = build_avcc(sps, pps)
                        hdr = json.dumps({
                            "t": "cfg", "s": self.name, "c": "h264",
                            "codec": "avc1.%02X%02X%02X"
                                     % (sps[1] & 0xFF, sps[2] & 0xFF, sps[3] & 0xFF),
                            "description_b64": base64.b64encode(avcc).decode()})
                        self._send(hdr)  # str -> text frame (not binary)
                        stream_cfg[self.name] = hdr   # cache for late-connecting clients
                        sent_cfg = True
                    rt = meta["realtime_us"] if meta else 0
                    seq = meta["seq"] if meta else 0
                    ntp = meta["ntp"] if meta else False
                    hdr = json.dumps({"t": "au", "s": self.name, "k": bool(is_key),
                                      "seq": seq, "rt": rt, "n": ntp,
                                      "sz": len(au)}).encode()
                    self._send(struct.pack(">I", len(hdr)) + hdr + bytes(au))
            except Exception as e:  # noqa: BLE001
                sys.stderr.write(f"[{self.name}] {e}\n")
            finally:
                try:
                    proc.stdout.close()
                finally:
                    proc.kill()
            time.sleep(1.5)   # breathe before respawning ffmpeg

    def _send(self, msg):
        asyncio.run_coroutine_threadsafe(self.outq.put(msg), self.loop)


async def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rtsp", nargs="+", required=True, metavar="NAME=url")
    ap.add_argument("--ws-port", type=int, default=8765)
    args = ap.parse_args()

    streams = {}
    for item in args.rtsp:
        name, _, url = item.partition("=")
        streams[name] = url

    loop = asyncio.get_running_loop()
    outq = asyncio.Queue()
    clients = set()

    for name, url in streams.items():
        Streamer(name, url, loop, outq).start()

    async def broadcast():
        while True:
            msg = await outq.get()
            dead = []
            for ws in list(clients):
                try:
                    await ws.send(msg)
                except Exception:  # noqa: BLE001
                    dead.append(ws)
            for ws in dead:
                clients.discard(ws)

    async def ticker():
        while True:
            await asyncio.sleep(0.5)
            await outq.put(json.dumps(
                {"t": "tick", "epoch_us": int(time.time() * 1e6)}))  # str -> text frame

    async def handler(ws):
        clients.add(ws)
        print(f"  + ws client ({len(clients)})")
        # Resend each stream's cached cfg (SPS/PPS) so a client that connects
        # after the one-time cfg was broadcast still gets the decoder config.
        try:
            for name, msg in stream_cfg.items():
                await ws.send(msg)
        except Exception:  # noqa: BLE001
            pass
        try:
            async for _ in ws:
                pass
        finally:
            clients.discard(ws)
            print(f"  - ws client ({len(clients)})")

    async def server():
        async with websockets.serve(handler, "0.0.0.0", args.ws_port):
            await asyncio.Future()  # run forever

    if websockets is None:
        sys.exit("missing dependency: pip install websockets")
    await asyncio.gather(server(), broadcast(), ticker())


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)