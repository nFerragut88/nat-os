"""Long unattended pressure capture, for the phantom-touch question.

Two runs logged spurious presses at ~374 s and ~390 s with nobody in the room,
and one of them launched a program. The leading hypothesis is thermal drift: a
baseline that walks with board temperature would cross a fixed threshold at a
repeatable time, and six minutes is about right for a small board to settle.

This runs past both reported times and records the whole approach, not just the
event. What settles it is the BASELINE:

  - `min` climbing steadily toward `thr`   -> thermal drift, confirmed before
                                              any phantom press occurs
  - `min` flat and `max` spiking from
    nowhere                                -> an electrical event; drift is dead

Touch routing is suppressed with `touchoff` so a phantom press cannot launch
anything mid-run, while sampling and telemetry stay live. The presses remain
visible in the counters; they simply cannot act.

Usage: ztrack_run.py COM5 [minutes] [extra,commands] [outfile]

The third argument is what makes a second run worth taking. The first run was
idle and saw nothing; the runs that originally logged phantom presses were under
continuous display load, so `view3d` is the one variable to change.
"""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM5"
MINUTES = float(sys.argv[2]) if len(sys.argv) > 2 else 13.0
EXTRA = [c for c in (sys.argv[3].split(",") if len(sys.argv) > 3 else []) if c]
OUT = sys.argv[4] if len(sys.argv) > 4 else "ztrack.log"

ser = serial.Serial()
ser.port = PORT
ser.baudrate = 115200
ser.timeout = 0.2
ser.dsrdtr = False
ser.rtscts = False
ser.open()

# From a cold reset, because the hypothesis is about elapsed time since power-on.
ser.dtr = False
ser.rts = True
time.sleep(0.15)
ser.rts = False
ser.reset_input_buffer()

print(f"== cold boot; capturing {MINUTES:.0f} min to {OUT} ==", flush=True)

# Let the boot self-tests finish, then arm.
time.sleep(12.0)
ser.reset_input_buffer()
for cmd in ["touchoff", "ztrack"] + EXTRA:
    print(f"  $ {cmd}", flush=True)
    ser.write((cmd + "\n").encode())
    ser.flush()
    time.sleep(1.0)

start = time.time()
end = start + MINUTES * 60.0
buf = b""
n_ztrk = 0
with open(OUT, "w", encoding="utf-8") as fh:
    while time.time() < end:
        buf += ser.read(4096)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode("utf-8", "replace").strip()
            if not text:
                continue
            # Keep the ZTRK lines and anything that looks like a touch event
            # reaching the log; drop the 2 kB status line.
            if text.startswith("ZTRK"):
                n_ztrk += 1
                fh.write(f"{time.time()-start:8.1f} {text}\n")
                fh.flush()
                if n_ztrk % 30 == 0:
                    print(f"  {time.time()-start:6.0f}s  {text}", flush=True)
            elif "touch s/e=" in text:
                # Pull just the touch fields out of the status line.
                i = text.find("touch s/e=")
                fh.write(f"{time.time()-start:8.1f} STATUS {text[i:i+120]}\n")
                fh.flush()

print(f"\ncaptured {n_ztrk} ZTRK windows -> {OUT}", flush=True)
ser.close()
