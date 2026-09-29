#!/usr/bin/env python3
"""Illustrate every face pairing changed by inside/outside complementation.

Uses the source face lookup and the same six-face cube assembly as the case
gallery. Python 3.9+ and the standard library suffice. --check verifies the
exhaustive selection and byte-for-byte reproducibility of all output files.
"""

import argparse
from collections import Counter
import json
from pathlib import Path

import generate_double_crossing_tables as tables
from double_crossing_case_data import build_cases, _local_crossing, _TURN_POINTS
from render_double_crossing_cases import COLORS, cube, points, project, svg_document, text


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "doc/double_crossing/conflicts"
A_COLOR = "#c66322"
B_COLOR = "#256abc"
FAMILIES = {
    0x05: ("family-01", "Diagonal corners; four single hits"),
    0x13: ("family-02", "Adjacent corners; one double-hit edge"),
    0x53: ("family-03", "Adjacent corners; two double-hit edges"),
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def endpoint_pairs(paths):
    """Unoriented pairs, keeping the exact edge and slot at each endpoint."""
    return {tuple(sorted((path[0], path[-1]))) for path in paths}


def orbit_pair_key(key):
    """Canonical complementary pair under a simultaneous D4 transform."""
    keys = []
    for code in range(8):
        mapped, _ = tables.transform(key & 15, key >> 4, [], code)
        keys.append(min(mapped, mapped ^ 15))
    return min(keys)


def template_name(key):
    for name, corners, pairs, paths in tables.SEEDS:
        if any(tables.transform(corners, pairs, paths, code)[0] == key
               for code in range(8)):
            return name
    raise ValueError(f"Missing representative for 0x{key:02X}")


def build_conflicts():
    cases = {case["face_key"]: case for case in build_cases(True)}
    changed = [key for key in sorted(cases)
               if tables.signature(cases[key]["seed_paths"]) !=
               tables.signature(cases[key ^ 15]["seed_paths"])]
    require(len(changed) == 14, "Expected exactly 14 complement-sensitive face states")
    selected = [(key, key ^ 15) for key in changed if key < (key ^ 15)]
    require(len(selected) == 7, "Expected exactly seven complementary pairs")
    pairs = []
    for index, (key_a, key_b) in enumerate(selected, 1):
        case_a, case_b = cases[key_a], cases[key_b]
        require(case_a["face_counts"] == case_b["face_counts"], "Complement changed hits")
        require(case_a["cube_counts"] == case_b["cube_counts"], "Cube hit counts differ")
        require(case_a["cube_corners"] ^ case_b["cube_corners"] == 255,
                "Expected the two cube inputs to be complements")
        edge_nodes_a = {key: value for key, value in case_a["nodes"].items() if int(key) < 24}
        edge_nodes_b = {key: value for key, value in case_b["nodes"].items() if int(key) < 24}
        require(edge_nodes_a == edge_nodes_b, "Edge-hit coordinates or IDs differ")
        paths_a, paths_b = endpoint_pairs(case_a["seed_paths"]), endpoint_pairs(case_b["seed_paths"])
        require(paths_a != paths_b, "Selected pair differs only in direction or turn naming")
        canonical = orbit_pair_key(key_a)
        require(canonical in FAMILIES, "Unexpected D4 complement-pairing family")
        family, description = FAMILIES[canonical]
        pairs.append({
            "name": f"conflict-{index:02d}",
            "key_a": key_a,
            "key_b": key_b,
            "case_a": case_a,
            "case_b": case_b,
            "template_a": template_name(key_a),
            "template_b": template_name(key_b),
            "same_connections": [list(pair) for pair in sorted(paths_a & paths_b)],
            "only_a": [list(pair) for pair in sorted(paths_a - paths_b)],
            "only_b": [list(pair) for pair in sorted(paths_b - paths_a)],
            "family": family,
            "family_description": description,
            "family_representative_key": canonical,
        })
    family_counts = Counter(pair["family"] for pair in pairs)
    require(sorted(family_counts.values()) == [1, 2, 4], "Unexpected D4 family sizes")
    return {
        "definition": "Both inputs are valid. Unoriented face pairing differs between key and key XOR 0x0F, at fixed edge/slot labels.",
        "face_state_count": len(cases),
        "changed_state_count": len(changed),
        "pair_count": len(pairs),
        "d4_family_count": len(family_counts),
        "geometry_note": "Illustrative coordinates only. Each side is a separate complemented input; no GT or triangle reconstruction is asserted.",
        "families": [{"name": family, "description": description,
                      "representative_key": key, "pair_count": family_counts[family]}
                     for key, (family, description) in FAMILIES.items()],
        "pairs": pairs,
    }


def face(case, x, y, size, color):
    """A front-on source face. Labels use the face table's own edge slots."""
    def screen(uv):
        return x + size * uv[0], y + size * (1 - uv[1])

    out = [f'<rect x="{x}" y="{y}" width="{size}" height="{size}" '
           'fill="#f5f9fd" stroke="#98aabd" stroke-width="1.3"/>']
    for path in case["seed_paths"]:
        coords = [screen(_TURN_POINTS[int(path[0][1])] if token.startswith("y")
                         else _local_crossing(token, case["face_counts"])) for token in path]
        coords_text = " ".join(f"{px:.2f},{py:.2f}" for px, py in coords)
        out.append(f'<polyline points="{coords_text}" fill="none" stroke="{color}" '
                   'stroke-width="3.3" stroke-linejoin="round"/>')
        if len(path) == 3:
            px, py = coords[1]
            out.append(f'<circle cx="{px:.2f}" cy="{py:.2f}" r="4" '
                       'fill="#f5bf3e" stroke="#ad7705" stroke-width="1.2"/>')
            out.append(text(round(px + 7, 2), round(py - 7, 2), "Y", 10, "#936303"))
    for edge, count in enumerate(case["face_counts"]):
        for slot in range(count):
            token = f"e{edge}:{slot}"
            px, py = screen(_local_crossing(token, case["face_counts"]))
            out.append(f'<circle cx="{px:.2f}" cy="{py:.2f}" r="4" '
                       f'fill="white" stroke="{color}" stroke-width="1.5"/>')
            dx, dy, anchor = ((0, 20, "middle"), (12, 4, "start"),
                              (0, -11, "middle"), (-12, 4, "end"))[edge]
            out.append(text(round(px + dx, 2), round(py + dy, 2), token, 10,
                            "#304b65", f'text-anchor="{anchor}"'))
    for index, uv in enumerate(((0, 0), (1, 0), (1, 1), (0, 1))):
        px, py = screen(uv)
        fill = "#20364e" if case["face_corners"] & (1 << index) else "white"
        out.append(f'<circle cx="{px:.2f}" cy="{py:.2f}" r="5" '
                   f'fill="{fill}" stroke="#20364e" stroke-width="1.5"/>')
        dx = -10 if uv[0] == 0 else 10
        dy = 16 if uv[1] == 0 else -10
        anchor = "end" if uv[0] == 0 else "start"
        out.append(text(round(px + dx, 2), round(py + dy, 2), f"c{index}", 10,
                        extra=f'text-anchor="{anchor}"'))
    return "".join(out)


def highlighted_cube(case, origin, color):
    # Boundary graphs only: the selected face is vivid, the remaining graph
    # is muted. Do not imply an actual triangulated or GT surface.
    geometry = {key: value for key, value in case.items() if key != "patches"}
    body = cube(geometry, scale=146, origin=origin)
    for loop_color in COLORS:
        body = body.replace(f'stroke="{loop_color}"', 'stroke="#a4b7c9"')
    for path in case["face_paths"]:
        if path["face"] != 0:
            continue
        coords = [case["nodes"][str(node)] for node in path["nodes"]]
        body += (f'<polyline points="{points(coords, 146, origin)}" fill="none" '
                 f'stroke="{color}" stroke-width="4" stroke-linejoin="round"/>')
        for node, coord in zip(path["nodes"], coords):
            px, py = project(coord, 146, origin)
            fill = "#f5bf3e" if node >= 24 else "white"
            body += (f'<circle cx="{px:.2f}" cy="{py:.2f}" r="3.8" '
                     f'fill="{fill}" stroke="{color}" stroke-width="1.4"/>')
    return body


def diagram(pair):
    index = int(pair["name"].split("-")[1])
    body = text(24, 31, f'COMPLEMENT PAIR {index:02d} / 07', 23, "#1c344d", 'font-weight="700"')
    body += text(976, 29, f'{pair["family"]} / 3 D4 families', 12,
                 extra='text-anchor="end"')
    body += text(24, 54, "Both inputs are valid: identical edge hits; opposite corner labels; different pairing.", 14)
    for side, color, offset in (("a", A_COLOR, 0), ("b", B_COLOR, 500)):
        case = pair[f"case_{side}"]
        body += f'<rect x="{offset + 16}" y="70" width="468" height="497" rx="13" fill="white" stroke="#dae4ed"/>'
        body += text(offset + 34, 96, f'{side.upper()}  /  face 0x{case["face_key"]:02X}', 18,
                     color, 'font-weight="700"')
        body += text(offset + 468, 94, f'template {pair[f"template_{side}"]}', 12,
                     extra='text-anchor="end"')
        body += highlighted_cube(case, (offset + 247, 212), color)
        body += text(offset + 247, 326, f'{len(case["loops"])} cube boundary loops; z=0 face highlighted', 11,
                     extra='text-anchor="middle"')
        body += f'<line x1="{offset + 32}" y1="340" x2="{offset + 468}" y2="340" stroke="#e2e9ef"/>'
        body += text(offset + 34, 360, 'SOURCE FACE  /  SAME FIXED EDGE-SLOT LABELS', 11,
                     "#516b80", 'font-weight="700"')
        body += face(case, offset + 74, 393, 130, color)
        body += text(offset + 276, 396, 'Chosen connections', 12, "#20364e", 'font-weight="700"')
        paths = sorted(case["seed_paths"], key=lambda path: tuple(sorted((path[0], path[-1]))))
        for line, path in enumerate(paths):
            # Direction is intentionally suppressed: the comparison concerns
            # connectivity, not oriented inside-left path direction.
            endpoints = sorted((path[0], path[-1]))
            label = f'{endpoints[0]} -- {endpoints[1]}'
            body += text(offset + 276, 420 + 35 * line, label, 12, color)
            if len(path) == 3:
                body += text(offset + 276, 434 + 35 * line, 'via interior turn Y', 10, "#987114")
        body += text(offset + 276, 542, 'filled corner = inside', 10)
        body += text(offset + 276, 555, 'hollow corner = outside', 10)
    body += text(24, 588, f'{pair["family_description"]}. Counts: {pair["case_a"]["face_counts"]}.', 12)
    body += text(24, 609, 'Separate complemented inputs; illustrative coordinates and turns. Boundary connectivity only, no GT or triangle reconstruction.', 11)
    return svg_document(1000, 620, body, f'Valid complement-pairing comparison {index}: 0x{pair["key_a"]:02X} versus 0x{pair["key_b"]:02X}')


def overview(data):
    width, height = 1200, 810
    body = text(28, 38, '7 COMPLEMENT PAIRS / 14 VALID STATES / 3 D4 FAMILIES', 24, "#1c344d", 'font-weight="700"')
    body += text(28, 65, 'At fixed edge slots, swapping all inside/outside labels changes the lookup pairing in these cases.', 14)
    body += text(28, 88, 'Orange = input A. Blue = complemented input B. Filled corner = inside; hollow = outside; yellow = face turn.', 13)
    for index, pair in enumerate(data["pairs"]):
        x, y = 24 + (index % 2) * 588, 109 + (index // 2) * 172
        body += f'<rect x="{x}" y="{y}" width="572" height="157" rx="10" fill="white" stroke="#dde5ed"/>'
        body += text(x + 15, y + 23, f'{index + 1:02d}   0x{pair["key_a"]:02X} / 0x{pair["key_b"]:02X}', 15, "#1c344d", 'font-weight="700"')
        body += text(x + 557, y + 23, pair["family"], 11, extra='text-anchor="end"')
        body += face(pair["case_a"], x + 65, y + 50, 70, A_COLOR)
        body += face(pair["case_b"], x + 278, y + 50, 70, B_COLOR)
        body += text(x + 215, y + 92, 'A / B', 12, extra='text-anchor="middle"')
        body += text(x + 403, y + 76, f'template {pair["template_a"]}', 11, A_COLOR)
        body += text(x + 403, y + 96, f'template {pair["template_b"]}', 11, B_COLOR)
        body += text(x + 403, y + 120, f'hits {pair["case_a"]["face_counts"]}', 10)
    body += text(625, 693, 'Why this matters', 17, "#1c344d", 'font-weight="700"')
    body += text(625, 720, 'Complement is not a valid compression symmetry', 14)
    body += text(625, 743, 'for this selected face-pairing policy.', 14)
    body += text(625, 770, 'Both sides are valid inputs, separate from the 174 invalid keys.', 12)
    return svg_document(width, height, body, 'All seven complement-sensitive pairs')


def readme(data):
    rows = []
    for pair in data["pairs"]:
        rows.append(f'| [对照 {pair["name"].split("-")[1]}]({pair["name"]}.svg) | `0x{pair["key_a"]:02X}` / `0x{pair["key_b"]:02X}` '
                    f'| {pair["template_a"]} / {pair["template_b"]} | {pair["family"]} '
                    f'| {pair["case_a"]["face_counts"]} |')
    return '''# 内外翻转后的连接差异（合法输入）

[连接差异总览](overview.svg) · [23 个代表 case](../cases/README.md) · [174 个非法输入](../invalid/README.md) · [机器可读完整数据](conflicts.json)

[下载分类 PDF 图册（本组内容位于附录）](../../../output/pdf/MeshUtil_double_crossing_cases_and_conflicts.pdf)

**每组两侧都是合法输入。** 固定面坐标、边编号和交点槽编号，把四个角点的内外状态全部翻转后，查表给出的无向端点配对改变。这说明本表不能把内外翻转当成压缩对称。

输入矛盾是“端点异号却要求两个交点”，共 174 个非法编码，另见[非法输入图集](../invalid/README.md)。本组不属于它们，也不表示同一输入在表中有两种结果，或两图来自同一个 GT 网格。目录名与 `conflict-XX.svg` 文件名保留，仅为兼容已有链接。

枚举全部 82 个合法面状态，比较 `key` 和 `key ^ 0x0F`，得到 14 个合法状态、7 对互补输入。再按同时旋转/镜像、允许 A/B 两侧交换归并，可得 **3 类**：对角角点四条单交边（1 对）；相邻角点一条双交边（4 对）；相邻角点两条双交边（2 对）。原始 23 个模板中涉及 `3b`、`3c`、`3d`、`4a`。

| 对照图 | 面编码 A / B | 代表模板 A / B | 旋转镜像类 | 边交点数 e0…e3 |
| --- | --- | --- | --- | --- |
''' + "\n".join(rows) + '''

## 图怎么读

- 每张图上方是 A/B 两个立方体，下方是对应的 `z=0` 二维面。橙色为 A 的选定配对，蓝色为 B 的选定配对。
- 实心角点表示 inside，空心角点表示 outside；两侧所有角点的状态相反，但边上的交点位置和数量完全相同。
- `eI:J` 是二维面边 I 的第 J 个交点，与源表一致；边方向为 `c0→c1→c2→c3→c0`，槽编号沿边方向递增。**这是面局部标签，不是 C++ 立方体全局节点 ID。**
- 黄色 `Y` 是路径必须进入面内部再返回同一条边时的示意转折点；具体位置和是否展开为多个点，仍依赖实际输入几何。
- 立方体在 `z=0` 和 `z=1` 重复该面输入，竖向边没有交点。上方浅灰蓝线展示六个面按原表组装的完整边界环，彩色强调 `z=0` 的差异。
- 本组互补输入的边界环数量相同，但连接到哪些交点不同。因此只数环不能判断配对是否一致。
- 图中的交点取边中点，或双交点的 0.32/0.68 参数位置，黄色点取示意偏移；这些是可视化坐标，**不是 GT 表面或实际三角化重建结果**。

## 复现与验证

```bash
python tools/render_double_crossing_conflicts.py
python tools/render_double_crossing_conflicts.py --check
```

生成器调用 `double_crossing_case_data.build_cases(True)` 取得全部面状态及六面组装的立方体数据，用源表的无向 `signature` 比较内外翻转。它同时检查交点编号与坐标不变、立方体角点恰好互补、14 状态 / 7 对 / 3 类的穷举数量；`--check` 再按字节验证提交的 SVG、JSON 和本说明可确定性复现。

`conflicts.json` 的每个 `pairs[]` 包含 `key_a/key_b`、原始 `case_a/case_b` 完整数据、`template_a/template_b`、共同与各侧独有的无向端点对 `same_connections/only_a/only_b`，以及 `family` 分类。是否含黄色转折由各 `case_*.seed_paths` 保留，不能仅由端点对字段解释几何。
'''


def generate():
    data = build_conflicts()
    files = {f'{pair["name"]}.svg': diagram(pair) for pair in data["pairs"]}
    files["overview.svg"] = overview(data)
    files["conflicts.json"] = json.dumps(data, ensure_ascii=False, indent=2) + "\n"
    files["README.md"] = readme(data)
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=OUTPUT)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    files = generate()
    if args.check:
        stale = [name for name, content in files.items()
                 if not (args.output / name).exists() or
                 (args.output / name).read_bytes() != content.encode("utf-8")]
        if stale:
            parser.exit(1, "Missing or stale output: " + ", ".join(stale) + "\n")
        print(f"Verified {len(files)} artifacts: 14 valid states, 7 pairs, 3 D4 families.")
    else:
        args.output.mkdir(parents=True, exist_ok=True)
        for name, content in files.items():
            (args.output / name).write_bytes(content.encode("utf-8"))
        print(f"Generated {len(files)} artifacts in {args.output}")


if __name__ == "__main__":
    main()
