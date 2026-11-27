"""Erkennt aus dem Leistungsverlauf einer Maschine, wann ein Lauf fertig ist. Reine Funktionen, ohne MQTT.

Regel: Ein Lauf ist fertig, wenn die Maschine vorher gearbeitet hat (Mittel ueber arbeit_min Minuten ueber
arbeit_w) und die Leistung danach still_min Minuten durchgehend unter still_w blieb. Kurze Pausen mitten im
Programm (Einweichen, Trommel steht) loesen so nichts aus, solange sie kuerzer als still_min sind.

Die Werte kommen ueber Funk: solange eine Maschine laeuft alle 30 s, sonst alle 5 min, bei jedem Wechsel ueber
oder unter die Schwelle des Senders (5 W) sofort. Weil die Strecke ausfallen kann, gilt:

- Nichts gilt ueber den letzten Wert hinaus: fertig ist ein Lauf erst, wenn eine Sendung nach Ablauf der Ruhezeit
  die Ruhe bestaetigt (der Aufrufer uebergibt als "jetzt" die Zeit des letzten Werts). Faellt der Funk mitten im
  Programm aus, meldet deshalb niemand "fertig".
- Erreicht der Sender den Shelly nicht (online false), ist der Wert unbekannt (None): er zaehlt weder als Leistung
  noch als Ruhe, die Ruhezeit beginnt danach neu.

Die Auswertung ist eine reine Funktion ueber den Verlauf (Zeit, Watt): dieselben Daten ergeben dieselbe Meldung
mit derselben Kennung, auch nach einem Neustart.
"""
from __future__ import annotations

import bisect
import math
from dataclasses import dataclass, field

FERTIG_ZEIGEN_S = 3 * 3600  # so lange heisst der Zustand nach dem Ende "fertig", danach "bereit"
VERWERFEN_S = 30 * 60      # ein Anlauf ohne Arbeit gilt nach 30 min Ruhe nicht mehr
AUS_S = 60                 # so lange durchgehend unter aus_w: aus; einzelne 0-W-Messwerte zaehlen nicht


@dataclass(frozen=True)
class Maschine:
    """Eine Maschine mit ihren Schwellen. Die Vorgaben sind ein erster Ansatz und muessen an den eigenen
    Maschinen nachgestellt werden (siehe README, Abschnitt Fertig-Meldung)."""
    schluessel: str             # "waschmaschine", wie im MQTT-Thema sensors/waschkueche/<schluessel>
    name: str                   # "Waschmaschine"
    text: str                   # "Die Waschmaschine ist fertig."
    arbeit_w: float = 20.0      # Mittel ueber arbeit_min Minuten darueber: die Maschine arbeitet
    arbeit_min: float = 5.0
    still_w: float = 5.0        # danach still_min Minuten durchgehend darunter: fertig
    still_min: float = 4.0
    aus_w: float = 0.0          # am Ende AUS_S lang darunter: abgebrochen (Stecker gezogen), keine Meldung; 0 = nie
    stumm_min: float = 15.0     # so lange kein Wert: nicht erreichbar


@dataclass
class Lauf:
    start: float                     # erster Wert ab still_w
    arbeit_ab: float | None = None   # Beginn des ersten Fensters, dessen Mittel ueber arbeit_w lag
    ende: float | None = None        # fertig (oder abgebrochen) um
    fertig: bool = False             # True: fertig; False mit ende: abgebrochen
    spitze: float = 0.0              # hoechste Leistung im Lauf


@dataclass
class Auswertung:
    laeufe: list[Lauf] = field(default_factory=list)   # abgeschlossene Laeufe (fertig oder abgebrochen)
    offen: Lauf | None = None                          # laufender Anlauf (noch nicht fertig)
    zuletzt_warm: float | None = None                  # Ende der letzten Leistung ab still_w
    wert: float | None = None                          # zuletzt gemeldete Leistung
    seit: float | None = None                          # Zeit der letzten Meldung
    null_seit: float | None = None                     # seit dann durchgehend unter aus_w (None: zuletzt darueber)


def _sauber(proben) -> list[tuple[float, float | None]]:
    """Nach Zeit sortiert, nur endliche Zahlen oder None (unbekannt); gleiche Zeit: die spaetere Meldung gilt."""
    d: dict[float, float | None] = {}
    for t, w in proben:
        try:
            t = float(t)
            w = None if w is None else float(w)
        except (TypeError, ValueError):
            continue
        if math.isfinite(t) and (w is None or math.isfinite(w)):
            d[t] = None if w is None else max(0.0, w)
    return sorted(d.items())


