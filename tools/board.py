#!/usr/bin/env python3
"""board.py -- talk to the board without lying about what happened.

WHY THIS EXISTS. UM-NATOS-058 §7 and UM-NATOS-059 §7 record seven rounds of work
lost to broken measurement rather than to a broken system, and the report says
plainly that this is now the dominant failure mode. Every one of them came from
an ad-hoc capture script that was rewritten from memory each session and
re-broke the same way:

  - the port open failed and the error was swallowed, so three empty logs were
    read as "the board is silent" while a stale process held COM5
  - a command was typed three seconds after opening the port, into a board that
    was still booting, and was simply lost
  - the USB link dropped mid-capture and a traceback threw away everything
    already collected
  - a flash retry loop matched the wrong success line and reflashed eight times,
    seven of them redundant writes to a part with a finite erase count
  - a fix that was never on the board was reported as not working

None of those were subtle. They recurred because the tool was disposable.

WHAT THIS GUARANTEES, because each one is a failure listed above:

  1. An open failure is LOUD and exits non-zero. Never an empty log.
  2. It PROBES for the prompt before typing, rather than waiting a fixed time.
     Opening the port usually resets the board, and a command sent into one that
     is still booting is lost.
  3. A dropped link ends the capture and KEEPS what was collected, with a
     `[board] link lost` line where it happened.
  4. `flash` succeeds only on esptool's own verification line, and reports the
     attempt count so a silent retry storm cannot hide.
  5. It names the stale process holding the port instead of reporting a
     mysterious PermissionError.
  6. It FINDS the port rather than assuming one, and prints which it chose --
     the board moves between USB sockets and a hardcoded COM5 does not.

Usage:
    python tools/board.py ports
    python tools/board.py flash [--port COM6] [--tries 5]
    python tools/board.py run  "ps" "run meter"  [--wait 12]
    python tools/board.py watch 120
"""

import argparse
import os
import subprocess
import sys
import time

try:
    import serial
    import serial.tools.list_ports as list_ports
except ImportError:
    print("board: pyserial is not installed", file=sys.stderr)
    sys.exit(2)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# What shell_begin() prints. Waiting for this rather than for a fixed delay is
# the difference between a command running and a command vanishing.
BANNER = "nat-os shell"

# esptool's own line. NOT "Hash of data verified", which it prints per segment
# and which a partial flash also prints -- matching that is what made a retry
# loop reflash eight times after succeeding on the first.
FLASH_OK = "flash verified: 3 segments"


def ports():
    return [(p.device, p.description) for p in list_ports.comports()]


def pick_port(explicit):
    """The board moves between USB ports; a hardcoded COM5 does not.

    [step 378] Every command here defaulted to COM5, which was right until the
    board was plugged in somewhere else and then produced "the port doesn't
    exist" for a board sitting on COM6. That is UM-NATOS-059 section 3's census
    defect in another costume: a fact that was true when written, hardened into
    a default, and never rechecked.

    So: an explicit --port always wins. Otherwise, if exactly one serial port
    looks like a USB-serial adapter, use it and SAY SO -- silently picking a
    port would be worse than the constant, because the reader could no longer
    tell which board answered. If there are several, refuse and list them
    rather than guess."""
    if explicit:
        return explicit
    found = ports()
    if not found:
        print("board: no serial ports at all. The board is not connected, or "
              "USB enumeration failed -- check Device Manager for an 'Unknown "
              "USB Device (Device Descriptor Request Failed)'.", file=sys.stderr)
        sys.exit(2)
    likely = [d for d, desc in found
              if "CH340" in desc.upper() or "USB-SERIAL" in desc.upper()
              or "CP210" in desc.upper() or "UART" in desc.upper()]
    if len(likely) == 1:
        print("board: using %s" % likely[0], file=sys.stderr)
        return likely[0]
    if len(found) == 1:
        print("board: using %s" % found[0][0], file=sys.stderr)
        return found[0][0]
    print("board: several ports; say which with --port:", file=sys.stderr)
    for d, desc in found:
        print("   %s  %s" % (d, desc), file=sys.stderr)
    sys.exit(2)


def holder(port):
    """Who has the port open. A PermissionError with no name attached is what
    made three captures look like a silent board."""
    try:
        out = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             "Get-CimInstance Win32_Process | "
             "Where-Object { $_.CommandLine -like '*%s*' } | "
             "Select-Object -ExpandProperty ProcessId" % port],
            capture_output=True, text=True, timeout=20).stdout.split()
        mine = str(os.getpid())
        return [p for p in out if p != mine]
    except Exception:
        return []


