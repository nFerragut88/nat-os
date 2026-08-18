"""Reset the board, let it boot, send a shell command, capture the reply.

Usage: send_cmd.py COM5 <settle_s> <command> <capture_s>

Same reset discipline as capture.py: the port is opened first, then the
auto-reset lines are pulsed, so the boot banner is never missed.
"""
import sys
import time

import serial

PORT = sys.argv[1]
SETTLE = float(sys.argv[2])
COMMAND = sys.argv[3]
CAPTURE = float(sys.argv[4])

ser = serial.Serial(PORT, 115200, timeout=0.2)

ser.dtr = False   # GPIO0 high -> normal boot
ser.rts = True    # EN low     -> held in reset
time.sleep(0.15)
ser.rts = False   # release; chip boots
ser.reset_input_buffer()

# Let the self-tests finish so their output is not confused with the reply.
deadline = time.time() + SETTLE
while time.time() < deadline:
    ser.read(4096)

ser.write((COMMAND + "\r").encode())
ser.flush()
print("--- sent: %s ---" % COMMAND)

buf = bytearray()
deadline = time.time() + CAPTURE
while time.time() < deadline:
    buf += ser.read(4096)

ser.close()
print(buf.decode("utf-8", "replace"))
