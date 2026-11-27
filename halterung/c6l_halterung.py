#!/usr/bin/env python3
"""Halterung Rev. 4: M5Stack Unit C6L auf dem offiziellen Raspberry-Pi-USB-C-Netzteil (EU, 15,3 W).

Zwei Platten und ein Buegel:

  c6l_halterung_wohn.stl   Gateway am Fenster. Wie Rev. 3 (Platte mit zwei Gewindeeinsaetzen M4 short unter
                           den Lego-Loechern des C6L), dazu rechts zwei weitere Einsaetze fuer den Buegel.
  c6l_halterung_wasc.stl   Sender in der Waschkueche. Der C6L sitzt 20 mm hoeher, darunter bleibt Platz fuer
                           den Grove-Stecker und das Kabel zum Beschleunigungssensor. Quer unter dem Kabel
                           laeuft ein Tunnel fuer einen Kabelbinder 3 x 1,5 mm durch die Platte.
  usb_buegel.stl           U-Halter ueber dem USB-C-Stecker des Netzteils (12 mm breit, Oberkante auf Hoehe
                           des C6L). Zwei Fuesse, von oben mit M4x12 in die Einsaetze geschraubt. Die Fuesse
                           sind so hoch wie der C6L (8 mm), es sind also dieselben Schrauben. Langloecher
                           +-1 mm, damit der Buegel genau ueber den Stecker rueckt. Passt auf beide Platten.

Unterteil (vier Eckbeine mit Kragen, das Kabel wird darum gewickelt) aus
"Raspberry pi 4 b mount for the orginal power supply" (printables.com/model/578212, Datei Koerper1.stl).

Druck: Platten flach aufs Bett (C6L-Seite unten), Beine nach oben, ohne Stuetze. Der Tunnel liegt dabei
offen auf dem Bett und schliesst sich als Bogen. Der Buegel liegt in der STL schon auf der Seite, so wie er
gedruckt wird (U-Profil auf dem Bett, ohne Stuetze).

    pip install trimesh manifold3d shapely numpy
    python3 c6l_halterung.py Koerper1.stl      -> c6l_halterung_wohn.stl, c6l_halterung_wasc.stl, usb_buegel.stl
"""
import sys
import numpy as np
import trimesh
from shapely.geometry import box, Point
from shapely.ops import unary_union

VORLAGE = sys.argv[1] if len(sys.argv) > 1 else "Koerper1.stl"

# ---- C6L (aus dem M5-Gehaeusemodell Unit_C6L.stl und einem Foto) ----
C6L_L, C6L_B, C6L_H = 56.0, 24.0, 8.0
LEGO_Y = 12.0                             # Lochmitte ab Grove-Ende
LEGO_X = 8.0                              # je Seite ab Mitte (16 mm Abstand)
USB_Y = 21.5                              # Mitte der USB-C-Buchse ab Grove-Ende (Oeffnung 17,3 bis 25,7), rechte Seite

# ---- Gewindeeinsatz M4 short (CNC Kitchen: Bohrung 5,6, Sackloch min. 5 tief, Wand min. 2,1) ----
INSERT_D = 5.6 + 0.2                      # +0,2 mm Zugabe auf die Insert-Loecher (am Drucker ausprobiert)
INSERT_T = 5.0                            # Sacklochtiefe
INSERT_FASE = 0.5                         # 45 Grad am Lochrand

# ---- Platte ----
PLATTE_D = 6.0                            # 5 mm Sackloch + 1 mm Boden (Schraubenspitze bleibt vom Netzteil weg)
PLATTE_B = 54.0                           # quer, deckt die Beine (50,5) ab
ECKE_R = 4.0
WASC_HOEHER = 20.0                        # Sender: C6L so viel weiter oben