def open_port(port):
    try:
        return serial.Serial(port, 115200, timeout=0.2)
    except Exception as e:
        print("board: cannot open %s: %s" % (port, e), file=sys.stderr)
        if not any(d == port for d, _ in ports()):
            print("board: %s is not enumerated. The board is not connected, or "
                  "USB enumeration failed -- check Device Manager for an "
                  "'Unknown USB Device (Device Descriptor Request Failed)'."
                  % port, file=sys.stderr)
        else:
            pids = holder(port)
            if pids:
                print("board: still held by process %s -- stop it first"
                      % ", ".join(pids), file=sys.stderr)
        sys.exit(2)


def pump(s, out, seconds, until=None):
    """Read for `seconds`, or until `until` appears. Returns False if the link
    dropped -- and everything read so far is already in `out`."""
    stop = time.time() + seconds
    while time.time() < stop:
        try:
            d = s.read(4096)
        except Exception as e:
            out.append("\n[board] link lost: %s\n" % e)
            return False
        if d:
            out.append(d.decode("utf-8", "replace"))
            if until and until in "".join(out[-40:]):
                return True
    return True


def await_shell(s, out, limit=45.0):
    """Wait until the shell will actually answer.

    The first version waited for the boot BANNER, on the theory that opening
    the port always resets the board. It does not always -- and on a board
    already up, that theory cost 45 seconds waiting for a line printed minutes
    earlier. The tool written to stop measurement lying had a wrong assumption
    in it on its first run, which is the argument for running it before
    trusting it.

    So it PROBES: send a bare newline and look for the prompt the shell echoes.
    That works whether the board just reset or has been up for an hour, and a
    newline typed into a booting board is lost like any other character, which
    costs nothing."""
    start = time.time()
    while time.time() - start < limit:
        mark = len(out)
        try:
            s.write(b"\r\n")
            s.flush()
        except Exception as e:
            out.append("\n[board] link lost while probing: %s\n" % e)
            return False
        if not pump(s, out, 1.0):
            return False
        fresh = "".join(out[mark:])
        if "> " in fresh or BANNER in fresh:
            out.append("\n[board] shell answered after %.0fs\n"
                       % (time.time() - start))
            return True
    out.append("\n[board] no prompt in %.0fs -- sending anyway, but a command "
               "sent into a board that is not listening is lost\n" % limit)
    return True


def cmd_run(args):
    s = open_port(args.port)
    out = []
    live = await_shell(s, out)
    for c in args.command:
        if not live:
            break
        out.append("\n>>> %s\n" % c)
        try:
            s.write((c + "\r\n").encode())
            s.flush()
        except Exception as e:
            out.append("[board] write failed: %s\n" % e)
            break
        live = pump(s, out, args.wait)
    s.close()
    sys.stdout.buffer.write("".join(out).encode("utf-8", "replace"))
    return 0 if live else 1


def cmd_watch(args):
    s = open_port(args.port)
    out = []
    live = pump(s, out, args.seconds)
    s.close()
    sys.stdout.buffer.write("".join(out).encode("utf-8", "replace"))
    return 0 if live else 1


def cmd_flash(args):
    for attempt in range(1, args.tries + 1):
        r = subprocess.run(
            ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
             "-File", os.path.join(ROOT, "build.ps1"),
             "-Flash", "-Port", args.port],
            capture_output=True, text=True)
        text = (r.stdout or "") + (r.stderr or "")
        if FLASH_OK in text:
            print("board: flashed on attempt %d of %d" % (attempt, args.tries))
            return 0
        why = [l.strip() for l in text.splitlines()
               if "error" in l.lower() or "fatal" in l.lower()]
        print("board: attempt %d failed: %s"
              % (attempt, why[0] if why else "no verification line"))
        # A build error will not fix itself by retrying; only a link will.
        if any("compile failed" in w or "nattest failed" in w for w in why):
            print("board: that is a build failure, not a link failure")
            return 1
        time.sleep(4)
    print("board: gave up after %d attempts" % args.tries, file=sys.stderr)
    return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", default=None,
                    help="serial port; auto-detected when omitted")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("ports")

    f = sub.add_parser("flash")
    f.add_argument("--tries", type=int, default=5)

    r = sub.add_parser("run")
    r.add_argument("command", nargs="+")
    r.add_argument("--wait", type=float, default=12.0)

    w = sub.add_parser("watch")
    w.add_argument("seconds", type=float)

    args = ap.parse_args()
    if args.cmd != "ports":
        args.port = pick_port(args.port)
    if args.cmd == "ports":
        p = ports()
        print("\n".join("%s  %s" % pd for pd in p) if p else "no serial ports")
        return 0 if p else 1
    return {"flash": cmd_flash, "run": cmd_run, "watch": cmd_watch}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
