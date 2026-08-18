"""Send a shell command to a RUNNING board, without resetting it.

send_cmd.py pulses the auto-reset lines, which is correct for boot output and
destructive here: the state being asked about is exactly what a reset clears.
This one never writes .dtr or .rts.

Usage: send_live.py COM5 <command> <capture_s>
"""
import sys
import time

import serial

PORT = sys.argv[1]
COMMAND = sys.argv[2]
CAPTURE = float(sys.argv[3]) if len(sys.argv) > 3 else 4.0

ser = serial.Serial()
ser.port = PORT
ser.baudrate = 115200
ser.timeout = 0.2
ser.dsrdtr = False
ser.rtscts = False
# Deassert BEFORE open(). On Windows the driver asserts DTR on open by default,
# which pulls EN low and resets the board - so a tool built to observe a running
# system destroys the state it came to read. This project learned that once
# already (UM-CYDOS-017 section 5) and both of these scripts repeated it.
ser.dtr = False
ser.rts = False
ser.open()

ser.reset_input_buffer()
ser.write((COMMAND + "\r").encode())
ser.flush()

buf = bytearray()
deadline = time.time() + CAPTURE
while time.time() < deadline:
    buf += ser.read(4096)
ser.close()

print(buf.decode("utf-8", "replace"))
