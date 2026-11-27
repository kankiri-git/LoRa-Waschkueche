#!/usr/bin/env python3
"""Halterung fuer den M5Stack Unit Accel (U056, ADXL345) an der Seitenwand der Waschmaschine.

Rev. 2: grosse Klebeplatte 60 x 80 mm (48 cm2, das 2,8-fache von Rev. 1), 3 mm dick, damit sie sich an das
Blech anlegt. In der Mitte ein 6,6 mm dicker Sockel mit zwei Gewindeeinsaetzen M4 short (CNC Kitchen M4x4)
genau unter den beiden Lego-Loechern des Unit Accel (16 mm Abstand, 12 mm vom Grove-Ende). Der Sensor wird
mit zwei Schrauben M4x12 von vorn festgeschraubt, die Rueckseite der Platte klebt mit VHB-Band an der Maschine.
Ein niedriger Rand an drei Seiten haelt den Sensor gerade, das Grove-Ende bleibt frei.
Davor ein Steg mit Tunnel fuer einen Kabelbinder (2,5 mm) als Zugentlastung: das Kabel darf nicht
am Stecker ziehen und nicht frei am Sensor schwingen, sonst misst er das Kabel mit.

Masse des Sensors aus dem M5-Gehaeusemodell (Unit_Accel.stl): 24 x 32 x 8 mm, Lego-Loecher 5,0 mm,
Grove-Oeffnung 10,2 mm breit an der Stirnseite y = 0, Mitte 5,2 mm ueber dem Boden.

Druck: Klebeseite flach aufs Bett, ohne Stuetze (der Tunnel ist eine 3,4-mm-Bruecke).

    pip install trimesh manifold3d shapely numpy
    python3 accel_halterung.py            -> accel_halterung.stl
"""
import sys
import numpy as np
import trimesh
from shapely.geometry import box

AUSGABE = sys.argv[1] if len(sys.argv) > 1 else "accel_halterung.stl"

# ---- Unit Accel: Ecke am Grove-Ende bei (0, 0), Boden auf z = 0 ----
UNIT_B, UNIT_L, UNIT_H = 24.0, 32.0, 8.0
LEGO_Y = 12.0                             # Lochmitte ab Grove-Ende
LEGO_X = (4.0, 20.0)                      # 16 mm Abstand

# ---- Gewindeeinsatz M4 short (CNC Kitchen: Bohrung 5,6, Sackloch min. 5 tief) ----
INSERT_D = 5.6 + 0.2                      # +0,2 mm Zugabe auf die Insert-Loecher (am Drucker ausprobiert)
INSERT_T = 5.0                            # Sacklochtiefe
INSERT_FASE = 0.5                         # 45 Grad am Lochrand

# ---- Sockel unter dem Sensor ----
PLATTE_D = 6.6                            # 5 mm Sackloch + 1,6 mm Boden, damit die Klebeseite beim Einschmelzen eben bleibt
SPIEL = 0.3                               # Luft um den Sensor
RAND_B = 2.7                              # Randbreite
RAND_H = 3.0                              # Randhoehe (Sensor 8 mm, bleibt gut greifbar)
RAND_AB_Y = 2.0                           # Rand beginnt hinter dem Grove-Ende
ECKE_R = 3.0
FASE_UNTEN = 0.5                          # Klebeseite: Kante gebrochen (Elefantenfuss, Band steht nicht ueber)

# ---- Zugentlastung vor dem Grove-Ende ----
STEG_Y0, STEG_Y1 = -23.0, -16.0           # 16 mm Platz fuer Stecker und Kabelbogen
STEG_B = 12.0                             # quer, mittig
STEG_H = 4.4
TUNNEL_B, TUNNEL_H = 3.4, 1.8             # Kabelbinder 2,5 x 1 mm
RINNE_B, RINNE_T = 5.0, 1.0               # Kabel liegt oben in der Rinne, Mitte etwa auf Hoehe der Grove-Buchse
PLATTE_Y0 = -25.0

# ---- Klebeplatte (Flansch) rund um den Sockel ----
FLANSCH_B, FLANSCH_L = 60.0, 80.0         # quer x laengs, mittig um den Sockel
FLANSCH_D = 3.0                           # duenn genug, um sich an das Blech anzulegen
FLANSCH_R = 6.0
KEHLE = 2.0                               # 45-Grad-Schraege vom Flansch zum Sockel

X0, X1 = -SPIEL - RAND_B, UNIT_B + SPIEL + RAND_B
Y1 = UNIT_L + SPIEL + RAND_B
T = trimesh.transformations


def prisma(poly2d, z, h):
    m = trimesh.creation.extrude_polygon(poly2d, h)
    m.apply_translation([0, 0, z])
    return m