# ---- Buegel ueber dem USB-C-Stecker (Stecker 12 mm breit, Oberkante etwa 8 mm = wie der C6L) ----
STECKER_B = 12.0
STECKER_OBEN = 8.0
BUEGEL_LUFT_B = 0.0                       # in der Breite keine Luft: der Buegel soll den Stecker klemmen
BUEGEL_LUFT_H = 0.2                       # nach oben 0,2 mm, damit er den Stecker nicht in die Buchse hebelt
BUEGEL_INNEN_R = 0.4                      # Innenecken oben, kleiner als die Kantenrundung des Steckers
BUEGEL_WAND = 3.0
BUEGEL_L = 12.0                           # entlang des Steckers
BUEGEL_AB = 1.5                           # Abstand zur Seitenwand des C6L
FUSS_H = C6L_H                            # gleiche Klemmhoehe wie der C6L: M4x12 greift 4 mm in den Einsatz
SCHRAUBE_Y = 15.0                         # Schraubenachse ab Steckermitte
FUSS_ENDE = 20.5                          # Fussende ab Steckermitte
LANGLOCH_D = 4.4
LANGLOCH_SPIEL = 1.0                      # +- in Laengsrichtung des C6L
BUEGEL_X = C6L_B / 2 + BUEGEL_AB + BUEGEL_L / 2       # Mitte des Buegels ab Mitte C6L

# ---- Tunnel fuer den Kabelbinder (3 x 1,5 mm) unter dem Grove-Kabel, nur Sender ----
TUNNEL_B = 4.0                            # in Kabelrichtung (Binder 3 mm breit)
TUNNEL_D = 2.3                            # Hoehe des Kanals (Binder 1,5 mm dick)
TUNNEL_BODEN = 1.0                        # Rest unter dem Kanal bis zum Netzteil
TUNNEL_MITTE_H = 4.0                      # Mittelpunkt des Bogens so weit ueber der Platte: flacher Einlauf
TUNNEL_Y = 6.0                            # Mitte ab Unterkante der Platte (14 mm unter dem Grove-Ende)

# ---- Vorlage ----
# Plattenunterseite z = -8, Beine darunter bis z = -36,25, Netzteilmitte bei x = -2,05, y = 0.
# Kragen am Beinende z -35,0..-36,25, 5 mm nach aussen.
Z_UNTEN = -8.0
MITTE_X, MITTE_Y = -2.05, 0.0
KRAGEN_Z = -35.0
KRAGEN_B = 5.0
Z_OBEN = Z_UNTEN + PLATTE_D
Y0 = MITTE_Y - C6L_L / 2                  # Unterkante der Platte (bei WOHN zugleich das Grove-Ende)
T = trimesh.transformations


def prisma(poly2d, z, h):
    m = trimesh.creation.extrude_polygon(poly2d, h)
    m.apply_translation([0, 0, z])
    return m


def querschnitt(mesh, z):
    s = mesh.section(plane_origin=[0, 0, z], plane_normal=[0, 0, 1])
    polys = trimesh.path.polygons.paths_to_polygons([d[:, :2] for d in s.discrete])
    return unary_union([p for p in polys if p is not None])


def insertloch(x, y):
    """Sackloch fuer einen Einsatz M4 short von oben, mit Fase am Rand."""
    r = INSERT_D / 2
    profil = [(0, -INSERT_T), (r, -INSERT_T), (r, -INSERT_FASE), (r + INSERT_FASE + 0.3, 0.3), (0, 0.3)]
    l = trimesh.creation.revolve(profil, sections=64)
    l.apply_translation([x, y, Z_OBEN])
    return l


