"""Measure the CPU frequency against the HOST's clock.

UM-NATOS-036 §10.1. Written because the fix for that report was verified by
reading SOC_CLK_SEL and printing "80 MHz" -- which is reading a register, not
measuring a frequency, and is the same class of evidence that let the original
defect through.

Everything in nat-os that reports time is derived from CCOUNT, so a clock fault
is invisible to all of it at once -- that is what UM-NATOS-036 was about. This
uses a reference the board cannot influence: the PC.

The scheduler tick is programmed at TICK_INTERVAL_CYCLES = 800000 CPU cycles,
and the reporter prints a monotonically increasing `t=` tick count. So

    ticks_per_second * 800000 = CPU Hz

80 MHz -> 100 ticks/s.  40 MHz -> 50 ticks/s.  Nothing in that chain reads a
clock register.
"""
import re, sys, time, serial
port, label = sys.argv[1], sys.argv[2]
s = serial.Serial(); s.port=port; s.baudrate=115200; s.timeout=0.2
s.dsrdtr=s.rtscts=False; s.open()
s.dtr=False; s.rts=True; time.sleep(0.15); s.rts=False
s.reset_input_buffer()
time.sleep(16)                       # past boot and the self-tests
s.reset_input_buffer()

samples, buf, end = [], "", time.time()+25
while time.time() < end:
    chunk = s.read(4096).decode("utf-8","replace")
    if chunk:
        now = time.time()
        buf += chunk
        for m in re.finditer(r"\bt=(\d+)\s", buf):
            samples.append((now, int(m.group(1))))
        buf = buf[-200:]
s.close()

# de-duplicate by tick value, keep first host-time each was seen
seen, pts = set(), []
for t_host, tick in samples:
    if tick not in seen:
        seen.add(tick); pts.append((t_host, tick))
pts.sort(key=lambda p: p[1])
if len(pts) < 3:
    print(f"{label}: only {len(pts)} tick samples -- inconclusive"); raise SystemExit(1)

dt_host = pts[-1][0] - pts[0][0]
dticks  = pts[-1][1] - pts[0][1]
rate    = dticks / dt_host
mhz     = rate * 800000 / 1e6
print(f"  {label}")
print(f"     {dticks} ticks over {dt_host:.2f} s host  ->  {rate:.1f} ticks/s")
print(f"     CPU = {mhz:.1f} MHz   (expect 80.0; a 40 MHz board reads ~40)")
