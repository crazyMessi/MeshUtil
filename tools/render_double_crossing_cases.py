#!/usr/bin/env python3
"""Render source-backed cube examples as standalone SVGs and an offline gallery.

Python 3.9+; standard library only. Geometry comes from double_crossing_case_data.
Run again after changing the connectivity tables. --check detects stale output.
"""

import argparse
import html
import json
import math
from pathlib import Path

from double_crossing_case_data import CUBE_EDGES, build_cases

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "doc/double_crossing/cases"
COLORS = ["#008d91", "#df6c35", "#8661c5", "#347dcc"]
CORNERS = [[i & 1, (i >> 1) & 1, (i >> 2) & 1] for i in range(8)]


def project(point, scale=125, origin=(175, 155)):
    x, y, z = [v - .5 for v in point]
    return origin[0] + scale * (x + .58 * y), origin[1] + scale * (.34 * y - z)


def points(vertices, scale, origin):
    return " ".join(f"{x:.2f},{y:.2f}" for x, y in
                    (project(point, scale, origin) for point in vertices))


def text(x, y, content, size=12, color="#516276", extra=""):
    return (f'<text x="{x}" y="{y}" font-size="{size}" fill="{color}" {extra}>'
            f'{html.escape(str(content))}</text>')


def cube(case, scale=125, origin=(175, 155), labels=False):
    """A fixed oblique projection with the z=0 reference face tinted."""
    out = []
    out.append(f'<polygon points="{points([CORNERS[i] for i in [0,1,3,2]],scale,origin)}" '
               'fill="#e4f0fa" fill-opacity=".65" stroke="none"/>')
    for patch in case.get("patches", []):
        out.append(f'<polygon points="{points(patch,scale,origin)}" '
                   'fill="#27aca6" fill-opacity=".17" stroke="none"/>')
    for a, b in CUBE_EDGES:
        out.append(f'<polyline points="{points([CORNERS[a], CORNERS[b]],scale,origin)}" '
                   'fill="none" stroke="#9eafc0" stroke-width="1.3"/>')
    for index, loop in enumerate(case["loops"]):
        coords = [case["nodes"][str(node)] for node in loop]
        coords.append(coords[0])
        color = COLORS[index % len(COLORS)]
        out.append(f'<polyline points="{points(coords,scale,origin)}" fill="none" '
                   f'stroke="{color}" stroke-width="3.1" stroke-linejoin="round"/>')
    for node, coord in case["nodes"].items():
        x, y = project(coord, scale, origin)
        fill = "#f5bf3e" if int(node) >= 24 else "#fff"
        stroke = "#ae7705" if int(node) >= 24 else "#008d91"
        out.append(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="3.6" '
                   f'fill="{fill}" stroke="{stroke}" stroke-width="1.2"/>')
        if labels:
            out.append(text(round(x+6,2), round(y-6,2), node, 9))
    for index, coord in enumerate(CORNERS):
        x, y = project(coord, scale, origin)
        inside = bool(case["cube_corners"] & (1 << index))
        out.append(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="5" '
                   f'fill="{"#20364e" if inside else "white"}" stroke="#20364e" stroke-width="1.6"/>')
        if labels:
            out.append(text(round(x-14,2), round(y+4,2), f"c{index}", 10))
    if not case["loops"]:
        out.append(text(origin[0], origin[1]+5, "ALL INSIDE" if case["cube_corners"] else "ALL OUTSIDE",
                        11, "#64798b", 'text-anchor="middle"'))
    return "".join(out)


def svg_document(width, height, body, title):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
            f'viewBox="0 0 {width} {height}" role="img">'
            f'<title>{html.escape(title)}</title><rect width="100%" height="100%" fill="#f6f8fb"/>'
            '<g font-family="Segoe UI, Arial, sans-serif">'+body+'</g></svg>\n')


def card(case, x=0, y=0):
    body = '<rect x="1" y="1" width="348" height="298" rx="14" fill="white" stroke="#dde5ed"/>'
    body += text(20, 30, f'CASE {case["name"]}', 19, "#1c344d", 'font-weight="700"')
    body += text(330, 29, f'{len(case["loops"])} loops', 12, extra='text-anchor="end"')
    body += cube(case)
    body += text(20, 264, f'face 0x{case["face_key"]:02X}   counts [{", ".join(map(str,case["face_counts"]))}]', 12)
    body += text(20, 284, f'cube 0x{case["cube_corners"] | (case["cube_pairs"] << 8):05X}', 11, "#7b8b9d")
    return f'<g transform="translate({x},{y})">{body}</g>'


def generate():
    cases = build_cases()
    files = {f'case-{case["name"]}.svg': svg_document(350, 300, card(case), f'Cube example for case {case["name"]}')
             for case in cases}
    body = text(28, 43, "DOUBLE-CROSSING / 23 CUBE EXAMPLES", 26, "#1c344d", 'font-weight="700"')
    body += text(28, 72, "Each face case is repeated at z=0 and z=1. Colored lines are boundary loops from the lookup rules.", 15)
    body += text(28, 99, "Filled corner = inside    Hollow corner = outside    Small circle = edge hit    Yellow = illustrative face turn", 14)
    body += text(28, 121, "Blue-tinted face = source face (z=0). Example coordinates only; these diagrams are not a triangle reconstruction.", 13)
    for index, case in enumerate(cases):
        body += card(case, 24 + (index % 4) * 366, 145 + (index // 4) * 316)
    files["overview.svg"] = svg_document(1496, 145 + math.ceil(len(cases)/4)*316 + 20, body,
                                         "All 23 representative double-crossing cases shown as cubes")
    data = {"cases": cases, "edges": CUBE_EDGES, "corners": CORNERS}
    files["cases.json"] = json.dumps(data, ensure_ascii=False, indent=2) + "\n"
    template = (ROOT / "tools/templates/double_crossing_gallery.html").read_text(encoding="utf-8")
    thumbs = "\n".join(f'<button class="case-card" data-case="{html.escape(c["name"])}" '
                         f'aria-label="查看 case {html.escape(c["name"])}">'
                         f'<img src="case-{html.escape(c["name"])}.svg" alt="Case {html.escape(c["name"])} 示例立方体" '
                         'width="350" height="300" loading="lazy"></button>' for c in cases)
    files["index.html"] = template.replace("__CASE_DATA__", json.dumps(data, ensure_ascii=False)).replace("__CASE_CARDS__", thumbs)
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=OUTPUT)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    files = generate()
    if args.check:
        stale = [name for name, content in files.items()
                 if not (args.output / name).exists() or (args.output / name).read_bytes() != content.encode("utf-8")]
        if stale:
            parser.exit(1, "Missing or stale output: " + ", ".join(stale) + "\n")
        print(f"Verified {len(files)} artifacts: all 23 source cases, deterministic SVG/HTML/JSON.")
    else:
        args.output.mkdir(parents=True, exist_ok=True)
        for name, content in files.items():
            (args.output / name).write_bytes(content.encode("utf-8"))
        print(f"Generated {len(files)} artifacts in {args.output}")


if __name__ == "__main__":
    main()
