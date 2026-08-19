"""Two boards, one channel: does a frame ever leave this radio?

---- why this test and not the previous ones -----------------------------

Every transmit attempt in the record asked an ACCESS POINT to react. 178 of 178
frames reported complete, `forced=0`, and twenty probe requests drew zero
responses from two APs that are received continuously. That evidence is stuck,
because "an AP did not answer" is equally consistent with two very different
faults:

    nothing left the antenna at all
    something left but was malformed

An AP that ignores a malformed frame looks exactly like a radio that never
transmitted, and no amount of repeating the experiment separates them.

Two nat-os boards separate them. The receiver is ours, it runs in promiscuous
mode, and `scan` lists every distinct beacon source it has heard with the
source MAC and SSID attached. A beacon from board A carries A's factory MAC,
which board B cannot manufacture and no neighbour's router can forge. Same
principle as the probe-response test in UM-NATOS-028: a radio cannot put
someone else's identity into your receiver.

---- the control, which is the part that makes a negative meaningful --------

Board B is measured BEFORE board A transmits. If B hears nothing in that
window it has proved nothing about A, only about itself -- a negative result is
only informative if the experiment demonstrably ran (Rule 6). So the run is:

    1. arm B, listen, record what is already on the air     <- control
    2. start A beaconing
    3. listen again, and look for A's MAC specifically

Three outcomes, pointing in completely different directions:

    A's MAC appears in B's scan   -> TRANSMIT WORKS. The "no AP response"
                                     history was a framing problem, not RF.
    B hears real APs, never A     -> B is provably live and A is genuinely
                                     silent. That is the RF / power-domain
                                     lead, now confirmed rather than inferred.
    B hears nothing at all        -> B is not set up. A's silence means
                                     nothing yet; fix B first.

Nothing here touches the link. No new symbols, no libphy objects pulled in, no
risk to the working receiver.

Usage: wifi_link.py [tx_port] [rx_port] [channel]
"""
import re
import sys
import time

import serial

TX_PORT = sys.argv[1] if len(sys.argv) > 1 else "COM5"
RX_PORT = sys.argv[2] if len(sys.argv) > 2 else "COM6"
CHANNEL = sys.argv[3] if len(sys.argv) > 3 else "6"
SSID    = "natlink"


def open_board(port):
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.dsrdtr = False
    s.rtscts = False
    s.open()
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


def quiet(text):
    """Drop the 2 kB periodic status line; keep command replies."""
    out = []
    for ln in text.splitlines():
        if "switches r/a/b=" in ln or ln.lstrip().startswith("lock owner="):
            continue
        if ln.strip() in ("", ">"):
            continue
        out.append(ln.rstrip())
    return out


def mac_of(s):
    for ln in quiet(cmd(s, "macaddr", 2.0)):
        m = re.search(r"factory mac ([0-9a-f:]{17})", ln)
        if m:
            return m.group(1)
    return None


def networks(s):
    """(bssid, seen, ssid) for every beacon source heard so far."""
    out = []
    for ln in quiet(cmd(s, "scan", 2.5)):
        m = re.match(r"\s*([0-9a-f]{2}(?::[0-9a-f]{2}){5})\s+x(\d+)\s+\"(.*)\"", ln)
        if m:
            out.append((m.group(1), int(m.group(2)), m.group(3)))
    return out


print("== opening both boards ==", flush=True)
tx = open_board(TX_PORT)
rx = open_board(RX_PORT)
time.sleep(12.0)          # let both finish their boot self-tests

tx_mac = mac_of(tx)
rx_mac = mac_of(rx)
print(f"   TX {TX_PORT}  {tx_mac}")
print(f"   RX {RX_PORT}  {rx_mac}")
if not tx_mac or not rx_mac:
    print("!! could not read a MAC; stopping rather than guessing")
    raise SystemExit(1)
if tx_mac == rx_mac:
    print("!! both boards report the same MAC -- the signature would be useless")
    raise SystemExit(1)

# The 3D view framebuffer is 80,640 bytes out of a ~110 kB heap. MEASURED: with
# it allocated, heap free is 832 bytes and `macrx` fails with "out of DRAM for
# rx buffers"; with it released, 108,352 free and the receiver arms. On this
# board the renderer and the radio cannot coexist -- a fact worth knowing before
# anyone designs a node that wants both. A headless relay never allocates it.
print("")
print("== freeing the 3D framebuffer on both boards ==", flush=True)
for _name, _s in (("A", tx), ("B", rx)):
    for ln in quiet(cmd(_s, "fb off", 2.0)):
        print(f"   {_name}|", ln)

print("\n== bringing up the receiver (board B) ==", flush=True)
for c in ("phyinit", "macinit", f"chan {CHANNEL}", "macrx"):
    for ln in quiet(cmd(rx, c, 3.0)):
        print("   B|", ln)

print(f"\n== CONTROL: what is already on channel {CHANNEL}, with A silent ==",
      flush=True)
time.sleep(10.0)
before = networks(rx)
for b, n, s in before:
    print(f"   B| {b}  x{n}  \"{s}\"")
if not before:
    print("   B| (nothing heard yet)")

print("\n== bringing up the transmitter (board A) and beaconing ==", flush=True)
for c in ("phyinit", "macinit", f"chan {CHANNEL}", "macrx", f"beacon {SSID}"):
    for ln in quiet(cmd(tx, c, 3.0)):
        print("   A|", ln)

print("\n== listening for A ==", flush=True)
time.sleep(15.0)
after = networks(rx)
for b, n, s in after:
    mark = "  <-- BOARD A" if b == tx_mac else ""
    print(f"   B| {b}  x{n}  \"{s}\"{mark}")

for ln in quiet(cmd(tx, "txstat", 2.0)):
    print("   A|", ln)

print("\n" + "=" * 66)
heard_a = any(b == tx_mac for b, _, _ in after)
if heard_a:
    print("RESULT: board B heard board A's MAC.")
    print("        A FRAME LEFT THE ANTENNA AND WAS RECEIVED.")
    print("        Transmit works; the AP silence was a framing problem.")
elif after or before:
    print("RESULT: board B is provably live -- it heard other networks --")
    print("        and never heard board A.")
    print("        Transmit is genuinely silent. RF / power-domain lead.")
else:
    print("RESULT: board B heard NOTHING, not even a real access point.")
    print("        The receiver is not proven. This says nothing about A yet.")
print("=" * 66)

tx.close()
rx.close()
