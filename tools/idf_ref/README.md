# A known-good transmitter

The only thing in this repository built with somebody else's build system, and
it is here for one reason.

nat-os's WiFi transmit does not reach the air (UM-NATOS-034). Every measurement
of that was made with a receiver whose only proven reference was a router
across the house — so "our transmitter is silent" and "our receiver is deaf in
some way we have not noticed" were never fully separated.

This is Espressif's own stack, unmodified, running as a SoftAP: known MAC,
known SSID, known channel, 30 cm from the board under test.

```
REF-AP ssid=natref channel=6 mac=5c:01:3b:51:2b:41
```

## Result

nat-os's receiver hears it. 170 beacons in 15 seconds, MAC and SSID matching
exactly:

```
frames=235  recycled=235  networks=2
5c:01:3b:51:2b:41  x170  "natref"
44:25:38:19:0d:1a  x53   "TC7NR"
```

**The receiver is fine.** A transmitter 30 cm away beaconing at ~10 Hz is heard
170 times; nat-os's own transmitter, at the same rate and the same distance, is
heard zero times. Those two runs are not simultaneous — a radio cannot hear
itself, so the comparison is across runs rather than within one — but the
receiver, channel, room and session are the same.

## Build and flash

```
cd tools/idf_ref
pio run
pio run --target upload --upload-port COM6
```

**COM6 only.** It must never be flashed to the board running nat-os.

## What it is for next

Phase B: a register-level diff. Both firmwares dump the same ranges — MAC
0x3FF73xxx, RTC 0x3FF48xxx, DPORT 0x3FF000xx — one taken while this stack is
successfully transmitting, one while nat-os is failing to. The point of a
differential is that you do not need to know what a register means to notice
that it differs.

Both stacks call the same PHY blob, so whatever the blob does internally is
identical. The difference has to be in the surrounding code, which is exactly
the layer visible from both sides.
