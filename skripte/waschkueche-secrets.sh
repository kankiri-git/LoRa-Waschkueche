#!/usr/bin/env bash
# Legt firmware/waschkueche/secrets.h an (macOS und Linux). Fragt nach dem Heim-WLAN fuer das Gateway und
# optional nach dem MQTT-Zugang; LoRa-Schluessel, die Update-Passwoerter und das Passwort fuer das
# Shelly-WLAN erzeugt es zufaellig.
#
#   bash skripte/waschkueche-secrets.sh
#
# Eine vorhandene secrets.h bleibt unveraendert. Werte aendern: secrets.h von Hand bearbeiten.
# Danach beide Geraete mit dieser secrets.h bauen (skripte/waschkueche-flashen.sh).
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
NEU="$REPO/firmware/waschkueche/secrets.h"
command -v openssl >/dev/null || { echo "openssl fehlt"; exit 1; }

if [ -f "$NEU" ]; then
  echo "secrets.h gibt es schon, nichts geaendert: $NEU"
  exit 0
fi

# Nur Zeichen zulassen, die in einer C-Zeichenkette nichts kaputt machen
pruefen() {
  case "$2" in
    *\"*|*\\*) echo "$1 darf weder \" noch \\ enthalten"; exit 1 ;;
  esac
}

echo "Heim-WLAN fuer das Gateway in der Wohnung (2,4 GHz):"
read -r -p "  Name (SSID): " SSID
read -r -s -p "  Passwort: " PASS; echo
[ -n "$SSID" ] && [ -n "$PASS" ] || { echo "SSID und Passwort werden gebraucht"; exit 1; }
pruefen SSID "$SSID"; pruefen Passwort "$PASS"

AP_VORSCHLAG="wlan-$(openssl rand -hex 2)"
echo
echo "Eigenes WLAN des Senders fuer die Shellys in der Waschkueche. Jeder dort sieht den Namen,"
echo "am besten also einer, der nicht verraet, worum es geht."
read -r -p "  Name [$AP_VORSCHLAG]: " AP_SSID
AP_SSID="${AP_SSID:-$AP_VORSCHLAG}"
pruefen "WLAN-Name" "$AP_SSID"

echo
echo "MQTT-Zugang des Gateways (leer lassen, wenn es noch keinen Server gibt; spaeter in secrets.h ergaenzen)."
echo "Mit dem Server aus server/: Benutzer waschkueche, Passwort MQTT_PASS_WASCHKUECHE aus server/.env"
read -r -p "  Server (IP oder Name): " MQTT_HOST
MQTT_ZEILEN="// MQTT: Server eintragen und die vier Zeilen einkommentieren (siehe secrets.h.example)
// #define MQTT_HOST \"192.168.178.10\"
// #define MQTT_PORT 1883
// #define MQTT_USER \"waschkueche\"
// #define MQTT_PASS \"...\""
if [ -n "$MQTT_HOST" ]; then
  read -r -p "  Benutzer [waschkueche]: " MQTT_USER
  MQTT_USER="${MQTT_USER:-waschkueche}"
  read -r -s -p "  Passwort: " MQTT_PASS; echo
  pruefen Server "$MQTT_HOST"; pruefen Benutzer "$MQTT_USER"; pruefen Passwort "$MQTT_PASS"
  MQTT_ZEILEN="#define MQTT_HOST     \"$MQTT_HOST\"
#define MQTT_PORT     1883
#define MQTT_USER     \"$MQTT_USER\"
#define MQTT_PASS     \"$MQTT_PASS\""
fi

umask 077
cat > "$NEU" <<EOT
// Erzeugt von skripte/waschkueche-secrets.sh am $(date +%Y-%m-%d). Nie veroeffentlichen.
#pragma once
#define WIFI_SSID     "$SSID"
#define WIFI_PASS     "$PASS"
#define OTA_PASS      "$(openssl rand -hex 8)"
#define OTA_PASS_WASC "$(openssl rand -hex 8)"
#define AP_SSID       "$AP_SSID"
#define AP_PASS       "$(openssl rand -hex 8)"
#define LORA_KEY_HEX  "$(openssl rand -hex 16)"
$MQTT_ZEILEN
EOT
echo
echo "secrets.h angelegt: $NEU"
echo "Das Passwort fuer das Shelly-WLAN \"$AP_SSID\" steht dort unter AP_PASS (braucht man beim Einrichten der Shellys)."