def unterteil():
    """Beine der Vorlage und die 45-Grad-Schraege unter dem Kragen (im Druck liegt der Kragen oben)."""
    vorlage = trimesh.load(VORLAGE)
    unten = trimesh.creation.box(extents=[200, 200, 50])                 # alles unter z = -8 (nur Beine)
    unten.apply_translation([0, 0, Z_UNTEN - 0.05 - 25])
    scheibe = trimesh.creation.box(extents=[52.0, 52.0, 0.4])            # 0,3 mm in die neue Platte, nur ueber den Beinen
    scheibe.apply_translation([MITTE_X, MITTE_Y, Z_UNTEN + 0.1])
    schnitt = trimesh.boolean.union([unten, scheibe], engine="manifold")
    beine = trimesh.boolean.intersection([vorlage, schnitt], engine="manifold")
    bein = querschnitt(vorlage, KRAGEN_Z + 1.0)
    kragen = querschnitt(vorlage, KRAGEN_Z - 0.25)
    schraege = []
    stufe = 0.2
    for i in range(int(KRAGEN_B / stufe)):
        h = i * stufe                                                    # Abstand ueber dem Kragen
        poly = kragen.intersection(bein.buffer(KRAGEN_B - h - stufe / 2, join_style="mitre"))
        for teil in getattr(poly, "geoms", [poly]):
            if teil.geom_type == "Polygon" and teil.area > 0.01:
                schraege.append(prisma(teil, KRAGEN_Z + h - 0.01, stufe + 0.02))
    return [beine] + schraege


def tunnel(y_mitte):
    """Bogenfoermiger Kanal quer unter dem Kabel: beide Enden kommen schraeg an der Oberseite heraus."""
    ra = TUNNEL_MITTE_H + PLATTE_D - TUNNEL_BODEN                        # aeussere Wand, tiefster Punkt 1 mm ueber dem Boden
    rb = ra - TUNNEL_D
    mitte = Point(MITTE_X, Z_OBEN + TUNNEL_MITTE_H)
    ring = mitte.buffer(ra, resolution=48).difference(mitte.buffer(rb, resolution=48))
    ring = ring.intersection(box(MITTE_X - ra - 1, Z_UNTEN, MITTE_X + ra + 1, Z_OBEN + 0.3))
    m = trimesh.creation.extrude_polygon(ring, TUNNEL_B)                 # Profil in (x, z), Laenge entlang y
    m.apply_transform(T.rotation_matrix(np.pi / 2, [1, 0, 0]))           # (x, v, w) -> (x, -w, v)
    m.apply_translation([0, y_mitte + TUNNEL_B / 2, 0])
    aussen = np.sqrt(ra ** 2 - TUNNEL_MITTE_H ** 2)
    innen = np.sqrt(rb ** 2 - TUNNEL_MITTE_H ** 2)
    return m, innen, aussen, rb - TUNNEL_MITTE_H


def platte(name, hoeher, mit_tunnel, basis):
    """hoeher = Abstand des Grove-Endes von der Unterkante der Platte."""
    y_grove = Y0 + hoeher
    umriss = box(MITTE_X - PLATTE_B / 2, Y0, MITTE_X + PLATTE_B / 2, y_grove + C6L_L + 1.0)
    umriss = umriss.buffer(-ECKE_R).buffer(ECKE_R, resolution=16)
    teile = basis + [prisma(umriss, Z_UNTEN, PLATTE_D)]
    for t in teile:
        t.fix_normals()
    koerper = trimesh.boolean.union(teile, engine="manifold")
    punkte = [(MITTE_X - LEGO_X, y_grove + LEGO_Y), (MITTE_X + LEGO_X, y_grove + LEGO_Y),
              (MITTE_X + BUEGEL_X, y_grove + USB_Y - SCHRAUBE_Y), (MITTE_X + BUEGEL_X, y_grove + USB_Y + SCHRAUBE_Y)]
    ab = [insertloch(x, y) for x, y in punkte]
    text = ""
    if mit_tunnel:
        t, innen, aussen, steg = tunnel(Y0 + TUNNEL_Y)
        ab.append(t)
        text = (f", Tunnel bei y {Y0 + TUNNEL_Y:.1f}: Oeffnungen {innen:.1f} bis {aussen:.1f} mm neben der Mitte, "
                f"Steg unter dem Kabel {2 * innen:.1f} mm breit und {steg:.1f} mm dick")
    ergebnis = trimesh.boolean.difference([koerper] + ab, engine="manifold")
    assert ergebnis.is_watertight and len(ergebnis.split(only_watertight=False)) == 1, name + ": nicht ein geschlossener Koerper"
    ergebnis.export(name)
    b = ergebnis.bounds
    print(f"{name}: {b[1][0]-b[0][0]:.1f} x {b[1][1]-b[0][1]:.1f} x {b[1][2]-b[0][2]:.1f} mm, {ergebnis.volume/1000:.1f} cm3, "
          f"Einsaetze bei " + ", ".join(f"({x:.2f} / {y:.2f})" for x, y in punkte) + text)
    return ergebnis


