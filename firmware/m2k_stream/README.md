# M2K-profile streaming relay

This ESP32-S3 sketch adds immediate relative movement and persistent button
states for live left/right dragging. It also retains the older preloaded batch
commands. It uses Arduino-ESP32 3.3.8, USB-OTG mode, UART0 RX44/TX43 at 115200,
and GPIO4 as the stop input.

The original M2K build source was not present on this machine. This sketch
extends the locally available v2 relay implementation, preserving the device,
configuration and report descriptor bytes recovered from the board's full flash
backup. The manufacturer, product, release and serial match its IDENTITY reply.
This preserves the existing reported USB profile, not all original behavior.

Original flash: `.tools/m2k-original-20261012.bin` (16,777,216 bytes).
SHA256: `9cd63ec5584b3ce5e12f022a4a35782e9fc22066539968cc934a1362912a0e89`.
An additional copy lives in the parent project's `runs/hardware_backups/`.
Keep these local backups; they are intentionally excluded from Git.

## Protocol

Use the existing `replay.Relay` and CRC framing utilities. HELLO reports v3,
with the original batch capacity, timing and watchdog fields. IDENTITY reports
the preserved M2K profile. Commands:

- `CAPS` returns `OK CAPS STREAM=1 MAX_AXIS=32767 WATCHDOG_MS=250`.
- `PACE` returns `OK PACE HZ=1000 SOURCE=USB` on the updated firmware. The USB
  producer task primes each next report after completion; the unchanged 1 ms
  endpoint interval clocks delivery. It sends zero movement with held buttons
  between input updates, so stale relative displacement is never repeated.
- `STREAM` releases prior state and starts a session with sequence number zero.
  Requires a ready USB host and an unpressed stop switch.
- `M sequence dx dy buttons wheel` supplies the next USB-slot report and returns
  `OK M sequence` only after transfer completion. Sequence numbers increase
  modulo 2^32. Axes are -32767..32767, buttons 0..7, wheel -127..127.
  Button state persists across reports, enabling holds and dragging.
- `CIRCLE radius period_slots turns buttons` generates circles locally at one
  sample per USB report. Radius is 1..1000 HID counts, period is 500..10000
  reports (nominal milliseconds), turns is 1..20, and total is <=60000 reports.
  Buttons is 0 for cursor movement or 1 for left-button drawing. The cursor
  starts at the circle's right edge; every turn has exactly zero net HID
  displacement. A final zero-motion report releases drawing before completion.
  `M` during an active circle stops with an error instead of mixing movements.
- `STATS` returns `OK STATS REPORTS=n ELAPSED_US=n MIN_US=n MAX_US=n GAPS=n CIRCLE=n`.
  Counts/timing reset on STREAM. ELAPSED_US spans the first through last
  completed streaming reports; average Hz is `(REPORTS-1)*1000000/ELAPSED_US`.
  GAPS counts completion-task intervals >=1500 us. CIRCLE stays 1 until the
  generator's final release has been processed. Timestamps include firmware
  task wake-up jitter and are not receiver-side packet timestamps.
- `PING` keeps the session alive without changing button state.
- Plain `STOP` discards movement and releases buttons.

The host sends PING every 50 ms while idle. After 250 ms without a valid command,
the firmware stops and releases buttons. CRC/sequence/parser errors, USB loss,
the physical switch or a transfer failure also stop the session. A failed
movement report must never be retried: it may already have reached the host.
Buttons are released when USB becomes available again after a disconnect.

Streaming removes the LOAD/E/RUN start delay. Batch mode still requires its
final report to release buttons and retains a 10 ms minimum event interval.
USB remains full-speed, with the original 6-byte report and 1 ms endpoint.
The dedicated USB task continues while UART reads/writes and acknowledgements
run on the Arduino task. Normal serial source updates remain transport-limited;
continuous USB reporting does not invent new measured movement samples. The
circle command provides fresh device-generated movement every USB slot for
testing. STREAM's 250 ms watchdog and physical stop input apply to circles too.

From the parent project, `python tools/mouse_circle_test.py --port COM6` runs
three click-free circles after a five-second countdown and saves timing JSON.
Add `--draw` only with a blank drawing canvas focused on the receiving PC.
On 2026-10-30 the updated build was flashed to app0 on COM6 after backing up
the previous 3 MiB application to the parent project's `runs/hardware_backups/`.
The flash tool verified its written hash. Three click-free circles completed;
the receiving user confirmed the visible motion. Across 4115 reports, firmware
completion-task timing measured 999.995 Hz, minimum 992 us, maximum 1008 us and
zero gaps >=1.5 ms. All original device/configuration/report descriptor bytes
were found unchanged in the new application. This is completion timing and
visual confirmation, not a receiver-side packet capture. The following older
measurements describe the previous, UART-paced build.

## Build

From the submodule root with Arduino-ESP32 3.3.8 installed:

```powershell
$fqbn = 'esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB'
arduino-cli compile --fqbn $fqbn --output-dir firmware/m2k_stream/build firmware/m2k_stream
```

The task-local toolchain is `.tools/arduino-cli/arduino-cli.exe`, with
`--config-file .tools/arduino-cli.yaml`. Only the compiler, ESP32-S3 libraries,
esptool and core required by this sketch are installed there; downloads were
verified against the package index SHA256 values.

For this backed-up board, app0 starts at 0x10000 and is 3 MiB. Updating only the
application preserves its bootloader, partition table, NVS and other data:

```powershell
python -m esptool --port COM6 --baud 921600 write-flash 0x10000 firmware/m2k_stream/build/m2k_stream.ino.bin
```

Restore the full original image on this same board, if needed:

```powershell
python -m esptool --port COM6 --baud 921600 write-flash 0 .tools/m2k-original-20261012.bin
```

Replug native USB after changing firmware. The standalone `replay.py` utility
still checks for v1; the parent project's `tools/relay_mouse.py` supports v3.

## Verification on 2026-10-12

Arduino-ESP32 3.3.8 compiled the sketch successfully (384,466 bytes of program
storage). All three descriptor byte sequences were found unchanged in the
compiled application. Only app0 was written at 0x10000, and esptool verified
the written hash. After reboot, COM6 returned HELLO v3, the original IDENTITY,
and the expected STREAM capabilities. Native USB reported USB=0, so end-to-end
dragging and USB report timing were not measured in that initial verification.
After connecting native USB, HELLO reported USB=1. Thirty zero-motion streaming
reports completed with median round trip 4.00 ms (maximum 4.14 ms), versus
27.01 ms for ten batch reports. These timings include serial and HID completion,
not the receiving display or capture-card delay. No test clicks were injected.
