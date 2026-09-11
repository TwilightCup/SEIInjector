#!/usr/bin/env python3
"""Transparent TCP relay for the RTSP pull, to dodge a Homebrew-block.

Symptom: /opt/homebrew/bin/ffmpeg and python3 get ENETUNREACH ("No route to
host") to the local subnet (192.168.2.0/24) but Apple binaries connect fine.
Homebrew binaries DO reach 127.0.0.1 (loopback), so we relay ffmpeg's traffic
through loopback to the real RTSP server.

  ffmpeg -> 127.0.0.1:18554  ->(this relay)-->  192.168.2.201:8554  (MediaMTX)

RTSP-over-TCP is a plain bidirectional byte stream, so a dumb TCP splice is
all that's needed; the relay is transparent to ffmpeg/MediaMTX.

Usage:
  python3 tools/rtsp_relay.py [listen_port] [target_host] [target_port]
    defaults: 127.0.0.1:18554  ->  192.168.2.201:8554
"""
import socket
import sys
import threading

LISTEN_HOST = "127.0.0.1"
LISTEN_PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18554
TARGET_HOST = sys.argv[2] if len(sys.argv) > 2 else "192.168.2.201"
TARGET_PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 8554

BUF = 1 << 16


def _bridge(src, dst):
    """Forward one direction until EOF; then half-close dst."""
    try:
        while True:
            data = src.recv(BUF)
            if not data:
                break
            dst.sendall(data)
    except OSError:
        pass
    finally:
        try:
            dst.shutdown(socket.SHUT_WR)
        except OSError:
            pass


def handle(client):
    """Splice one relayed RTSP connection both directions."""
    up = socket.create_connection((TARGET_HOST, TARGET_PORT), timeout=10)
    up.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    # each direction on its own thread; closing any side tears down the pair
    t1 = threading.Thread(target=_bridge, args=(client, up), daemon=True)
    t2 = threading.Thread(target=_bridge, args=(up, client), daemon=True)
    t1.start(); t2.start()
    t1.join(); t2.join()
    for s in (client, up):
        try:
            s.close()
        except OSError:
            pass


def main():
    ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind((LISTEN_HOST, LISTEN_PORT))
    ls.listen(16)
    print(f"relay: 127.0.0.1:{LISTEN_PORT} -> {TARGET_HOST}:{TARGET_PORT}", flush=True)
    while True:
        conn, _ = ls.accept()
        threading.Thread(target=handle, args=(conn,), daemon=True).start()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)