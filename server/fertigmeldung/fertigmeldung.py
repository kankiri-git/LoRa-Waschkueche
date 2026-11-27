"""Fertig-Meldung fuer Waschmaschine und Trockner.

Hoert per MQTT auf sensors/waschkueche/waschmaschine und .../trockner ({"power": W, "energy": Wh, "online": bool},
schickt das Gateway bei jeder Sendung aus der Waschkueche), erkennt das Ende eines Laufs (fertig.py) und meldet es:

  sensors/waschkueche/fertig                {"maschine": "waschmaschine", "text": "Die Waschmaschine ist fertig.",
                                             "zeit": <Unix-Zeit>}   (nicht retained, einmal je Lauf)
  sensors/waschkueche/zustand/<maschine>    {"zustand": "laeuft"|"fertig"|"bereit"|"aus"|"unbekannt"|
                                             "nicht_erreichbar", "seit": <Unix-Zeit oder null>, "leistung": W}
                                            (retained, bei jedem Wechsel des Zustands)

Optional zusaetzlich als Push-Nachricht ueber ntfy (https://ntfy.sh oder ein eigener ntfy-Server): NTFY_URL setzen,
zum Beispiel https://ntfy.sh/<eigenes-langes-zufaelliges-thema>.

Einstellungen ueber Umgebungsvariablen (siehe server/.env.example):
  MQTT_HOST, MQTT_PORT, MQTT_USER, MQTT_PASS, MQTT_BASIS (Vorgabe sensors/waschkueche),
  NTFY_URL, NTFY_TOKEN (optional), DATEN (Ordner fuer den Verlauf, Vorgabe /data),
  Schwellen je Maschine, z. B. WASCHMASCHINE_ARBEIT_W=20, TROCKNER_STILL_MIN=4 (siehe MASCHINEN unten).

Der Verlauf der letzten Stunden liegt in DATEN/verlauf.json, damit ein Neustart mitten im Waschgang nichts
verliert und eine Meldung nicht doppelt kommt.
"""
from __future__ import annotations

import json
import logging
import os
import signal
import sys
import threading
import time
import urllib.request
from dataclasses import fields, replace

import paho.mqtt.client as mqtt

import fertig

log = logging.getLogger("fertigmeldung")

# Vorgaben: ein erster Ansatz, an den eigenen Maschinen nachstellen (Umgebungsvariablen oder hier).
MASCHINEN = {
    "waschmaschine": fertig.Maschine("waschmaschine", "Waschmaschine", "Die Waschmaschine ist fertig.",
                                     arbeit_w=20, arbeit_min=5, still_w=5, still_min=4, aus_w=0, stumm_min=15),
    "trockner": fertig.Maschine("trockner", "Trockner", "Der Trockner ist fertig.",
                                arbeit_w=50, arbeit_min=5, still_w=5, still_min=4, aus_w=0, stumm_min=15),
}
AUFHEBEN_S = 6 * 3600      # so viel Verlauf je Maschine (laengstes Programm plus Reserve)
GEMELDET_S = 2 * 86400     # so lange merkt sich der Dienst, was er schon gemeldet hat
PRUEFEN_S = 60             # so oft wird der Zustand auch ohne neue Werte geprueft (nicht erreichbar)


def maschinen_aus_umgebung(umgebung=os.environ) -> dict[str, fertig.Maschine]:
    """Schwellen aus Umgebungsvariablen <MASCHINE>_<FELD> (z. B. TROCKNER_ARBEIT_W=80) uebernehmen."""
    erg = {}
    for k, m in MASCHINEN.items():
        neu = {}
        for f in fields(m):
            if f.type not in ("float", float):
                continue
            wert = umgebung.get(f"{k.upper()}_{f.name.upper()}")
            if wert:
                neu[f.name] = float(wert)
        erg[k] = replace(m, **neu)
    return erg


