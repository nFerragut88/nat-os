---
description: Build and flash nat-os to ESP32.
agent: os-dev
---

Build and flash nat-os to the connected ESP32:

$ARGUMENTS

## Instructions

1. Run `.\build.ps1 -Flash` with any arguments provided (e.g., `-Port COM5`, `-Board cyd`).
2. If the build or flash fails, diagnose and fix the issue.
3. If `-Monitor` was included, attach serial monitor at 115200 baud.
4. Report success/failure.
