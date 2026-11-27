#!/usr/bin/env bash
# Waschkueche-Firmware bauen und aufspielen (macOS und Linux). Die Rolle steht beim Bauen fest:
# das Geraet in der Waschkueche (wasc) enthaelt weder Heim-WLAN noch OTA_PASS noch MQTT-Zugang.
#
#   bash skripte/waschkueche-flashen.sh wohn                         # nur bauen, Ports zeigen
#   bash skripte/waschkueche-flashen.sh wohn /dev/cu.usbmodem1101    # Gateway per USB (Linux: /dev/ttyACM0)
#   bash skripte/waschkueche-flashen.sh wohn ota 192.168.178.60      # Gateway per WLAN (IP des Gateways)
#   bash skripte/waschkueche-flashen.sh wasc                         # nur bauen und pruefen
#   bash skripte/waschkueche-flashen.sh wasc /dev/cu.usbmodem1101    # Sender per USB, Flash vorher ganz leer
#   bash skripte/waschkueche-flashen.sh wasc web                     # Sender vor Ort, Rechner im WLAN des Senders
#
# Beide Geraete brauchen dieselbe Version und dieselbe secrets.h (gemeinsamer LoRa-Schluessel).
# Nach dem Bauen prueft das Skript, dass die Werte der anderen Rolle nicht im Image stehen, und bricht sonst ab.
# Per USB wird der Sender immer erst ganz geloescht: so verschwinden auch alte Firmware und ein im Flash
# gemerktes Heim-WLAN (zum Beispiel vom Reichweitentest, bei dem beide Geraete als WOHN starten).
set -euo pipefail
# shellcheck source-path=SCRIPTDIR source=gemeinsam.sh
source "$(dirname "$0")/gemeinsam.sh"

SKETCH="$REPO/firmware/waschkueche"

ROLLE="${1:-}"
case "$ROLLE" in
  wohn) DEFINE="ROLLE_WOHN" ;;
  wasc) DEFINE="ROLLE_WASC" ;;
  *) echo "Rolle fehlt: bash $0 wohn|wasc [/dev/... | ota IP | web]"; exit 1 ;;
esac
ZIEL="${2:-}"
BUILD="$REPO/build/waschkueche-$ROLLE"
BIN="$BUILD/waschkueche.ino.bin"
SECRETS="$SKETCH/secrets.h"

[ -f "$SECRETS" ] || { echo "secrets.h fehlt: bash $REPO/skripte/waschkueche-secrets.sh"; exit 1; }
bibliotheken RadioLib U8g2 ArduinoJson PubSubClient

wert() { sed -n "s/^#define $1 *\"\(.*\)\".*/\1/p" "$SECRETS" | head -1; }

# Prueft, dass die genannten Werte aus secrets.h nicht im Image stehen. Zeigt nur Namen, nie Werte.
nicht_im_image() {
  local fehler=0 name w
  for name in "$@"; do
    w="$(wert "$name")"
    [ -n "$w" ] || continue
    if LC_ALL=C grep -q -F -a -e "$w" "$BIN"; then
      echo "FEHLER: $name steht im Image"
      fehler=1
    else
      echo "ok: $name nicht im Image"
    fi
  done
  if [ "$fehler" = 1 ]; then
    rm -f "$BIN"
    echo "Image geloescht, nichts aufgespielt."
    exit 1
  fi
}

echo "== Bauen ($ROLLE)"
# BAU_ZEIT: daraus startet ein frisch geloeschter Sender seinen Sendezaehler (immer hoeher als bei jedem aelteren Image)
printf '// Erzeugt von skripte/waschkueche-flashen.sh, wird nach dem Bauen geloescht.\n#define %s\n#define BAU_ZEIT %sUL\n' \
  "$DEFINE" "$(date +%s)" > "$SKETCH/rolle.h"
trap 'rm -f "$SKETCH/rolle.h"' EXIT
arduino-cli compile --fqbn "$FQBN" --build-path "$BUILD" "$SKETCH"
rm -f "$SKETCH/rolle.h"

echo "== Pruefen"
if [ "$ROLLE" = wasc ]; then
  nicht_im_image WIFI_SSID WIFI_PASS OTA_PASS MQTT_PASS
else
  nicht_im_image AP_PASS OTA_PASS_WASC
fi

case "$ZIEL" in
  "")
    echo
    ports_zeigen
    ;;
  ota)
    [ "$ROLLE" = wohn ] || { echo "ota geht nur fuer wohn; fuer den Sender: wasc web"; exit 1; }
    IP="${3:-}"
    [ -n "$IP" ] || { echo "IP des Gateways fehlt: bash $0 wohn ota <IP> (steht auf seiner Statusseite und im Router)"; exit 1; }
    ESPOTA="$(espota_finden)"
    echo "== Update per WLAN auf $IP"
    python3 "$ESPOTA" -i "$IP" -p 3232 -a "$(wert OTA_PASS)" -f "$BIN"
    ;;
  web)
    [ "$ROLLE" = wasc ] || { echo "web geht nur fuer wasc; fuer das Gateway: wohn ota IP"; exit 1; }
    echo "== Update ueber http://192.168.4.1/update (Rechner muss im WLAN \"$(wert AP_SSID)\" des Senders sein)"
    # Passwort ueber stdin an curl, damit es nicht in der Prozessliste steht
    printf 'user = "admin:%s"\n' "$(wert OTA_PASS_WASC)" |
      curl -sS -f -K - --max-time 120 -F "firmware=@$BIN" http://192.168.4.1/update
    echo
    echo "Fertig. Der Sender startet neu."
    ;;
  /dev/*)
    if [ "$ROLLE" = wasc ]; then
      ESPTOOL="$(esptool_finden)"
      echo "== Flash ganz loeschen"
      "$ESPTOOL" --chip esp32c6 --port "$ZIEL" erase_flash
      sleep 5
    fi
    echo "== Aufspielen auf $ZIEL"
    arduino-cli upload -p "$ZIEL" --fqbn "$FQBN" --input-dir "$BUILD" "$SKETCH"
    echo "Fertig. Nach dem Neustart steht die Rolle auf dem Display."
    ;;
  *)
    echo "Unbekanntes Ziel: $ZIEL (/dev/..., ota IP oder web)"
    exit 1
    ;;
esac
