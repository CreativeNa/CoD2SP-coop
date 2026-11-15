"""log_collector.py - receive the log lines the other player's bridge forwards over UDP.

Run it on the machine that should collect the logs (usually the host):

    python tools\\log_collector.py [--port 27300] [--tcp-port 27301] [--dir logs]

Two channels from the other player's machine:
  * live lines (UDP): the bridge forwards every log line when the other player's coop_peer.txt has a third
    number, <this machine's ip> <bridge port> 27300 (CoD2Coop.exe 'connect' writes it by default).
    Each line is printed and appended to <dir>\\peer-<sender ip>-<date>.log, with this machine's receive
    time in front (the sender's own time is inside the line; the two clocks may differ). Plain UDP: lines
    can be lost or arrive out of order.
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


def tcp_server(host, port, outdir):
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
            if not 0 < size <= 200 * 1024 * 1024:
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
    ap.add_argument("--bind", default="0.0.0.0")
    args = ap.parse_args()

    os.makedirs(args.dir, exist_ok=True)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.bind, args.port))
    threading.Thread(target=tcp_server, args=(args.bind, args.tcp_port, args.dir), daemon=True).start()
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
            line = f"{now:%H:%M:%S.%f}"[:-3] + " " + text
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