class Melder:
    def __init__(self, maschinen: dict[str, fertig.Maschine], datei: str | None, senden, ntfy_url: str = "",
                 ntfy_token: str = ""):
        self.maschinen = maschinen
        self.datei = datei
        self.senden = senden                  # senden(thema_relativ, nutzlast_dict, retained)
        self.ntfy_url = ntfy_url
        self.ntfy_token = ntfy_token
        self.verlauf: dict[str, list[tuple[float, float | None]]] = {k: [] for k in maschinen}
        self.zuletzt: dict[str, float] = {}   # Zeit des letzten Werts je Maschine (auch unbekannt)
        self.gemeldet: dict[str, float] = {}  # Kennung -> Zeit der Meldung
        self.zustaende: dict[str, tuple] = {}
        self.lock = threading.Lock()
        self._laden()

    # ---- Speicher -------------------------------------------------------------------------------------------
    def _laden(self) -> None:
        if not self.datei or not os.path.exists(self.datei):
            return
        try:
            with open(self.datei, encoding="utf-8") as f:
                d = json.load(f)
            for k in self.maschinen:
                self.verlauf[k] = [(float(t), None if w is None else float(w)) for t, w in d.get("verlauf", {}).get(k, [])]
                if self.verlauf[k]:
                    self.zuletzt[k] = self.verlauf[k][-1][0]
            self.gemeldet = {str(k): float(v) for k, v in d.get("gemeldet", {}).items()}
            log.info("Verlauf geladen: %s", {k: len(v) for k, v in self.verlauf.items()})
        except (OSError, ValueError, TypeError, AttributeError) as e:
            log.warning("Verlauf nicht lesbar (%s), beginne leer", e)

    def _speichern(self) -> None:
        if not self.datei:
            return
        tmp = self.datei + ".neu"
        try:
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump({"verlauf": self.verlauf, "gemeldet": self.gemeldet}, f)
            os.replace(tmp, self.datei)
        except OSError as e:
            log.warning("Verlauf nicht gespeichert: %s", e)

    # ---- Auswertung -----------------------------------------------------------------------------------------
    def wert(self, maschine: str, nutzlast: bytes, jetzt: float | None = None) -> None:
        """Ein Wert vom Gateway: {"power": W, "online": bool}."""
        if maschine not in self.maschinen:
            return
        try:
            d = json.loads(nutzlast)
            online = d.get("online", True) is not False
            watt = float(d["power"]) if online else None
        except (ValueError, TypeError, KeyError, AttributeError):
            log.warning("unlesbarer Wert fuer %s: %r", maschine, nutzlast[:200])
            return
        jetzt = time.time() if jetzt is None else jetzt
        with self.lock:
            v = self.verlauf[maschine]
            v.append((jetzt, watt))
            v.sort(key=lambda x: x[0])
            while v and v[0][0] < jetzt - AUFHEBEN_S:
                v.pop(0)
            self.zuletzt[maschine] = jetzt
            self._pruefen(maschine, jetzt)
            self._speichern()

    def pruefen_alle(self, jetzt: float | None = None) -> None:
        jetzt = time.time() if jetzt is None else jetzt
        with self.lock:
            for k in self.maschinen:
                self._pruefen(k, jetzt)
            for kennung, t in list(self.gemeldet.items()):
                if t < jetzt - GEMELDET_S:
                    del self.gemeldet[kennung]

    def _pruefen(self, maschine: str, jetzt: float) -> None:
        m = self.maschinen[maschine]
        v = self.verlauf[maschine]
        # Nichts gilt ueber den letzten Wert hinaus: ausgewertet wird bis zur Zeit des letzten Werts.
        stand = v[-1][0] if v else jetzt
        a = fertig.auswerten(v, stand, m)
        for lauf in a.laeufe:
            if not lauf.fertig or lauf.ende is None:
                continue
            kennung = fertig.kennung(m, lauf)
            if kennung in self.gemeldet:
                continue
            self.gemeldet[kennung] = jetzt
            if jetzt - lauf.ende > 2 * 3600:
                continue        # alter Lauf aus dem gespeicherten Verlauf: nicht mehr melden
            log.info("%s (Ende %s)", m.text, time.strftime("%H:%M", time.localtime(lauf.ende)))
            self.senden("fertig", {"maschine": maschine, "text": m.text, "zeit": round(lauf.ende)}, False)
            self._push(m.text)
        zst, seit = fertig.zustand(a, stand, m)
        letzter = self.zuletzt.get(maschine)
        if letzter is None or jetzt - letzter > m.stumm_min * 60 or (v and v[-1][1] is None):
            zst, seit = "nicht_erreichbar", None
        neu = (zst, None if seit is None else round(seit))
        if self.zustaende.get(maschine) != neu:      # nur bei einem Wechsel, die Leistung steht zur Info dabei
            self.zustaende[maschine] = neu
            self.senden(f"zustand/{maschine}", {"zustand": zst, "seit": neu[1], "leistung": a.wert}, True)

    def _push(self, text: str) -> None:
        if not self.ntfy_url:
            return
        kopf = {"Title": "Waschkueche", "Tags": "basket"}
        if self.ntfy_token:
            kopf["Authorization"] = f"Bearer {self.ntfy_token}"
        try:
            req = urllib.request.Request(self.ntfy_url, data=text.encode("utf-8"), headers=kopf, method="POST")
            with urllib.request.urlopen(req, timeout=15) as r:
                r.read()
        except OSError as e:
            log.warning("ntfy fehlgeschlagen: %s", e)


def main() -> None:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s", stream=sys.stdout)
    basis = os.environ.get("MQTT_BASIS", "sensors/waschkueche").rstrip("/")
    daten = os.environ.get("DATEN", "/data")
    os.makedirs(daten, exist_ok=True)
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="waschkueche-fertigmeldung")
    if os.environ.get("MQTT_USER"):
        client.username_pw_set(os.environ["MQTT_USER"], os.environ.get("MQTT_PASS", ""))

    def senden(thema: str, nutzlast: dict, retained: bool) -> None:
        client.publish(f"{basis}/{thema}", json.dumps(nutzlast, ensure_ascii=False), qos=1, retain=retained)

    melder = Melder(maschinen_aus_umgebung(), os.path.join(daten, "verlauf.json"), senden,
                    os.environ.get("NTFY_URL", ""), os.environ.get("NTFY_TOKEN", ""))

    def verbunden(c, _userdata, _flags, grund, _props):
        if grund.is_failure:
            log.error("MQTT-Anmeldung abgelehnt: %s", grund)
            return
        log.info("MQTT verbunden")
        for k in melder.maschinen:
            c.subscribe(f"{basis}/{k}", qos=0)

    def nachricht(_c, _userdata, msg):
        melder.wert(msg.topic.rsplit("/", 1)[-1], msg.payload)

    client.on_connect = verbunden
    client.on_message = nachricht
    client.reconnect_delay_set(min_delay=2, max_delay=60)
    client.connect_async(os.environ.get("MQTT_HOST", "mosquitto"), int(os.environ.get("MQTT_PORT", "1883")), keepalive=60)
    client.loop_start()

    stopp = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stopp.set())
    signal.signal(signal.SIGINT, lambda *_: stopp.set())
    while not stopp.wait(PRUEFEN_S):
        melder.pruefen_alle()
    client.loop_stop()
    client.disconnect()


if __name__ == "__main__":
    main()
