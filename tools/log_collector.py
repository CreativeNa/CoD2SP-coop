"""log_collector.py - receive the log lines the other player's bridge forwards over UDP.

Run it on the machine that should collect the logs (usually the host):

    python tools\\log_collector.py [--port 27300] [--tcp-port 27301] [--dir logs]

Two channels from the other player's machine:
  * live lines (UDP): the bridge forwards every log line when the other player's coop_peer.txt has a third
    number, <this machine's ip> <bridge port> 27300 (CoD2Coop.exe 'connect' writes it by default).
    Each line is printed and appended to <dir>\\peer-<sender ip>-<date>.log, with this machine's receive
    time in front (the sender's own time is inside the line; the two clocks may differ). Plain UDP: lines
    can be lost or arrive out of order.
  * (audit 2026-11-22) the collector listens on THIS PC only by default. To collect from another PC give the address of this PC in your VPN or LAN with
    --bind <that address>; there is no password, so do that only on a network you trust and only while collecting. The folder is capped (--max-dir-mb).
  * bundles (TCP): 'CoD2Coop.exe logs --send <this ip>' uploads a zip with the full coop5.log and
    coop_net.csv; it is saved as <dir>\\<sender ip>-<date>-<name>.zip.
"""

import argparse
import datetime
import os
import re
import socket
import sys
import threading

MAX_UPLOAD = 50 * 1024 * 1024        # one zip bundle
MAX_LIVE_FILE = 20 * 1024 * 1024     # one live log file of one sender and day


def dir_size(path):
    total = 0
    for name in os.listdir(path):
        full = os.path.join(path, name)
        if os.path.isfile(full):
            total += os.path.getsize(full)
    return total


def tcp_server(host, port, outdir, max_dir):
    """Receive log bundles pushed by `CoD2Coop.exe logs --send`: "CODLOG1\\n<name>\\n<size>\\n" + bytes."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(4)

    def handle(conn, ip):
        try:
            conn.settimeout(15)
            f = conn.makefile("rb")
            if f.readline().strip() != b"CODLOG1":
                return
            name = f.readline().decode("utf-8", "replace").strip()
            size = int(f.readline().strip())
            if not 0 < size <= MAX_UPLOAD:
                return
            if dir_size(outdir) + size > max_dir:
                conn.sendall(b"FULL\n")
                print(f"{ip} | upload refused: the folder is at its size cap")
                return
            safe = re.sub(r"[^A-Za-z0-9._-]", "_", os.path.basename(name))[:80] or "logs.zip"
            path = os.path.join(outdir, f"{ip}-{datetime.datetime.now():%Y%m%d-%H%M%S}-{safe}")
            got = 0
            with open(path, "wb") as out:
                while got < size:
                    chunk = f.read(min(65536, size - got))
                    if not chunk:
                        break
                    out.write(chunk)
                    got += len(chunk)
            ok = got == size
            conn.sendall(b"OK\n" if ok else b"SHORT\n")
            print(f"{ip} | log bundle {'saved' if ok else 'INCOMPLETE'}: {path} ({got}/{size} bytes)")
        except Exception as e:  # a bad sender must not stop the collector
            print(f"{ip} | upload failed: {e}")
        finally:
            conn.close()

    while True:
        conn, (ip, _p) = srv.accept()
        threading.Thread(target=handle, args=(conn, ip), daemon=True).start()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=27300, help="UDP port for live log lines")
    ap.add_argument("--tcp-port", type=int, default=27301, help="TCP port for log bundles (zip)")
    ap.add_argument("--dir", default="logs")
    ap.add_argument("--bind", default="127.0.0.1", help="the address to listen on; default: this PC only (see the top of this file)")
    ap.add_argument("--max-dir-mb", type=int, default=500, help="size cap of the whole log folder")
    args = ap.parse_args()
    max_dir = args.max_dir_mb * 1024 * 1024

    os.makedirs(args.dir, exist_ok=True)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.bind, args.port))
    threading.Thread(target=tcp_server, args=(args.bind, args.tcp_port, args.dir, max_dir), daemon=True).start()
    print(f"collecting on udp {args.bind}:{args.port} (live lines) and tcp {args.tcp_port} (zip bundles) -> {args.dir}")
    print("lines go to peer-<ip>-<date>.log; if nothing arrives from the other PC: allow python.exe in the")
    print("Windows firewall (private network). Ctrl+C to stop.")

    files = {}
    counts = {}
    try:
        while True:
            data, (ip, _port) = sock.recvfrom(2048)
            now = datetime.datetime.now()
            text = data.decode("utf-8", "replace").rstrip("\r\n")
            name = os.path.join(args.dir, f"peer-{ip}-{now:%Y%m%d}.log")
            f = files.get(name)
            if f is None:
                f = open(name, "a", encoding="utf-8", buffering=1)
                files[name] = f
            # a log line is one line of plain text: control characters of a packet cannot start a forged line
            text = "".join(ch if (32 <= ord(ch) < 127) or (ord(ch) > 160 and ch.isprintable()) else "?" for ch in text)
            line = f"{now:%H:%M:%S.%f}"[:-3] + " " + text
            if f.tell() + len(line) > MAX_LIVE_FILE:
                continue
            f.write(line + "\n")
            counts[ip] = counts.get(ip, 0) + 1
            print(f"{ip} | {line}")
    except KeyboardInterrupt:
        print()
        for ip, n in counts.items():
            print(f"{ip}: {n} lines")
    finally:
        for f in files.values():
            f.close()


if __name__ == "__main__":
    sys.exit(main())
