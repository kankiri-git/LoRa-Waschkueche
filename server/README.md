# Server

Die Server-Seite der LoRa-Waschküche: Sie nimmt die Werte des Gateways per MQTT an, speichert sie, zeigt sie in Grafana und meldet, wann Waschmaschine oder Trockner fertig sind. Alles läuft mit Docker Compose auf einem Rechner im Heimnetz, der dauerhaft an ist.

```
Gateway (C6L am Fenster)
    │  MQTT, Port 1883
    ▼
Mosquitto ───► Telegraf ───► InfluxDB ───► Grafana (Dashboard, Alarme)
    │  ▲
    ▼  │ fertig, zustand/<maschine>
Fertig-Meldung ───► ntfy (Push aufs Handy, optional)
```

Die Anleitung für das ganze Projekt steht im [README im Hauptordner](../README.md). Hier geht es nur um diesen Ordner.

## Voraussetzungen

- Ein Rechner, der durchläuft: Mini-PC, NAS mit Docker oder ein Raspberry Pi 4/5 mit 64-Bit-System. Getestet ist der Stack unter Linux auf x86_64. Die Images gibt es auch für 64-Bit-ARM, dort ist er aber nicht ausprobiert.
- Docker mit Docker Compose v2 (`docker compose version` muss gehen) und `openssl`.
- Eine feste Adresse für diesen Rechner im Heimnetz (im Router reservieren), denn das Gateway verbindet sich immer mit derselben Adresse.
- Freie Ports: 1883 (MQTT), 3000 (Grafana) und 8086 (Oberfläche der InfluxDB, optional).

## Einrichten

```bash
cd server
./einrichten.sh
docker compose up -d
docker compose ps        # alle fünf Dienste sollten laufen ("running")
```

`einrichten.sh` legt beim ersten Lauf `.env` mit zufälligen Passwörtern an und trägt die MQTT-Benutzer in `mosquitto/config/passwd` ein. Ein zweiter Lauf lässt `.env` in Ruhe und setzt nur die MQTT-Benutzer neu.

Danach gehören diese Werte in `firmware/waschkueche/secrets.h`:

| `secrets.h` | Wert |
| --- | --- |
| `MQTT_HOST` | Adresse dieses Rechners im Heimnetz |
| `MQTT_PORT` | `1883` |
| `MQTT_USER` | `waschkueche` |
| `MQTT_PASS` | `MQTT_PASS_WASCHKUECHE` aus `server/.env` |

Grafana läuft auf `http://<Server>:3000`, Benutzer `admin`, Passwort `GRAFANA_PASS` aus `.env`. Datenquelle, Dashboard **Waschküche** und Alarme sind schon eingerichtet.

**Schon ein MQTT-Broker im Haus?** Dann dort einen Benutzer für das Gateway anlegen, der unter `sensors/waschkueche/#` schreiben und lesen darf, und aus diesem Ordner nur die Teile übernehmen, die fehlen (Rechte wie in `mosquitto/config/acl`).

## Dienste

| Dienst | Image | Aufgabe |
| --- | --- | --- |
| `mosquitto` | `eclipse-mosquitto:2` | MQTT-Broker auf Port 1883, nur mit Benutzer und Passwort |
| `influxdb` | `influxdb:2.9` | Datenbank, Organisation `home`, Bucket `waschkueche`, Daten ohne Ablaufdatum |
| `telegraf` | `telegraf:1.40` | schreibt die MQTT-Werte in die Datenbank |
| `grafana` | `grafana/grafana:13.2.5` | Dashboard und Alarme auf Port 3000 |
| `fertigmeldung` | wird aus `fertigmeldung/` gebaut | erkennt das Ende eines Laufs und meldet es |

Telegraf legt drei Measurements an:

| Measurement | MQTT-Thema | Inhalt |
| --- | --- | --- |
| `waschkueche` | `sensors/waschkueche/waschmaschine`, `…/trockner` | `power` (W), `energy` (Wh), `online`; Tag `maschine` |
| `waschkueche_funk` | `sensors/waschkueche/diag` | Zustand der Funkstrecke: RSSI, SNR, Verluste, Alter der letzten Sendung … |
| `waschkueche_vibration` | `sensors/waschkueche/vibration` | Kennzahlen des Beschleunigungssensors (nur mit Sensor) |

