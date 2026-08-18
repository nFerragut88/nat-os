"""Pull the framebuffer over serial and write it as a PNG.

Every render-vs-transport experiment so far has stalled on the same thing: the
only instrument that can see the picture is a person. fbhash proves two frames
are identical but cannot say whether either is a corridor or noise, and a frozen
camera stably rendering a WRONG scene gives a perfectly constant hash. MISO is
dead, so the panel cannot be read back.

So dump the buffer instead and look at it. RGB565, every second pixel and row,
120x112, scaled up on write so it is legible.

Usage: grab_fb.py COM5 out.png [--open3d] [--freeze]
"""
import struct
import sys
import time
import zlib

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM5"
OUT = sys.argv[2] if len(sys.argv) > 2 else "fb.png"
SCALE = 4

ser = serial.Serial()
ser.port = PORT
ser.baudrate = 115200
ser.timeout = 0.3
ser.dsrdtr = False
ser.rtscts = False
ser.dtr = False
ser.rts = False
ser.open()


def send(cmd, wait):
    ser.reset_input_buffer()
    ser.write((cmd + "\r").encode())
    ser.flush()
    buf = bytearray()
    deadline = time.time() + wait
    while time.time() < deadline:
        buf += ser.read(8192)
    return buf.decode("utf-8", "replace")


if "--open3d" in sys.argv:
    if "--freeze" in sys.argv:
        send("camfreeze", 1.0)
    send("view3d", 1.0)
    time.sleep(2.0)

def send_until(cmd, marker, limit):
    """Read until the terminator appears rather than for a fixed duration.

    A fixed window truncated the dump at row 49 twice, and a truncated dump that
    does not say so is how a partial image gets read as a corrupt one."""
    ser.reset_input_buffer()
    ser.write((cmd + "\r").encode())
    ser.flush()
    buf = bytearray()
    deadline = time.time() + limit
    while time.time() < deadline:
        chunk = ser.read(8192)
        if chunk:
            buf += chunk
            if marker in buf:
                break
    return buf.decode("utf-8", "replace")


text = send_until("fbdump", b"FBEND", 60.0)

if "FBDUMP" not in text:
    print("no FBDUMP header; got:\n" + text[:400])
    ser.close()
    raise SystemExit(1)

body = text.split("FBDUMP", 1)[1]
header, _, rest = body.partition("\n")
w, h = (int(x) for x in header.split()[:2])

# Rows are indexed on the device. Position is READ, never inferred from arrival
# order -- the first version of this script silently dropped 61 malformed rows
# and reassembled the survivors as if they were contiguous, which produces
# convincing garbage out of a perfectly good buffer.
byindex = {}
bad = 0
for line in rest.splitlines():
    line = line.strip()
    if line.startswith("FBEND"):
        break
    if ":" not in line:
        if line:
            bad += 1
        continue
    idx, _, hexpart = line.partition(":")
    if not idx.isdigit() or len(hexpart) != w * 4:
        bad += 1
        continue
    try:
        px = []
        for i in range(w):
            v = int(hexpart[i * 4:(i + 1) * 4], 16)
            px.append((((v >> 11) & 0x1F) * 255 // 31,
                       ((v >> 5) & 0x3F) * 255 // 63,
                       (v & 0x1F) * 255 // 31))
    except ValueError:
        bad += 1
        continue
    byindex[int(idx)] = px

expect = h
missing = [i for i in range(expect) if i not in byindex]
print("rows received %d/%d   malformed %d   missing %s"
      % (len(byindex), expect, bad, missing[:12] + (["..."] if len(missing) > 12 else [])))
if bad or missing:
    print("!! DUMP INCOMPLETE -- do not read anything into the image below")
if not byindex:
    ser.close()
    raise SystemExit(1)

# Missing rows are filled with a flat mid-grey so a gap is obvious as a gap
# rather than passing for image content.
rows = [byindex.get(i, [(128, 128, 128)] * w) for i in range(expect)]

# Minimal PNG writer -- no external imaging library needed.
W, H = w * SCALE, len(rows) * SCALE
raw = bytearray()
for row in rows:
    line = bytearray()
    for (r, g, b) in row:
        line += bytes((r, g, b)) * SCALE
    for _ in range(SCALE):
        raw += b"\x00" + line


def chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


png = (b"\x89PNG\r\n\x1a\n"
       + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
       + chunk(b"IEND", b""))

with open(OUT, "wb") as fh:
    fh.write(png)
print("wrote %s  (%dx%d)" % (OUT, W, H))

# A few statistics, since they are free and sometimes say it faster than the eye
flat = [p for row in rows for p in row]
uniq = len(set(flat))
print("distinct colours: %d of %d pixels" % (uniq, len(flat)))
ser.close()