class _Energie:
    """Energie (W*s) des stueckweise konstanten Verlaufs bis zu einem Zeitpunkt; vor der ersten Meldung 0."""

    def __init__(self, proben: list[tuple[float, float | None]]):
        self.t = [t for t, _w in proben]
        self.w = [w or 0.0 for _t, w in proben]      # unbekannt zaehlt nicht als Leistung
        self.summe = [0.0]
        for i in range(1, len(proben)):
            self.summe.append(self.summe[-1] + self.w[i - 1] * (self.t[i] - self.t[i - 1]))

    def bis(self, x: float) -> float:
        i = bisect.bisect_right(self.t, x) - 1
        if i < 0:
            return 0.0
        return self.summe[i] + self.w[i] * (x - self.t[i])

    def mittel(self, von: float, bis: float) -> float:
        return (self.bis(bis) - self.bis(von)) / max(1e-9, bis - von)


def auswerten(proben, jetzt: float, m: Maschine) -> Auswertung:
    """Laeufe aus dem Verlauf [(Zeit, Watt)] bis jetzt (der letzte Wert gilt bis jetzt). Watt None = unbekannt
    (Shelly nicht erreichbar): weder Leistung noch Ruhe."""
    p = [(t, w) for t, w in _sauber(proben) if t <= jetzt]
    a = Auswertung()
    if not p:
        return a
    a.wert, a.seit = p[-1][1], p[-1][0]
    energie = _Energie(p)
    fenster = m.arbeit_min * 60.0
    still = m.still_min * 60.0
    for i, (t, w) in enumerate(p):
        ende = p[i + 1][0] if i + 1 < len(p) else max(t, jetzt)
        if w is None:
            a.null_seit = None
            if a.offen is not None:
                a.zuletzt_warm = ende       # die Ruhezeit (und die Frist zum Verwerfen) beginnt danach neu
            continue
        if w >= m.aus_w:
            a.null_seit = None
        elif a.null_seit is None:
            a.null_seit = t
        if w >= m.still_w:
            if a.offen is None:
                a.offen = Lauf(start=t)
            a.offen.spitze = max(a.offen.spitze, w)
            a.zuletzt_warm = ende
            if a.offen.arbeit_ab is None:
                # Mittel der arbeit_min Minuten vor jeder Stelle, an der es steigen kann (Ende eines warmen
                # Stuecks) oder ein kaltes Stueck hinten aus dem Fenster faellt (Beginn + Fensterlaenge)
                for x in (ende, *(s + fenster for s, _ in p[max(0, i - 120):i + 1] if t <= s + fenster <= ende)):
                    if x - fenster >= p[0][0] - 1e-9 and energie.mittel(x - fenster, x) > m.arbeit_w:
                        a.offen.arbeit_ab = max(a.offen.start, x - fenster)
                        break
            continue
        # unter still_w
        if a.offen is None or a.zuletzt_warm is None:
            continue
        if a.offen.arbeit_ab is not None:
            fertig_um = a.zuletzt_warm + still
            if fertig_um <= ende:
                a.offen.ende = fertig_um
                a.offen.fertig = a.null_seit is None or fertig_um - a.null_seit < AUS_S
                a.laeufe.append(a.offen)
                a.offen = None
        elif a.zuletzt_warm + VERWERFEN_S <= ende:
            a.offen = None          # Anlauf ohne Arbeit: kein Lauf
    return a


def kennung(m: Maschine, lauf: Lauf) -> str:
    return f"{m.schluessel}-{int(round(lauf.ende or 0))}"


def zustand(a: Auswertung, jetzt: float, m: Maschine) -> tuple[str, float | None]:
    """(zustand, seit): unbekannt (noch kein Wert), laeuft (seit Beginn der Arbeit), fertig (seit der Meldung,
    hoechstens FERTIG_ZEIGEN_S), aus (AUS_S lang unter aus_w) oder bereit."""
    if a.seit is None:
        return "unbekannt", None
    o = a.offen
    if o is not None and o.arbeit_ab is not None:
        return "laeuft", o.arbeit_ab
    aus = a.null_seit is not None and jetzt - a.null_seit >= AUS_S
    letzter = a.laeufe[-1] if a.laeufe else None
    if letzter is not None and letzter.fertig and jetzt - (letzter.ende or 0) <= FERTIG_ZEIGEN_S and not aus:
        return "fertig", letzter.ende
    if aus:
        return "aus", a.null_seit
    return "bereit", None
