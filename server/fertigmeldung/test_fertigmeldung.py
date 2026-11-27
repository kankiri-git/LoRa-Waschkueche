"""Tests ohne Geraete und ohne MQTT: python3 -m pytest server/fertigmeldung (braucht pytest und paho-mqtt).

Der Waschgang unten ist ein Nachbau ohne Messung an einer echten Maschine: so schickt der Sender die Werte
(Lebenszeichen alle 5 min, solange die Maschine laeuft alle 30 s, bei jedem Wechsel ueber oder unter 5 W sofort)."""
import json

import fertig
import fertigmeldung

WASCH = fertigmeldung.MASCHINEN["waschmaschine"]
START = 1_800_000_000.0


def waschgang(start=START, pause_s=180, ruhe_danach=1800):
    """([(Zeit, Watt)], Zeit des ersten ruhigen Werts nach dem Ende): Fuellen, Heizen mit 2 kW, Waschen mit einer
    Pause unter 5 W, Schleudern, Ende."""
    p = [(start - 600.0, 0.6), (start - 300.0, 0.6)]
    t = float(start)

    def stueck(dauer, watt):
        nonlocal t
        for i in range(int(dauer // 30)):
            p.append((t, float(watt[i % len(watt)])))
            t += 30

    stueck(180, [12, 45, 8, 60])              # Fuellen
    stueck(900, [2050, 2040, 2060])           # Heizen
    stueck(1200, [120, 6, 95, 140, 7])        # Waschen: Trommel dreht, steht, dreht
    p.append((t, 3.0))                        # Einweichen unter 5 W: der naechste Wert kommt erst beim Wechsel
    t += pause_s
    stueck(600, [110, 8, 130])
    stueck(480, [320, 450, 380, 290])         # Schleudern
    ende = t + 10
    p.append((ende, 2.5))                     # Wechsel unter 5 W: sofort
    k = ende + 300
    while k <= ende + ruhe_danach:
        p.append((k, 0.4))                    # Lebenszeichen
        k += 300
    return p, ende


def test_fertig_vier_minuten_nach_dem_ende():
    p, ende = waschgang()
    a = fertig.auswerten(p, p[-1][0], WASCH)
    assert [(l.fertig, l.ende) for l in a.laeufe] == [(True, ende + 240)]
    assert a.laeufe[0].spitze == 2060
    assert fertig.zustand(a, p[-1][0], WASCH) == ("fertig", ende + 240)


def test_erst_fertig_wenn_eine_sendung_die_ruhe_bestaetigt():
    p, ende = waschgang()
    bis_ende = [x for x in p if x[0] <= ende]
    a = fertig.auswerten(bis_ende, bis_ende[-1][0], WASCH)
    assert a.laeufe == [] and fertig.zustand(a, bis_ende[-1][0], WASCH)[0] == "laeuft"
    mit_lebenszeichen = [x for x in p if x[0] <= ende + 300]
    assert [l.ende for l in fertig.auswerten(mit_lebenszeichen, ende + 300, WASCH).laeufe] == [ende + 240]


def test_pause_unter_der_ruhezeit_meldet_nicht():
    p, ende = waschgang(pause_s=230)
    mitten = [x for x in p if x[0] < ende]
    a = fertig.auswerten(mitten, mitten[-1][0], WASCH)
    assert a.laeufe == [] and fertig.zustand(a, mitten[-1][0], WASCH)[0] == "laeuft"
    assert len(fertig.auswerten(p, p[-1][0], WASCH).laeufe) == 1


def test_pause_laenger_als_die_ruhezeit_meldet_zu_frueh():
    """Bekannte Grenze: eine Pause unter 5 W, die laenger als still_min dauert, sieht wie das Ende aus.
    Dann still_min erhoehen (siehe README)."""
    p, _ende = waschgang(pause_s=400)
    assert len(fertig.auswerten(p, p[-1][0], WASCH).laeufe) == 2
    laenger = fertigmeldung.maschinen_aus_umgebung({"WASCHMASCHINE_STILL_MIN": "8"})["waschmaschine"]
    assert laenger.still_min == 8 and len(fertig.auswerten(p, p[-1][0], laenger).laeufe) == 1


def test_unbekannte_werte_sind_keine_ruhe():
    """Der Sender erreicht den Shelly 10 min nicht (online false -> None): das ist keine Ruhe."""
    p, ende = waschgang()
    t0 = START + 1800
    luecke = sorted([(t, w) for t, w in p if not t0 <= t < t0 + 600] + [(t0 + 30.0 * i, None) for i in range(20)])
    assert [l.ende for l in fertig.auswerten(luecke, luecke[-1][0], WASCH).laeufe] == [ende + 240]


def nutzlast(w, online=True):
    return json.dumps({"power": w, "energy": 1234, "online": online}).encode()


def test_melder_meldet_einmal_und_haelt_den_zustand(tmp_path):
    gesendet = []
    datei = str(tmp_path / "verlauf.json")
    melder = fertigmeldung.Melder(fertigmeldung.MASCHINEN, datei, lambda t, n, r: gesendet.append((t, n, r)))
    p, ende = waschgang()
    for t, w in p:
        melder.wert("waschmaschine", nutzlast(w), jetzt=t)
    fertige = [n for t, n, r in gesendet if t == "fertig"]
    assert fertige == [{"maschine": "waschmaschine", "text": "Die Waschmaschine ist fertig.", "zeit": round(ende + 240)}]
    zustaende = [n["zustand"] for t, n, r in gesendet if t == "zustand/waschmaschine"]
    assert zustaende == ["bereit", "laeuft", "fertig"]
    assert all(r for t, n, r in gesendet if t.startswith("zustand/"))

    # Neustart: Verlauf und Gemeldetes kommen aus der Datei, nichts wird doppelt gemeldet
    gesendet.clear()
    neu = fertigmeldung.Melder(fertigmeldung.MASCHINEN, datei, lambda t, n, r: gesendet.append((t, n, r)))
    neu.wert("waschmaschine", nutzlast(0.4), jetzt=p[-1][0] + 300)
    assert [t for t, n, r in gesendet if t == "fertig"] == []

    # 20 min nichts mehr: nicht erreichbar
    neu.pruefen_alle(jetzt=p[-1][0] + 300 + 20 * 60)
    assert ("zustand/waschmaschine", {"zustand": "nicht_erreichbar", "seit": None, "leistung": 0.4}, True) in gesendet
    assert ("zustand/trockner", {"zustand": "nicht_erreichbar", "seit": None, "leistung": None}, True) in gesendet


def test_melder_offline_wert(tmp_path):
    gesendet = []
    melder = fertigmeldung.Melder(fertigmeldung.MASCHINEN, None, lambda t, n, r: gesendet.append((t, n, r)))
    melder.wert("trockner", nutzlast(0, online=False), jetzt=START)
    assert gesendet[-1][1]["zustand"] == "nicht_erreichbar"
    melder.wert("trockner", b"kein json", jetzt=START + 30)
    melder.wert("unbekannt", nutzlast(5), jetzt=START + 60)
    assert len(gesendet) == 1
