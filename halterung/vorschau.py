"""Vorschau Rev. 4: python3 vorschau.py  (braucht Koerper1.stl im selben Ordner, dazu matplotlib)
Baut die drei STL neu, schreibt c6l_halterung_wohn_vorschau.png, c6l_halterung_wasc_vorschau.png und
c6l_halterung_schnitt.png und prueft, dass sich Buegel, Stecker, C6L und Platte nicht durchdringen."""
import numpy as np, trimesh, matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from mpl_toolkits.mplot3d.art3d import Poly3DCollection
import c6l_halterung as h
from shapely.geometry import box

T = trimesh.transformations
MX, ZO = h.MITTE_X, h.Z_OBEN
light = np.array([0.3, -0.5, 0.8]); light /= np.linalg.norm(light)
# Buegel aus der Drucklage (X = y ab Steckermitte, Y = z ab Platte, Z = entlang des Steckers) in die Einbaulage
EINBAU = np.array([[0, 0, 1, 0], [1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 0, 1]], dtype=float)


def teile(hoeher):
    yg = h.Y0 + hoeher
    d = {}
    d['c6l'] = trimesh.creation.box(extents=[24, 56, 8], transform=T.translation_matrix([MX, yg + 28, ZO + 4]))
    sma = []
    for dx in (-6, 6):
        s = trimesh.creation.cylinder(radius=3.2, height=6, sections=24)
        s.apply_transform(T.rotation_matrix(np.pi / 2, [1, 0, 0])); s.apply_translation([MX + dx, yg + 59, ZO + 4]); sma.append(s)
    d['sma'] = sma
    # Stecker des Netzteils: 12 breit, Oberkante 8 mm ueber der Platte, etwa 6,5 dick, starrer Teil etwa 20 lang.
    # Kanten mit 0,5 mm gerundet (das Original ist runder): der Buegel liegt seitlich ohne Luft an.
    q = box(-h.STECKER_B / 2, -3.25, h.STECKER_B / 2, 3.25).buffer(-0.5).buffer(0.5, resolution=8)
    st = trimesh.creation.extrude_polygon(q, 20)
    st.apply_transform(EINBAU)
    st.apply_translation([MX + 12.5, yg + h.USB_Y, ZO + h.STECKER_OBEN - 3.25])
    d['usb'] = st
    d['grove'] = trimesh.creation.box(extents=[10, 8, 5.0], transform=T.translation_matrix([MX, yg - 4, ZO + 4]))
    kabel = trimesh.creation.cylinder(radius=1.9, height=max(hoeher - 8, 0.1) + 6, sections=16)
    kabel.apply_transform(T.rotation_matrix(np.pi / 2, [1, 0, 0])); kabel.apply_translation([MX, yg - 8 - (max(hoeher - 8, 0.1) + 6) / 2, ZO + 1.9])
    d['kabel'] = kabel
    b = trimesh.load('usb_buegel.stl'); b.apply_transform(EINBAU)
    b.apply_translation([MX + h.BUEGEL_X - h.BUEGEL_L / 2, yg + h.USB_Y, ZO])
    d['buegel'] = b
    k = [trimesh.creation.cylinder(radius=3.8, height=2.6, sections=32, transform=T.translation_matrix([MX + s * 8, yg + 12, ZO + 8 + 1.3])) for s in (-1, 1)]
    k += [trimesh.creation.cylinder(radius=3.8, height=2.6, sections=32, transform=T.translation_matrix([MX + h.BUEGEL_X, yg + h.USB_Y + s * h.SCHRAUBE_Y, ZO + h.FUSS_H + 1.3])) for s in (-1, 1)]
    d['koepfe'] = k
    d['psu'] = trimesh.creation.box(extents=[45.5, 45.6, 27.75], transform=T.translation_matrix([MX, 0, -8 - 27.75 / 2]))
    return d


def add(lst, mesh, col):
    v, f = trimesh.remesh.subdivide_to_size(mesh.vertices, mesh.faces, max_edge=2.5)
    mesh = trimesh.Trimesh(v, f, process=False)
    sh = 0.35 + 0.65 * np.clip(mesh.face_normals @ light, 0, 1)
    lst.append((mesh.triangles, np.c_[sh * col[0], sh * col[1], sh * col[2], np.ones(len(sh))]))


def zeige(ax, lst, lo, hi, e, a, titel):
    ax.add_collection3d(Poly3DCollection(np.concatenate([t for t, _ in lst]), facecolors=np.concatenate([c for _, c in lst]), edgecolor='none'))
    c = (lo + hi) / 2; r = (hi - lo).max() / 2
    ax.set_xlim(c[0] - r, c[0] + r); ax.set_ylim(c[1] - r, c[1] + r); ax.set_zlim(c[2] - r, c[2] + r)
    ax.view_init(e, a); ax.set_title(titel, fontsize=14); ax.set_axis_off()


def pruefe(name, m, d):
    def vol(a, b):
        s = trimesh.boolean.intersection([a, b], engine='manifold')
        return 0.0 if s.is_empty else abs(s.volume)
    w = {'Buegel/Platte': vol(d['buegel'], m), 'Buegel/C6L': vol(d['buegel'], d['c6l']), 'Buegel/Stecker': vol(d['buegel'], d['usb']),
         'C6L/Platte': vol(d['c6l'], m), 'Stecker/Platte': vol(d['usb'], m)}
    print(name, 'Durchdringung in mm3:', {k: round(v, 3) for k, v in w.items()})
    assert all(v < 0.05 for v in w.values()), name + ': Teile durchdringen sich'