Alle MQTT-Themen sind im [README im Hauptordner](../README.md#mqtt-themen-sensorswaschkueche) beschrieben.

## Dateien

```
server/
├── docker-compose.yml          alle Dienste
├── .env.example                Vorlage; die echte .env legt einrichten.sh an
├── einrichten.sh               .env und MQTT-Benutzer anlegen
├── mosquitto/config/
│   ├── mosquitto.conf          Broker: keine Anmeldung ohne Passwort
│   ├── acl                     wer welche Themen lesen und schreiben darf
│   └── passwd                  MQTT-Benutzer (von einrichten.sh, nicht im Repo)
├── telegraf/telegraf.conf      welche Themen in welches Measurement gehen
├── grafana/provisioning/
│   ├── datasources/            InfluxDB als Datenquelle
│   ├── dashboards/             Dashboard "Waschküche"
│   └── alerting/               drei Alarme, anfangs pausiert
└── fertigmeldung/              eigener kleiner Dienst in Python, mit Tests
```

`.env` und `mosquitto/config/passwd` stehen in `.gitignore` und gehören nie in ein Repo.

## `.env`

| Variable | Wofür |
| --- | --- |
| `TZ` | Zeitzone für Grafana und die Fertig-Meldung |
| `INFLUX_PASS` | Benutzer `admin` der InfluxDB-Oberfläche auf Port 8086 |
| `INFLUX_TOKEN` | Zugang von Telegraf und Grafana zur Datenbank |
| `GRAFANA_PASS` | Benutzer `admin` in Grafana |
| `MQTT_PASS_WASCHKUECHE` | MQTT-Passwort des Gateways, gehört als `MQTT_PASS` in `secrets.h` |
| `MQTT_PASS_TELEGRAF`, `MQTT_PASS_FERTIGMELDUNG` | MQTT-Passwörter der beiden Dienste |
| `NTFY_URL`, `NTFY_TOKEN` | optional: Push aufs Handy, siehe unten |

**Wichtig:** `INFLUX_PASS`, `INFLUX_TOKEN` und `GRAFANA_PASS` gelten nur beim allerersten Start, danach stehen sie in den Datenbanken der Dienste. Sie später in `.env` zu ändern, reicht nicht; Telegraf und Grafana kommen dann nicht mehr an die Datenbank. Das Grafana-Passwort ändert man in Grafana selbst (Profil) oder so:

```bash
docker compose exec grafana grafana cli admin reset-admin-password <neues-passwort>
```

Die MQTT-Passwörter lassen sich jederzeit ändern: in `.env` anpassen, `./einrichten.sh`, `docker compose up -d`. Für das Gateway danach `secrets.h` anpassen und neu aufspielen.

## Ohne Hardware ausprobieren

Alle Befehle hier und weiter unten laufen im Ordner `server`. Einen Wert so schicken, als käme er vom Gateway:

```bash
P=$(sed -n 's/^MQTT_PASS_WASCHKUECHE=//p' .env)
docker compose exec mosquitto mosquitto_pub -u waschkueche -P "$P" \
  -t sensors/waschkueche/waschmaschine -m '{"power":1850,"energy":1000,"online":true}'
```

Nach etwa 10 Sekunden steht er im Dashboard unter *Leistung*. Mitlesen, was auf dem Broker ankommt (Beenden mit Strg+C):

```bash
P=$(sed -n 's/^MQTT_PASS_TELEGRAF=//p' .env)
docker compose exec mosquitto mosquitto_sub -u telegraf -P "$P" -t 'sensors/waschkueche/#' -v
```

Testwerte bleiben in der Datenbank. Wer mit leerer Datenbank anfangen will, löscht vor dem Einbau alles bis jetzt:

```bash
docker compose exec influxdb influx delete --bucket waschkueche \
  --start 1970-01-01T00:00:00Z --stop "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
```

## Fertig-Meldung

Der Dienst `fertigmeldung` liest die Leistung beider Maschinen und meldet

- einmal je Lauf unter `sensors/waschkueche/fertig`, zum Beispiel `{"maschine":"trockner","text":"Der Trockner ist fertig.","zeit":…}`,
- bei jedem Wechsel den Zustand unter `sensors/waschkueche/zustand/<maschine>` (retained): `laeuft`, `fertig`, `bereit`, `nicht_erreichbar` …

Den Verlauf der letzten Stunden hält er im Volume `fertigmeldung-daten`, damit ein Neustart mitten im Waschgang nichts verliert und keine Meldung doppelt kommt.

**Push aufs Handy:** In `.env` `NTFY_URL=https://ntfy.sh/<langes-zufälliges-thema>` eintragen, `docker compose up -d`, in der App *ntfy* dasselbe Thema abonnieren. Auf ntfy.sh kann jeder mitlesen, der das Thema kennt. Wer mag, betreibt einen eigenen ntfy-Server und trägt in `NTFY_TOKEN` den Zugang ein.

**Schwellen:** Die Vorgaben (Waschmaschine 20 W, Trockner 50 W, Ruhe 4 Minuten unter 5 W) sind ein erster Ansatz. Wie die Regel genau funktioniert und wann man was nachstellt, steht im [README im Hauptordner](../README.md#fertig-meldung). Geändert wird im Dienst `fertigmeldung` in `docker-compose.yml`:

```yaml
    environment:
      - WASCHMASCHINE_STILL_MIN=8
      - TROCKNER_ARBEIT_W=80
```

Danach `docker compose up -d`. Möglich sind je Maschine (`WASCHMASCHINE_…`, `TROCKNER_…`): `ARBEIT_W`, `ARBEIT_MIN`, `STILL_W`, `STILL_MIN`, `STUMM_MIN` (nach so vielen Minuten ohne Wert gilt die Maschine als nicht erreichbar) und `AUS_W` (liegt die Leistung zum Ende hin mindestens eine Minute unter diesem Wert, etwa weil jemand die Maschine ausgeschaltet hat, gilt der Lauf als abgebrochen und wird nicht gemeldet; Vorgabe 0 = nie).

**Tests ohne Geräte:**

```bash
cd fertigmeldung
python3 -m pytest        # braucht pytest und paho-mqtt
```

## Alarme

Grafana bringt drei Alarme mit, die anfangs **pausiert** sind, damit beim Aufbau nichts meldet:

| Alarm | meldet, wenn |
| --- | --- |
| Gateway liefert keine Daten | 15 Minuten nichts vom Gateway |
| Sender sendet nicht mehr | über 30 Minuten keine Sendung aus der Waschküche |
| Steckdose nicht erreichbar | der Sender erreicht einen Shelly 30 Minuten lang nicht |

Wenn alles hängt und läuft: in `grafana/provisioning/alerting/waschkueche.yaml` bei allen drei `isPaused: false` setzen und `docker compose restart grafana`.

Wohin die Meldungen gehen, stellt man in Grafana unter *Alerting → Contact points* ein (Mail, Telegram, ntfy per Webhook und vieles mehr). Für Mail braucht Grafana einen SMTP-Server, zum Beispiel im Dienst `grafana` in `docker-compose.yml`:

```yaml
    environment:
      - GF_SMTP_ENABLED=true
      - GF_SMTP_HOST=smtp.example.com:587
      - GF_SMTP_USER=absender@example.com
      - GF_SMTP_PASSWORD=${SMTP_PASS}
      - GF_SMTP_FROM_ADDRESS=absender@example.com
```

`SMTP_PASS` dann in `.env` eintragen.

## Betrieb

**Logs:** `docker compose logs -f <dienst>`, zum Beispiel `docker compose logs -f fertigmeldung`.

**Aktualisieren:**

```bash
docker compose pull
docker compose up -d --build
```

Mosquitto, InfluxDB und Telegraf folgen damit den Updates ihrer Version (`2`, `2.9`, `1.40`). Grafana ist auf eine genaue Version festgelegt, weil sich Dashboards und Alarme zwischen den Versionen ändern können; für ein Update das Tag in `docker-compose.yml` anpassen und danach Dashboard und Alarme prüfen.

**Sichern:** Die Daten liegen in Docker-Volumes mit dem Vorsatz `waschkueche_`. Kurz anhalten, sichern, weiterlaufen lassen:

```bash
docker compose stop
for v in influxdb-daten influxdb-config grafana-daten fertigmeldung-daten; do
  docker run --rm -v waschkueche_$v:/daten:ro -v "$PWD":/sicherung alpine \
    tar czf /sicherung/$v.tgz -C /daten .
done
docker compose start
```

Dazu `.env` sicher aufheben. Zurückspielen auf einem neuen Rechner: Ordner `server` und `.env` hinlegen, `./einrichten.sh`, dann

```bash
docker compose create
for v in influxdb-daten influxdb-config grafana-daten fertigmeldung-daten; do
  docker run --rm -v waschkueche_$v:/daten -v "$PWD":/sicherung alpine \
    tar xzf /sicherung/$v.tgz -C /daten
done
docker compose start
```

**Anhalten und Entfernen:** `docker compose down` hält alles an, die Daten bleiben. `docker compose down -v` löscht zusätzlich alle Volumes, also alle Messwerte und Einstellungen in Grafana.

## Sicherheit

- Keinen der Ports per Portweiterleitung ins Internet freigeben. MQTT läuft ohne Verschlüsselung und ist nur fürs Heimnetz gedacht.
- Wer die InfluxDB-Oberfläche nicht braucht, nimmt in `docker-compose.yml` beim Dienst `influxdb` den Eintrag `ports` heraus. Telegraf und Grafana erreichen die Datenbank trotzdem.
- Jeder Dienst hat seinen eigenen MQTT-Benutzer mit nur den Rechten, die er braucht (`mosquitto/config/acl`). Das Gateway darf nur unter `sensors/waschkueche/` schreiben und lesen.

## Fehlersuche

| Beobachtung | Ursache, Abhilfe |
| --- | --- |
| Gateway zeigt MQTT „getrennt“ | `MQTT_HOST`, Benutzer und Passwort in `secrets.h` prüfen; Port 1883 durch eine Firewall auf dem Server gesperrt? `docker compose logs mosquitto` zeigt abgelehnte Anmeldungen. |
| Dashboard leer, obwohl Werte ankommen | `docker compose logs telegraf`. Wurde `INFLUX_TOKEN` in `.env` nach dem ersten Start geändert, zurückändern (siehe oben). |
| `fertigmeldung` meldet „MQTT-Anmeldung abgelehnt“ | `./einrichten.sh` noch einmal laufen lassen, dann `docker compose up -d`. |
| Keine Push-Nachricht | `NTFY_URL` in `.env` gesetzt und danach `docker compose up -d`? `docker compose logs fertigmeldung` zeigt Fehler beim Senden. |
| Meldung kommt zu früh oder zu spät | Schwellen nachstellen, siehe [Fertig-Meldung](#fertig-meldung). |
| Grafana-Passwort vergessen | `docker compose exec grafana grafana cli admin reset-admin-password <neues-passwort>` |
