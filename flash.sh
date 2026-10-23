#!/bin/zsh
# Uso:  ./flash.sh build [sketch]   -> compila
#       ./flash.sh upload [sketch]  -> compila + carica sulla scheda
#       ./flash.sh monitor          -> apre il monitor seriale (Ctrl+C per uscire)
#       ./flash.sh ports            -> elenca le porte seriali
# Default sketch: cyd_dash
set -e
CLI="${ARDUINO_CLI:-/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli}"
CFG="$HOME/.arduinoIDE/arduino-cli.yaml"
FQBN="esp32:esp32:esp32:UploadSpeed=115200"   # ESP32 Dev Module, 115200 per il CH340
HERE="$(cd "$(dirname "$0")" && pwd)"
SKETCH="$HERE/${2:-cyd_dash}"

gen_secrets() {   # crea cyd_dash/secrets.h da .env (WIFI_SSID, WIFI_PASS, DASH_HOST, DASH_PORT)
  [ "$(basename "$SKETCH")" = "cyd_dash" ] || return 0
  ENVF="$HERE/.env"
  if [ ! -f "$ENVF" ]; then
    cp "$SKETCH/secrets.example.h" "$SKETCH/secrets.h"
    echo "secrets.h generato per USB seriale (Wi-Fi spento)"
    return 0
  fi
  val() { grep -E "^$1=" "$ENVF" | head -1 | cut -d= -f2- | sed -e 's/^["'"'"']//' -e 's/["'"'"']$//'; }
  SS="$(val WIFI_SSID)"; PW="$(val WIFI_PASS)"; HO="$(val DASH_HOST)"; PO="$(val DASH_PORT)"
  if [ -z "$SS$HO" ]; then
    cp "$SKETCH/secrets.example.h" "$SKETCH/secrets.h"
    return 0
  fi
  if [ -n "$SS" ] && [ -z "$HO" ]; then
    echo "DASH_HOST richiesto quando WIFI_SSID è impostato" >&2
    exit 1
  fi
  printf '#define WIFI_SSID "%s"\n#define WIFI_PASS "%s"\n#define DASH_HOST "%s"\n#define DASH_PORT %s\n' \
    "$SS" "$PW" "${HO:-127.0.0.1}" "${PO:-8765}" > "$SKETCH/secrets.h"
  echo "secrets.h generato da .env (ssid='$SS' host=${HO:-127.0.0.1})"
}

port() {
  ls /dev/cu.* 2>/dev/null | grep -iE "usbserial|wchusb|SLAB|usbmodem" | head -1
}

case "$1" in
  build)   gen_secrets; "$CLI" --config-file "$CFG" compile --fqbn "$FQBN" "$SKETCH" ;;
  upload)  P="$(port)"; [ -z "$P" ] && { echo "Nessuna porta seriale trovata: la scheda non è vista dal Mac."; exit 1; }
           echo "Porta: $P"; gen_secrets
           "$CLI" --config-file "$CFG" compile --fqbn "$FQBN" "$SKETCH"
           "$CLI" --config-file "$CFG" upload --fqbn "$FQBN" -p "$P" "$SKETCH" ;;
  monitor) P="$(port)"; [ -z "$P" ] && { echo "Nessuna porta seriale trovata."; exit 1; }
           "$CLI" --config-file "$CFG" monitor -p "$P" -c baudrate=115200 ;;
  ports)   ls -1 /dev/cu.* ;;
  *)       sed -n '2,7p' "$0" ;;
esac
