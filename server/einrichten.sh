#!/usr/bin/env bash
# Richtet die Server-Seite einmalig ein (Linux oder macOS mit Docker):
#   - .env mit zufaelligen Passwoertern (nur wenn es sie noch nicht gibt)
#   - MQTT-Benutzer waschkueche (Gateway), telegraf und fertigmeldung in mosquitto/config/passwd
# Danach: docker compose up -d
# Ein zweiter Lauf aendert nichts an .env und setzt nur die MQTT-Benutzer neu (gleiche Passwoerter).
set -euo pipefail
cd "$(dirname "$0")"

command -v docker >/dev/null || { echo "Docker fehlt"; exit 1; }
docker compose version >/dev/null 2>&1 || { echo "docker compose fehlt (Docker Compose v2)"; exit 1; }
command -v openssl >/dev/null || { echo "openssl fehlt"; exit 1; }

if [ ! -f .env ]; then
  echo "== .env anlegen"
  umask 077
  {
    echo "TZ=Europe/Berlin"
    echo "INFLUX_PASS=$(openssl rand -hex 16)"
    echo "INFLUX_TOKEN=$(openssl rand -hex 32)"
    echo "GRAFANA_PASS=$(openssl rand -hex 12)"
    echo "MQTT_PASS_WASCHKUECHE=$(openssl rand -hex 24)"
    echo "MQTT_PASS_TELEGRAF=$(openssl rand -hex 24)"
    echo "MQTT_PASS_FERTIGMELDUNG=$(openssl rand -hex 24)"
    echo "# Optional: Push aufs Handy ueber ntfy, z. B. NTFY_URL=https://ntfy.sh/<langes-zufaelliges-thema>"
    echo "NTFY_URL="
    echo "NTFY_TOKEN="
  } > .env
  umask 022
else
  echo "== .env gibt es schon, bleibt unveraendert"
fi

wert() { sed -n "s/^$1=//p" .env | head -1; }

echo "== MQTT-Benutzer anlegen"
# mosquitto_passwd laeuft im Mosquitto-Image; die Datei passwd liegt danach in mosquitto/config/
docker run --rm -i -v "$PWD/mosquitto/config:/mosquitto/config" eclipse-mosquitto:2 sh -s <<EOT
set -e
rm -f /mosquitto/config/passwd
touch /mosquitto/config/passwd
chmod 0600 /mosquitto/config/passwd
mosquitto_passwd -b /mosquitto/config/passwd waschkueche '$(wert MQTT_PASS_WASCHKUECHE)'
mosquitto_passwd -b /mosquitto/config/passwd telegraf '$(wert MQTT_PASS_TELEGRAF)'
mosquitto_passwd -b /mosquitto/config/passwd fertigmeldung '$(wert MQTT_PASS_FERTIGMELDUNG)'
chown mosquitto:mosquitto /mosquitto/config/passwd
EOT

# laeuft der Broker schon, liest er die Benutzer neu ein
docker kill -s HUP waschkueche-mosquitto >/dev/null 2>&1 || true

echo
echo "Fertig. Jetzt starten:  docker compose up -d"
echo "Grafana: http://<dieser Rechner>:3000, Benutzer admin, Passwort GRAFANA_PASS in server/.env"
echo "Fuer das Gateway (firmware/waschkueche/secrets.h):"
echo "  MQTT_HOST = Adresse dieses Rechners im Heimnetz, MQTT_USER = waschkueche,"
echo "  MQTT_PASS = MQTT_PASS_WASCHKUECHE aus server/.env"
