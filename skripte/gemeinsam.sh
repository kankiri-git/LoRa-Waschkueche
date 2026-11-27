# shellcheck shell=bash disable=SC2034
# Gemeinsame Teile der Skripte (wird mit "source" geladen, nicht direkt ausfuehren).
# Laeuft auf macOS und Linux. Voraussetzung: arduino-cli mit dem ESP32-Core (siehe README, "Werkzeuge").

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FQBN="esp32:esp32:esp32c6:CDCOnBoot=cdc,PartitionScheme=min_spiffs"   # 1,9 MB App mit OTA

command -v arduino-cli >/dev/null || { echo "arduino-cli fehlt (siehe README, Werkzeuge)"; exit 1; }

# Installiert fehlende Bibliotheken ueber arduino-cli
bibliotheken() {
  local lib vorhanden
  vorhanden="$(arduino-cli lib list 2>/dev/null || true)"
  for lib in "$@"; do
    grep -q "^$lib " <<< "$vorhanden" || arduino-cli lib install "$lib"
  done
}

# Angeschlossene C6L zeigen (macOS: /dev/cu.usbmodem..., Linux: /dev/ttyACM...)
ports_zeigen() {
  echo "== Angeschlossene C6L (USB):"
  ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null || echo "keiner gefunden"
}

# Ordner, in denen arduino-cli Werkzeuge ablegt (macOS, Linux, oder per Umgebungsvariable umgebogen)
_arduino_daten() {
  printf '%s\n' "${ARDUINO_DIRECTORIES_DATA:-}" "${ARDUINO_DIRECTORIES_USER:-}" \
    "$HOME/Library/Arduino15" "$HOME/.arduino15"
}

esptool_finden() {
  local d c e=""
  while read -r d; do
    [ -n "$d" ] || continue
    for c in "$d"/packages/esp32/tools/esptool_py/*/esptool; do
      [ -x "$c" ] && e="$c"
    done
  done < <(_arduino_daten)
  [ -z "$e" ] && e="$(command -v esptool || command -v esptool.py || true)"
  [ -n "$e" ] || { echo "esptool nicht gefunden (kommt mit dem ESP32-Core oder per pip install esptool)" >&2; exit 1; }
  echo "$e"
}

espota_finden() {
  local d c e=""
  while read -r d; do
    [ -n "$d" ] || continue
    for c in "$d"/packages/esp32/hardware/esp32/*/tools/espota.py; do
      [ -f "$c" ] && e="$c"
    done
  done < <(_arduino_daten)
  [ -n "$e" ] || { echo "espota.py nicht gefunden (kommt mit dem ESP32-Core)" >&2; exit 1; }
  echo "$e"
}
