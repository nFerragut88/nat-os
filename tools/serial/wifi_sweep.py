"""Sweep CCA and EDCA while a second board listens.

UM-NATOS-034 narrowed WiFi transmit to: the MAC retires the frame and nothing
demodulable reaches the air. Disassembling hal_mac.o found two registers the
vendor's init writes that this driver never has -- the CCA mode and the EDCA
arbitration parameters -- and both read back as ZERO on a live board.

A MAC that believes the channel is permanently busy would behave exactly as
observed: accept the frame, arm the queue, retire the descriptor, key nothing.

So: leave one board beaconing continuously, leave the other listening, and
change ONE register at a time. The receiver's raw frame count is the answer.
Reverting two changes together destroys the information about which one
mattered, and so does applying two.

Every value is a guess about MEANING -- the bit positions are solid, the
semantics are 802.11 convention -- which is why this is a sweep and not a
single write.

Usage: wifi_sweep.py [tx_port] [rx_port] [channel]
"""
import re
import sys
import time

import serial

TX_PORT = sys.argv[1] if len(sys.argv) > 1 else "COM5"
RX_PORT = sys.argv[2] if len(sys.argv) > 2 else "COM6"
CHANNEL = sys.argv[3] if len(sys.argv) > 3 else "6"


def open_board(port, reset=True):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dsrdtr = s.rtscts = False
    s.open()
    if reset:
        s.dtr = False
        s.rts = True
        time.sleep(0.15)
        s.rts = False
    s.reset_input_buffer()
    return s


def cmd(s, c, wait=1.5):
    s.reset_input_buffer()
    s.write((c + "\n").encode())
    s.flush()
    end = time.time() + wait
    buf = b""
    while time.time() < end:
        buf += s.read(4096)
    return buf.decode("utf-8", "replace")


def quiet(t):
    return [l.rstrip() for l in t.splitlines()
            if l.strip() not in ("", ">")
            and "switches r/a/b=" not in l
            and not l.lstrip().startswith("lock owner=")]


def frames(s):
    for l in quiet(cmd(s, "scan", 2.0)):
        m = re.search(r"frames=(\d+)", l)
        if m:
            return int(m.group(1))
    return -1


def heard(s, mac):
    """Did the receiver see a beacon FROM THIS MAC?

    The first version of this keyed on the raw frame-count delta, which was
    wrong and flagged every step as a hit: a real access point in the room
    delivers ~1.5 frames/sec, so any 8-second window shows ~12 new frames
    whatever the transmitter does. The instrument was measuring the
    neighbourhood.

    A source MAC cannot be manufactured by anything else in the room, which is
    the whole reason the two-board rig exists. Key on that."""
    for l in quiet(cmd(s, "scan", 2.5)):
        if mac in l:
            return True
    return False


def tx_sent(s):
    for l in quiet(cmd(s, "txstat", 2.0)):
        m = re.search(r"hardware=(\d+)", l)
        if m:
            return int(m.group(1))
    return -1


tx = open_board(TX_PORT)
rx = open_board(RX_PORT)
time.sleep(12.0)

print("== setting both boards up ==", flush=True)
for s_ in (tx, rx):
    cmd(s_, "fb off", 2.0)
    for c in ("phyinit", "macinit", f"chan {CHANNEL}", "macrx"):
        cmd(s_, c, 3.0)

for l in quiet(cmd(tx, "wifitx", 2.0)):
    print("   TX|", l)

print("\n== transmitter beaconing continuously ==", flush=True)
for l in quiet(cmd(tx, "beacon sweep", 2.5)):
    print("   TX|", l)

# (register, value) one at a time. None = baseline, change nothing.
# CCA alone. AIFS/CW are left at whatever wifimac_tx sets, because sweeping
# them to 7/31 made the MAC stall -- 947 handed over, 247 completed, 699 forced
# -- which is a behaviour change that confounds the thing being measured.
STEPS = [
    (None, None),
    ("cca", 1), ("cca", 2), ("cca", 3), ("cca", 0),
]

print(f"\n{'step':<12}{'rx frames':>12}{'delta':>8}{'tx sent':>10}", flush=True)
print("-" * 44, flush=True)

prev_rx = frames(rx)
for reg, val in STEPS:
    if reg:
        cmd(tx, f"wifitx {reg} {val}", 1.5)
        label = f"{reg} {val}"
    else:
        label = "baseline"
    time.sleep(8.0)
    f = frames(rx)
    t = tx_sent(tx)
    d = f - prev_rx
    flag = "   <-- RX MOVED" if d > 6 else ""
    print(f"{label:<12}{f:>12}{d:>8}{t:>10}{flag}", flush=True)
    prev_rx = f

print("\n== what the receiver actually heard ==", flush=True)
for l in quiet(cmd(rx, "scan", 3.0)):
    print("   RX|", l)
for l in quiet(cmd(tx, "macaddr", 2.0)):
    print("   TX|", l)

tx.close()
rx.close()
