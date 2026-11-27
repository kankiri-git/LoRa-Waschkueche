"""Vorschau der Sensor-Halterung: python3 accel_vorschau.py accel_halterung.stl accel_halterung_vorschau.png"""
import sys, trimesh, numpy as np, matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from mpl_toolkits.mplot3d.art3d import Poly3DCollection
stl, out = sys.argv[1], sys.argv[2]
m = trimesh.load(stl)
T = trimesh.transformations.translation_matrix
R = trimesh.transformations.rotation_matrix
# Sensor als Quader mit zwei Befestigungsloechern, Grove-Stecker, Kabel, Schraubenkoepfe, Kabelbinder
unit = trimesh.creation.box(extents=[24, 32, 8], transform=T([12, 16, 4]))
loecher = [trimesh.creation.cylinder(radius=2.5, height=9, sections=32, transform=T([x, 12, 4])) for x in (4, 20)]
unit = trimesh.boolean.difference([unit] + loecher, engine='manifold')
koepfe = [trimesh.creation.cylinder(radius=3.5, height=2.4, sections=32, transform=T([x, 12, 8 + 1.2])) for x in (4, 20)]
stecker = trimesh.creation.box(extents=[10, 6, 4.6], transform=T([12, -3, 5.2]))
kabel = trimesh.creation.cylinder(radius=1.6, height=34, sections=20)
kabel.apply_transform(R(np.pi / 2, [1, 0, 0])); kabel.apply_translation([12, -23, 5.0])
binder = trimesh.creation.box(extents=[7.5, 2.5, 6.2], transform=T([12, -19.5, 3.6]))
light = np.array([0.3, -0.5, 0.8]); light /= np.linalg.norm(light)
def add(lst, mesh, col):
    v, f = trimesh.remesh.subdivide_to_size(mesh.vertices, mesh.faces, max_edge=2.0)
    mesh = trimesh.Trimesh(v, f, process=False)
    sh = 0.35 + 0.65 * np.clip(mesh.face_normals @ light, 0, 1)
    lst.append((mesh.triangles, np.c_[sh * col[0], sh * col[1], sh * col[2], np.ones(len(sh))]))
views = [(38, -55, 'Halterung Rev. 2: Klebeplatte 60 x 80 mm, Sockel mit Insert-Loechern', 'h'),
         (32, -55, 'mit Unit Accel, 2 Schrauben M4x12, Kabel am Steg festgebunden', 'u'),
         (30, 125, 'von der anderen Seite', 'u'),
         (-28, -55, 'Klebeseite (VHB), so liegt sie auf dem Druckbett', 'h')]
fig = plt.figure(figsize=(16, 13))
for i, (e, a, t, k) in enumerate(views):
    ax = fig.add_subplot(2, 2, i + 1, projection='3d')
    lst = []
    add(lst, m, (0.35, 0.55, 0.9))
    if k == 'u':
        add(lst, unit, (0.92, 0.92, 0.92)); add(lst, stecker, (0.98, 0.98, 0.95)); add(lst, kabel, (0.25, 0.25, 0.25))
        add(lst, binder, (0.1, 0.1, 0.1))
        for kk in koepfe: add(lst, kk, (0.6, 0.6, 0.65))
    ax.add_collection3d(Poly3DCollection(np.concatenate([t_ for t_, _ in lst]), facecolors=np.concatenate([c for _, c in lst]), edgecolor='none'))
    c = np.array([12, 5, 0]); r = 40
    ax.set_xlim(c[0] - r, c[0] + r); ax.set_ylim(c[1] - r, c[1] + r); ax.set_zlim(c[2] - r, c[2] + r)
    ax.view_init(e, a); ax.set_title(t, fontsize=14); ax.set_axis_off()
plt.tight_layout(); plt.savefig(out, dpi=80)
fig, axs = plt.subplots(1, 2, figsize=(15, 4.6))
for ax, (o, nrm, ix, t) in zip(axs, (([0, 12, 0], [0, 1, 0], (0, 2), 'Schnitt quer durch die Insert-Loecher (Sensor orange)'),
                                     ([12, 0, 0], [1, 0, 0], (1, 2), 'Schnitt laengs durch die Mitte: Steg mit Tunnel und Kabelrinne'))):
    s = m.section(plane_origin=o, plane_normal=nrm)
    for d in s.discrete: ax.plot(d[:, ix[0]], d[:, ix[1]], 'b-')
    if ix[0] == 0: ax.add_patch(plt.Rectangle((0, 0), 24, 8, color='orange', alpha=0.4))
    else:
        ax.add_patch(plt.Rectangle((0, 0), 32, 8, color='orange', alpha=0.4))
        ax.add_patch(plt.Rectangle((-6, 2.9), 6, 4.6, color='gray', alpha=0.4))
    ax.set_aspect('equal'); ax.set_title(t); ax.grid(alpha=0.3)
plt.tight_layout(); plt.savefig(out.replace('_vorschau.png', '_schnitt.png'), dpi=80)
