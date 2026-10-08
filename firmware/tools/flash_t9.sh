#!/usr/bin/env bash
# Write the predictive-text dictionary into the handset's `t9dic` partition.
#
# `idf.py flash` does NOT write this partition: the dictionary is data, not part
# of the app image. Without it the keypad still works, but shows digits instead
# of suggesting words. Run this once per handset (and again after rebuilding
# the dictionary). It does not touch the app, the settings or the pairings.
#
#   tools/flash_t9.sh /dev/ttyACM0
#
# The dictionary is built by tools/build_t9_dict.py from a system word list,
# filtered for a child's device. data/t9.dic is a ready-built copy.
set -euo pipefail
PORT="${1:-/dev/ttyACM0}"
DICT="${2:-$(dirname "$0")/../data/t9.dic}"
OFFSET=0x610000          # must match partitions.csv

[ -f "$DICT" ] || { echo "no dictionary at $DICT" >&2; exit 1; }
SZ=$(stat -c%s "$DICT")
echo "writing $DICT ($SZ bytes) to $OFFSET on $PORT"
python -m esptool --chip esp32s3 -p "$PORT" -b 460800 write_flash "$OFFSET" "$DICT"
echo "done - reboot and look for '[t9] N words mapped from flash'"