def quader(x0, x1, y0, y1, z0, z1):
    return trimesh.creation.box(extents=[x1 - x0, y1 - y0, z1 - z0],
                                transform=T.translation_matrix([(x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2]))


def zylinder(x, y, z0, z1, d, sections=64):
    return trimesh.creation.cylinder(radius=d / 2, height=z1 - z0, sections=sections,
                                     transform=T.translation_matrix([x, y, (z0 + z1) / 2]))


def kegel(x, y, z0, z1, d0, d1, sections=64):
    """Kegelstumpf von Durchmesser d0 (unten) auf d1 (oben)."""
    w = np.linspace(0, 2 * np.pi, sections, endpoint=False)
    p = [[x + d0 / 2 * np.cos(a), y + d0 / 2 * np.sin(a), z0] for a in w] + [[x + d1 / 2 * np.cos(a), y + d1 / 2 * np.sin(a), z1] for a in w]
    return trimesh.convex.convex_hull(np.array(p))


def huelle(poly_unten, z_unten, poly_oben, z_oben):
    """Konvexe Huelle zweier (konvexer) Umrisse auf verschiedenen Hoehen: Fase oder Schraege."""
    a, b = np.array(poly_unten.exterior.coords), np.array(poly_oben.exterior.coords)
    return trimesh.convex.convex_hull(np.vstack([np.c_[a, np.full(len(a), z_unten)], np.c_[b, np.full(len(b), z_oben)]]))


# Sockel: abgerundetes Rechteck unter Sensor und Steg
umriss = box(X0 + ECKE_R, PLATTE_Y0 + ECKE_R, X1 - ECKE_R, Y1 - ECKE_R).buffer(ECKE_R, resolution=24)
# Klebeplatte: groesseres abgerundetes Rechteck, mittig um den Sockel, unten mit Fase
fx, fy = (X0 + X1) / 2, (PLATTE_Y0 + Y1) / 2
flansch = box(fx - FLANSCH_B / 2 + FLANSCH_R, fy - FLANSCH_L / 2 + FLANSCH_R,
              fx + FLANSCH_B / 2 - FLANSCH_R, fy + FLANSCH_L / 2 - FLANSCH_R).buffer(FLANSCH_R, resolution=24)
z_flansch = -PLATTE_D + FLANSCH_D                                    # Oberseite der Klebeplatte
teile = [prisma(flansch, -PLATTE_D + FASE_UNTEN, FLANSCH_D - FASE_UNTEN),
         huelle(flansch.buffer(-FASE_UNTEN), -PLATTE_D, flansch, -PLATTE_D + FASE_UNTEN + 0.01),
         prisma(umriss, z_flansch - 0.01, -z_flansch + 0.01),
         huelle(umriss.buffer(KEHLE, resolution=24), z_flansch - 0.01, umriss, z_flansch + KEHLE)]

# Rand an beiden Laengsseiten und am hinteren Ende (U-Form), Aussenkontur wie die Platte
rand_aussen = umriss.intersection(box(X0, RAND_AB_Y, X1, Y1))
rand = rand_aussen.difference(box(-SPIEL, RAND_AB_Y - 1, UNIT_B + SPIEL, UNIT_L + SPIEL))
teile.append(prisma(rand, -0.01, RAND_H + 0.01))

# Steg fuer den Kabelbinder
mx = UNIT_B / 2
teile.append(quader(mx - STEG_B / 2, mx + STEG_B / 2, STEG_Y0, STEG_Y1, -0.01, STEG_H))
koerper = trimesh.boolean.union(teile, engine="manifold")

# Abzuege: Insert-Loecher mit Fase, Tunnel, Kabelrinne
ab = []
for x in LEGO_X:
    ab.append(zylinder(x, LEGO_Y, -INSERT_T, 0.1, INSERT_D))
    ab.append(kegel(x, LEGO_Y, -INSERT_FASE, 0.01, INSERT_D, INSERT_D + 2 * INSERT_FASE + 0.02))
ty = (STEG_Y0 + STEG_Y1) / 2
ab.append(quader(mx - STEG_B / 2 - 1, mx + STEG_B / 2 + 1, ty - TUNNEL_B / 2, ty + TUNNEL_B / 2, 0.0, TUNNEL_H))
rinne = trimesh.creation.cylinder(radius=RINNE_B / 2, height=STEG_Y1 - STEG_Y0 + 2, sections=48)
rinne.apply_transform(T.rotation_matrix(np.pi / 2, [1, 0, 0]))
rinne.apply_translation([mx, ty, STEG_H - RINNE_T + RINNE_B / 2])
ab.append(rinne)
koerper = trimesh.boolean.difference([koerper] + ab, engine="manifold")

assert koerper.is_watertight and koerper.is_volume, "Koerper ist nicht geschlossen"
koerper.export(AUSGABE)
g = koerper.bounds
print("%s: %.1f x %.1f x %.1f mm, %.1f cm3, Klebeflaeche %.1f cm2" % (
    AUSGABE, g[1][0] - g[0][0], g[1][1] - g[0][1], g[1][2] - g[0][2], koerper.volume / 1000,
    flansch.buffer(-FASE_UNTEN).area / 100))
