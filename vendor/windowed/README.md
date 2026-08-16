# vendor/windowed

Objects compiled `-mabi=windowed` and linked into the otherwise `-mabi=call0`
kernel.

Everything in `kernel/` is call0 and will stay that way. This directory is for
code that is not — which is the ABI **every precompiled Espressif library uses**,
and therefore the ABI any future WiFi work has to speak.

`wintest_windowed.c` is not vendor code. It is a stand-in: compiled by the same
compiler, with the same ABI, into the same image, and called through the same
bridge a real blob would use. It recurses deep enough to force register-window
overflow, keeps locals live across the recursive call so the compiler must use
callee-saved registers, and returns a checksum over every level — so a broken
window handler yields a **wrong number** rather than merely not crashing.

Two things this arrangement establishes:

- mixed-ABI linking works (the linker warns `incompatible Xtensa configuration
  (ABI does not match)`, which is expected and is the whole point)
- `kernel/window.S`'s handlers are correct against compiler-generated windowed
  code, not just against hand-written assembly

What it does **not** establish: that any real Espressif blob will run. Those need
an environment — heap, RTC and clock state, calibration data from NVS, and IDF
functions they call out to — none of which nat-os provides yet.
