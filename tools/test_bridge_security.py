#!/usr/bin/env python3
"""test_bridge_security.py - checks of the audit fixes F1, F3, F4, F12, F13 against a RUNNING game that is in HOST mode (CO-OP screen -> HOST), on THIS PC.

    python tools\\test_bridge_security.py [game folder]

It plays two "guests" on this PC: A = 127.0.0.1 (needs a build for local tests, or coop_testmode.txt) and B = another address of this PC (a LAN / VPN
address). It sends UDP packets to 27201 and one TCP connection to 27203 and reads what coop5.log says. It sends only packets that the bridge must REFUSE or
that are harmless (a pose packet, "anim 3"); it never sends a saved game that would be accepted. Run it with no mission running. It creates and removes
the file coop_testmode.txt in the game folder.
"""
import os, re, socket, sys, time

game = sys.argv[1] if len(sys.argv) > 1 else r"C:\Program Files (x86)\Steam\steamapps\common\Call of Duty 2"
log = os.path.join(game, "coop5.log")
testfile = os.path.join(game, "coop_testmode.txt")
fails = 0


def tail_from(offset):
    with open(log, "rb") as f:
        f.seek(offset)
        return f.read().decode("latin1")


def size():
    return os.path.getsize(log)


def check(name, cond, detail=""):
    global fails
    print(("PASS  " if cond else "FAIL  ") + name + (("  [" + detail + "]") if detail else ""))
    if not cond:
        fails += 1


def lan_address():
    for ai in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
        ip = ai[4][0]
        if not ip.startswith("127.") and not ip.startswith("169.254."):
            return ip
    return None


def send_from(src, data, dst=("127.0.0.1", 27201)):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind((src, 0))
    s.sendto(data, dst)
    s.close()


if os.path.exists(testfile):
    os.remove(testfile)
text0 = open(log, "rb").read().decode("latin1")
if "HOST (listen 27201" not in text0.split("=== cod2net5")[-1]:
    print("the game is not in HOST mode (CO-OP screen -> HOST); nothing to test")
    sys.exit(2)
B = lan_address()
print("address B of this PC:", B)
if not B:
    print("no second address on this PC: F1 cannot be tested here")
    sys.exit(2)
dstB = (B, 27201)

# ---- F12: without the test file a test command from this PC is refused
o = size(); send_from("127.0.0.1", b"anim 3"); time.sleep(0.8); t = tail_from(o)
check("F12 a test command is refused without coop_testmode.txt", "ignored a test command" in t and "anim command" not in t, t.strip()[-90:])
open(testfile, "w").write("test\n")
o = size(); send_from("127.0.0.1", b"anim 3"); time.sleep(0.8); t = tail_from(o)
check("F12 the same command works with coop_testmode.txt", "anim command 3" in t, t.strip()[-90:])

# ---- F4 + F1: guest A claims the slot; its first packet carries a line feed and a token
TA = 4000000000123456789
o = size()
send_from("127.0.0.1", b"0 0 0 0 1 1 0 0 18 0 100\nFORGED line " + (" T%d" % TA).encode())
time.sleep(1.0)
t = tail_from(o)
check("F1 the first guest is learned", "peer learned: 127.0.0.1" in t, t.strip()[:100])
check("F4 a line feed in a packet does not make a second log line", not re.search(r"(?m)^FORGED", t) and "pkt 1: " in t, "")
check("F1 the token is not written to the log", str(TA) not in t and re.search(r"T\*+", t) is not None, "")

# ---- F1: the slot cannot be taken while the guest is silent (> 5 s) by another address
time.sleep(6.5)
o = size(); send_from(B, b"0 0 0 0 2 2 0 0 18 0 100", dstB); time.sleep(0.8); t = tail_from(o)
check("F1 another address WITHOUT a token does not get the slot", "peer switched" not in t and "ignored a packet" in t, t.strip()[-100:])
o = size(); send_from(B, b"0 0 0 0 3 3 0 0 18 0 100 T12345", dstB); time.sleep(0.8); t = tail_from(o)
check("F1 another address with a WRONG token does not get the slot", "peer switched" not in t, t.strip()[-100:])   # (the log says "ignored a packet" once per 30 s per address)
o = size(); send_from(B, b"H 1 T777", dstB); time.sleep(0.8); t = tail_from(o)
check("F1 a hello with a wrong token does not get the slot", "peer switched" not in t and "peer learned" not in t, t.strip()[-100:])
o = size(); send_from(B, (b"0 0 0 0 4 4 0 0 18 0 100 T%d" % TA), dstB); time.sleep(0.8); t = tail_from(o)
check("F1 the same guest from a new address WITH its token is accepted", "peer switched" in t, t.strip()[-100:])

# ---- F13 + F3: checkpoints (sent from this PC; accepted from loopback only with coop_testmode.txt). Only refusals are tested.
def send_ckpt(name, payload):
    s = socket.create_connection(("127.0.0.1", 27203), timeout=5)
    s.sendall(("COOPSAVE %s %d\n" % (name, len(payload))).encode() + payload)
    time.sleep(0.8)
    s.close()


o = size(); send_ckpt("CON", os.urandom(1200000)); t = tail_from(o)
check("F13 a Windows device name is refused", "is a Windows device name" in t, t.strip()[-100:])
o = size(); send_ckpt("moscow-9", os.urandom(1500000)); t = tail_from(o)
check("F3 random bytes are not a saved game", "does not look like a saved game" in t, t.strip()[-100:])
o = size(); send_ckpt("moscow-9", b"\x99\x00\x00\x00" + os.urandom(1500000)); t = tail_from(o)
check("F3 a wrong header is refused", "does not look like a saved game" in t, t.strip()[-100:])

os.remove(testfile)
print("\nRESULT:", "all checks passed" if not fails else "%d FAILED" % fails)
sys.exit(1 if fails else 0)
