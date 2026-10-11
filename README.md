# Mouse spoofing and microcontroller output

Standalone ESP32-S3 firmware, serial path playback, HID fingerprint comparison,
and device investigation artifacts extracted from `mouse_generator`.
The tools consume JSON paths and do not import the movement generator.

Requires Python 3.10+. Run commands from this project's root directory.

```powershell
python -m pip install -r requirements-hardware.txt
python replay.py examples/mouse_smoke.json --dry-run
python -m unittest discover -v
```

See the [firmware guide](firmware/README.md) for wiring, building, flashing,
playback, and identity configuration. Dependencies are only needed for device
access; dry runs and unit tests use the Python standard library.

## Generate input paths

Paths are JSON arrays of `[t_ms, x, y, click]` rows: elapsed milliseconds,
absolute pixel coordinates, and a 0/1 click marker. The first row anchors
relative movement; it does not position the receiving cursor. Clicks require
`--clicks`. See the firmware guide for timing and displacement limits.

Generate a file from the sibling project, then return here to inspect it:

```powershell
cd ../mouse_generator
python generate.py 400 300 1200 700 --json > ../mouse_spoofing/path.json
cd ../mouse_spoofing
python replay.py path.json --dry-run
```

The generator has its own model dependencies. Alternatively, supply a JSON
file from any program using the same row format.

## Project contents

- `firmware/`: ESP32-S3 source and setup guide.
- `replay.py`: JSON conversion and serial playback.
- `hid_check.py`: read-only device snapshots and comparisons.
- `test_hardware.py`: protocol, conversion, and descriptor tests.
- `examples/`: small playback fixtures.
- `reports/`: saved device investigations.
- `data/hid_checks/`: local investigation captures (ignored by Git).

Standard HID compatibility does not establish that a device is indistinguishable
from a commercial mouse; the firmware guide describes observable differences.
