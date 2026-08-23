#!/usr/bin/env bash
# soak_battery.sh — run a sequence of soakctl manifests overnight, analyzing after each.
# Usage: soak_battery.sh <output-dir> <manifest.json> [more manifests...]
# Events on stdout: BATTERY_SOAK_BEGIN/DONE/FAILED <name>, BATTERY_DONE.
# Per-soak outputs in <output-dir>: <name>.events, <name>-analysis.md, <name>-conversion.txt.
set -uo pipefail
cd "$(dirname "$0")/.."
BATT_DIR="${1:?usage: soak_battery.sh <output-dir> <manifest...>}"
shift
mkdir -p "$BATT_DIR"
for MF in "$@"; do
  NAME=$(basename "$MF" .json)
  echo "BATTERY_SOAK_BEGIN $NAME"
  python3 tools/soakctl.py "$MF" | tee "$BATT_DIR/$NAME.events"
  SOAK_RC=${PIPESTATUS[0]}
  LOG=$(grep -aoE "log=[^ ]+" "$BATT_DIR/$NAME.events" | head -1 | cut -d= -f2)
  if [ "$SOAK_RC" -eq 0 ] && [ -n "$LOG" ] && [ -f "$LOG" ] && grep -q "^SOAK_DONE " "$BATT_DIR/$NAME.events"; then
    python3 tools/analyze_bot_log.py "$LOG" > "$BATT_DIR/$NAME-analysis.md" 2>&1
    python3 tools/flag_conversion.py "$LOG" > "$BATT_DIR/$NAME-conversion.txt" 2>&1
    echo "BATTERY_SOAK_DONE $NAME log=$LOG"
  else
    echo "BATTERY_SOAK_FAILED $NAME log=${LOG:-none}"
  fi
  # Rule 2: never leave a server running between soaks.
  if pgrep -x Descent3 >/dev/null; then
    echo "BATTERY_LEFTOVER_SERVER $NAME - killing"
    pkill -9 -x Descent3
    sleep 3
  fi
  sleep 5
done
echo BATTERY_DONE