BLAU, ORANGE = (0.35, 0.55, 0.9), (1.0, 0.6, 0.2)
basis = h.unterteil()
h.buegel('usb_buegel.stl')
modelle = {}
for kurz, hoeher, titel in (('wohn', 0.0, 'WOHN'), ('wasc', h.WASC_HOEHER, 'WASC')):
    m = modelle[kurz] = h.platte(f'c6l_halterung_{kurz}.stl', hoeher, hoeher > 0, basis)   # baut neu: im Speicher ist der Koerper geschlossen
    d = teile(hoeher)
    pruefe(kurz, m, d)
    oben = h.Y0 + hoeher + 62
    lo, hi = np.array([-34, -36, -38]), np.array([32, max(36, oben), 14])
    fig = plt.figure(figsize=(16, 13))
    for i, (e, a, t, k) in enumerate(((40, -60, f'{titel}: Platte, Einsatzloecher oben' + (', Tunnel unten' if hoeher else ''), 'h'),
                                      (50, -65, 'mit C6L, Stecker und Buegel (gruen), 4 Schrauben M4x12', 'c'),
                                      (22, -40, 'von rechts, mit Netzteil (grau)', 'p'),
                                      (28, -60, 'Druck: Platte aufs Bett, ohne Stuetze', 'd'))):
        ax = fig.add_subplot(2, 2, i + 1, projection='3d')
        lst = []
        if k == 'd':
            g = m.copy(); g.apply_transform(T.rotation_matrix(np.pi, [1, 0, 0]))
            add(lst, g, BLAU)
            zeige(ax, lst, np.array([-34, -max(36, oben), -2]), np.array([32, 36, 40]), e, a, t)
            continue
        add(lst, m, BLAU)
        if k in 'cp':
            add(lst, d['c6l'], ORANGE)
            for s in d['sma']: add(lst, s, (0.85, 0.75, 0.3))
            add(lst, d['usb'], (0.25, 0.25, 0.25)); add(lst, d['grove'], (0.95, 0.95, 0.95))
            if hoeher: add(lst, d['kabel'], (0.2, 0.2, 0.2))
            add(lst, d['buegel'], (0.3, 0.75, 0.4))
            for kk in d['koepfe']: add(lst, kk, (0.6, 0.6, 0.65))
        if k == 'p':
            add(lst, d['psu'], (0.6, 0.6, 0.6))
        zeige(ax, lst, lo, hi, e, a, t)
    plt.tight_layout(); plt.savefig(f'c6l_halterung_{kurz}_vorschau.png', dpi=80); plt.close(fig)

# Schnitte: quer durch den Buegel (beide Modelle gleich) und durch den Tunnel (nur WASC)
wasc = modelle['wasc']
d = teile(h.WASC_HOEHER)
yg = h.Y0 + h.WASC_HOEHER
fig = plt.figure(figsize=(18, 11))
ax = fig.add_subplot(2, 2, 1)
x = MX + h.BUEGEL_X
for mesh, farbe in ((wasc, 'b-'), (d['buegel'], 'g-')):
    s = mesh.section(plane_origin=[x, 0, 0], plane_normal=[1, 0, 0])
    for p in s.discrete: ax.plot(p[:, 1], p[:, 2], farbe)
ax.add_patch(plt.Rectangle((yg + h.USB_Y - 6, ZO + h.STECKER_OBEN - 6.5), 12, 6.5, color='k', alpha=0.45))
ax.set_xlim(yg + h.USB_Y - 26, yg + h.USB_Y + 26); ax.set_ylim(-10, 12)
ax.set_aspect('equal'); ax.grid(alpha=0.3); ax.set_title('Schnitt durch Buegel (gruen), Stecker (grau) und die zwei Einsatzloecher')
ax = fig.add_subplot(2, 2, 2, projection='3d')
lst = []
add(lst, trimesh.load('usb_buegel.stl'), (0.3, 0.75, 0.4))
zeige(ax, lst, np.array([-22, -6, -2]), np.array([22, 16, 14]), 35, -60, 'Buegel, so liegt er auf dem Bett (41 x 11,2 mm, 12 mm hoch)')
ax = fig.add_subplot(2, 2, 3)
s = wasc.section(plane_origin=[0, h.Y0 + h.TUNNEL_Y, 0], plane_normal=[0, 1, 0])
for p in s.discrete: ax.plot(p[:, 0], p[:, 2], 'b-')
ax.add_patch(plt.Circle((MX, ZO + 1.9), 1.9, color='k', alpha=0.45))
ax.set_xlim(MX - 20, MX + 20); ax.set_ylim(-12, 4); ax.set_aspect('equal'); ax.grid(alpha=0.3)
ax.set_title('WASC: Schnitt quer durch den Tunnel, Kabel (grau) liegt auf dem Steg')
ax = fig.add_subplot(2, 2, 4)
s = wasc.section(plane_origin=[MX, 0, 0], plane_normal=[1, 0, 0])
for p in s.discrete: ax.plot(p[:, 1], p[:, 2], 'b-')
ax.add_patch(plt.Rectangle((yg, ZO), 56, 8, color='orange', alpha=0.4))
ax.add_patch(plt.Rectangle((yg - 8, ZO + 1.5), 8, 5, color='0.8'))
ax.set_xlim(h.Y0 - 4, yg + 60); ax.set_ylim(-12, 10); ax.set_aspect('equal'); ax.grid(alpha=0.3)
ax.set_title('WASC: Laengsschnitt in der Mitte, C6L (orange) 20 mm hoeher, Grove-Stecker (hell)')
plt.tight_layout(); plt.savefig('c6l_halterung_schnitt.png', dpi=80)
