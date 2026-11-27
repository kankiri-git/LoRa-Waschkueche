#!/usr/bin/env bash
# LoRa-Reichweitentest fuer zwei M5Stack Unit C6L bauen und per USB aufspielen (macOS und Linux).
#
#   bash skripte/lora-test-flashen.sh                         # nur bauen, Ports anzeigen
#   bash skripte/lora-test-flashen.sh /dev/cu.usbmodem1101    # bauen + aufspielen (Linux: /dev/ttyACM0)
#   bash skripte/lora-test-flashen.sh /dev/cu.usbmodem1101 loeschen   # vorher Flash ganz leeren
#
# Achtung: Aufspielen ersetzt die Firmware auf dem Geraet (ab Werk oft Meshtastic).
# Springt der C6L nicht in den Download-Modus: Taster gedrueckt halten, USB einstecken,
# bis die LED rot wird, dann nochmal aufspielen.
set -euo pipefail
# shellcheck source-path=SCRIPTDIR source=gemeinsam.sh
source "$(dirname "$0")/gemeinsam.sh"

SKETCH="$REPO/firmware/lora-test"
BUILD="$REPO/build/lora-test"

echo "== Bibliotheken"
bibliotheken RadioLib U8g2

echo "== Bauen"
arduino-cli compile --fqbn "$FQBN" --output-dir "$BUILD" "$SKETCH"

PORT="${1:-}"
if [ -z "$PORT" ]; then
  echo
  ports_zeigen
  echo "Zum Aufspielen: bash $0 <Port>"
  exit 0
fi

if [ "${2:-}" = "loeschen" ]; then
  ESPTOOL="$(esptool_finden)"
  echo "== Flash ganz loeschen ($ESPTOOL)"
  "$ESPTOOL" --chip esp32c6 --port "$PORT" erase_flash
  sleep 5
fi

echo "== Aufspielen auf $PORT"
arduino-cli upload -p "$PORT" --fqbn "$FQBN" --input-dir "$BUILD" "$SKETCH"
echo "Fertig. Nach dem Neustart steht auf dem Display 'Rolle: WOHN'."
echo "Fuer das Geraet in der Waschkueche: Taster 3 s halten -> Rolle WASC."