def buegel(name):
    """U-Profil in (y, z): y ab Steckermitte, z ab Plattenoberseite. In der STL liegt das Profil auf dem Bett."""
    bi = (STECKER_B + BUEGEL_LUFT_B) / 2
    hi = STECKER_OBEN + BUEGEL_LUFT_H
    voll = unary_union([box(-FUSS_ENDE, -3, FUSS_ENDE, FUSS_H), box(-bi - BUEGEL_WAND, -3, bi + BUEGEL_WAND, hi + BUEGEL_WAND)])
    voll = voll.buffer(-1.0).buffer(1.0, resolution=16)                  # Aussenkanten runden
    voll = voll.buffer(0.8, resolution=16).buffer(-0.8, resolution=16)   # Kehle zwischen Fuss und Wand (Kerbwirkung)
    prof = voll.difference(box(-bi, -4, bi, hi))
    prof = prof.buffer(BUEGEL_INNEN_R, resolution=16).buffer(-BUEGEL_INNEN_R, resolution=16)   # Innenecken oben
    prof = prof.intersection(box(-FUSS_ENDE - 1, 0, FUSS_ENDE + 1, hi + BUEGEL_WAND + 1))   # Standflaeche bleibt scharfkantig
    koerper = trimesh.creation.extrude_polygon(prof, BUEGEL_L)
    ab = []
    for s in (-1, 1):                                                    # Langloch: Achse entlang z des Profils (im Druck: y)
        pkt = []
        for d in (-LANGLOCH_SPIEL, LANGLOCH_SPIEL):
            z = trimesh.creation.cylinder(radius=LANGLOCH_D / 2, height=FUSS_H + 2, sections=48)
            z.apply_transform(T.rotation_matrix(np.pi / 2, [1, 0, 0]))
            z.apply_translation([s * SCHRAUBE_Y + d, FUSS_H / 2, BUEGEL_L / 2])
            pkt.append(z.vertices)
        ab.append(trimesh.convex.convex_hull(np.vstack(pkt)))
    ergebnis = trimesh.boolean.difference([koerper] + ab, engine="manifold")
    assert ergebnis.is_watertight and len(ergebnis.split(only_watertight=False)) == 1, name + ": nicht ein geschlossener Koerper"
    ergebnis.export(name)
    b = ergebnis.bounds
    print(f"{name}: {b[1][0]-b[0][0]:.1f} x {b[1][1]-b[0][1]:.1f} x {b[1][2]-b[0][2]:.1f} mm (so liegt er auf dem Bett), "
          f"{ergebnis.volume/1000:.2f} cm3, innen {2 * bi:.1f} breit und {hi:.1f} hoch, Fuesse {FUSS_H:.1f} hoch, "
          f"Schrauben {2 * SCHRAUBE_Y:.0f} mm auseinander")
    return ergebnis


if __name__ == "__main__":
    basis = unterteil()
    platte("c6l_halterung_wohn.stl", 0.0, False, basis)
    platte("c6l_halterung_wasc.stl", WASC_HOEHER, True, basis)
    buegel("usb_buegel.stl")
