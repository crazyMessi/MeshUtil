import numpy as np

import meshutil

vertices = np.load("vertices.npy").astype(np.float32, copy=False)
faces = np.load("faces.npy").astype(np.uint32, copy=False)
out_vertices, out_faces, stats = meshutil.simplify(vertices, faces, 1_000_000)
np.save("vertices_simplified.npy", out_vertices)
np.save("faces_simplified.npy", out_faces)
print(stats)
