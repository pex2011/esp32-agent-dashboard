#!/bin/zsh
# Avvia il server della dashboard. Esempi:
#   server/run.sh                                   # solo Wi-Fi (HTTP :8765)
#   server/run.sh --serial auto                     # anche via USB, porta trovata da sola
#   server/run.sh --serial auto --no-usage          # senza leggere i consumi
HERE="$(cd "$(dirname "$0")" && pwd)"
PY=""
for c in python3 /Library/Frameworks/Python.framework/Versions/3.12/bin/python3 /opt/homebrew/bin/python3 /usr/bin/python3; do
  command -v "$c" >/dev/null 2>&1 && "$c" -c "import serial" 2>/dev/null && { PY="$c"; break; }
done
if [ -z "$PY" ]; then PY=python3; echo "pyserial mancante: lo installo per $PY"; "$PY" -m pip install --quiet --user pyserial || "$PY" -m pip install --quiet --user --break-system-packages pyserial; fi
ARGS=()
while [ $# -gt 0 ]; do
  if [ "$1" = "--serial" ] && [ "$2" = "auto" ]; then
    P=$(ls /dev/cu.* 2>/dev/null | grep -iE "usbserial|wchusb|SLAB" | head -1)
    [ -z "$P" ] && { echo "nessuna porta USB trovata, parto solo in Wi-Fi"; shift 2; continue; }
    ARGS+=(--serial "$P"); shift 2; continue
  fi
  ARGS+=("$1"); shift
done
exec "$PY" -u -I "$HERE/dash_server.py" "${ARGS[@]}"
