#!/usr/bin/env python3
"""Draw all 174 inconsistent face encodings without inventing a surface.

Python 3.9+; standard library only. The classification is checked against the
source table's crossing_counts(). --check verifies byte-for-byte outputs.
"""

import argparse
import html
import json
import math
from pathlib import Path

import generate_double_crossing_tables as tables
from double_crossing_case_data import CUBE_EDGES
from render_double_crossing_cases import CORNERS, points, project, text


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "doc/double_crossing/invalid"
INK = "#20364e"
MUTED = "#61778c"
RED = "#c53d42"
TEAL = "#168d93"
FACE_CORNERS = ((0, 0), (1, 0), (1, 1), (0, 1))
FACE_TO_CUBE = (0, 1, 3, 2)
EXPECTED_COUNTS = {"1": 104, "2": 60, "3": 8, "4": 2}
REPRESENTATIVES = (0x11, 0x91, 0x75, 0xF5)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def build_invalid():
    """Return exhaustive invalid inputs, grouped by the number of bad edges."""
    states, valid_count = [], 0
    for key in range(256):
        corners, pairs = key & 15, key >> 4
        signs = [(corners >> i) & 1 for i in range(4)]
        pair_bits = [(pairs >> i) & 1 for i in range(4)]
        bad_edges = [i for i in range(4)
                     if signs[i] != signs[(i + 1) % 4] and pair_bits[i]]
        counts = [2 if pair_bits[i] else int(signs[i] != signs[(i + 1) % 4])
                  for i in range(4)]
        source_counts = tables.crossing_counts(corners, pairs)
        require((source_counts is None) == bool(bad_edges),
                f"Classification disagrees with source table: 0x{key:02X}")
        if not bad_edges:
            valid_count += 1
            require(source_counts == counts, f"Crossing count mismatch: 0x{key:02X}")
            continue
        states.append({
            "key": key,
            "hex": f"0x{key:02X}",
            "corners": corners,
            "pairs": pairs,
            "corner_bits_c0_to_c3": signs,
            "pair_bits_e0_to_e3": pair_bits,
            "bad_edges": bad_edges,
            "counts": counts,
            "group": len(bad_edges),
            "bad_edge_count": len(bad_edges),
        })
    counts = {str(k): sum(s["group"] == k for s in states) for k in range(1, 5)}
    require(valid_count == 82 and len(states) == 174, "Unexpected face state totals")
    require(counts == EXPECTED_COUNTS, f"Unexpected invalid groups: {counts}")
    by_key = {s["key"]: s for s in states}
    require(all(by_key[key]["group"] == n for n, key in enumerate(REPRESENTATIVES, 1)),
            "Representative is in the wrong group")
    return {
        "definition": "A bad edge has opposite endpoint signs and its two-crossing pair bit set.",
        "encoding": "key = corners | (pairs << 4); bit i of corners is c_i; bit i of pairs is e_i.",
        "corner_order": "c0=(0,0), c1=(1,0), c2=(1,1), c3=(0,1)",
        "edge_directions": [[0, 1], [1, 2], [2, 3], [3, 0]],
        "geometry_note": "Only requested corner labels and edge hits are drawn. No valid cube, face connectivity, loop, or surface is asserted.",
        "valid_count": valid_count,
        "invalid_count": len(states),
        "counts_by_bad_edges": counts,
        "representatives": list(REPRESENTATIVES),
        "states": states,
    }


def circle(x, y, radius, fill="white", stroke=INK, width=1.4, extra=""):
    return (f'<circle cx="{x:.2f}" cy="{y:.2f}" r="{radius:.2f}" '
            f'fill="{fill}" stroke="{stroke}" stroke-width="{width}" {extra}/>')


def line(a, b, color, width=1.5, extra=""):
    return (f'<line x1="{a[0]:.2f}" y1="{a[1]:.2f}" '
            f'x2="{b[0]:.2f}" y2="{b[1]:.2f}" '
            f'stroke="{color}" stroke-width="{width}" {extra}/>')


def arrow(a, b, color=MUTED, width=1.3, head=5):
    dx, dy = b[0] - a[0], b[1] - a[1]
    length = math.hypot(dx, dy)
    ux, uy = dx / length, dy / length
    left = (b[0] - head * ux - .45 * head * uy, b[1] - head * uy + .45 * head * ux)
    right = (b[0] - head * ux + .45 * head * uy, b[1] - head * uy - .45 * head * ux)
    return line(a, b, color, width) + (f'<polyline points="{left[0]:.2f},{left[1]:.2f} '
            f'{b[0]:.2f},{b[1]:.2f} {right[0]:.2f},{right[1]:.2f}" '
            f'fill="none" stroke="{color}" stroke-width="{width}"/>')


def hits(state, edge):
    count = state["counts"][edge]
    return () if count == 0 else (.5,) if count == 1 else (.32, .68)


def face(state, x, y, size, labels=False):
    """Front view: c0 bottom left, then counterclockwise c1, c2, c3."""
    screen = lambda uv: (x + size * uv[0], y + size * (1 - uv[1]))
    radius = 5 if labels else 3.4
    body = f'<rect x="{x}" y="{y}" width="{size}" height="{size}" fill="#f4f8fc" stroke="none"/>'
    for i in range(4):
        a, b = screen(FACE_CORNERS[i]), screen(FACE_CORNERS[(i + 1) % 4])
        color = RED if i in state["bad_edges"] else "#94a8b9"
        invalid = str(i in state["bad_edges"]).lower()
        body += line(a, b, color, 3.4 if i in state["bad_edges"] else 1.4,
                     f'data-kind="edge" data-edge="{i}" data-invalid="{invalid}"')
        for slot, t in enumerate(hits(state, i)):
            px, py = a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1])
            body += circle(px, py, 3.5 if labels else 2.1, "white", RED if i in state["bad_edges"] else TEAL, 1.4,
                           f'data-kind="hit" data-edge="{i}" data-slot="{slot}" data-invalid="{invalid}"')
        if labels:
            offsets = ((0, 29), (31, 0), (0, -25), (-31, 0))
            dx, dy = offsets[i]
            mid = ((a[0] + b[0]) / 2 + dx, (a[1] + b[1]) / 2 + dy)
            direction = ("→", "↑", "←", "↓")[i]
            body += text(round(mid[0], 2), round(mid[1] + 4, 2), f"e{i} {direction}", 12,
                         RED if i in state["bad_edges"] else MUTED, 'text-anchor="middle"')
    for i, uv in enumerate(FACE_CORNERS):
        px, py = screen(uv)
        body += circle(px, py, radius, INK if state["corners"] & (1 << i) else "white",
                       extra=f'data-kind="corner" data-corner="{i}"')
        if labels:
            dx, dy = (-9 if uv[0] == 0 else 9), (17 if uv[1] == 0 else -11)
            anchor = "end" if uv[0] == 0 else "start"
            body += text(round(px + dx, 2), round(py + dy, 2), f"c{i}", 11, INK, f'text-anchor="{anchor}"')
    return body


def thumbnail(state, x=0, y=0, width=150, height=130):
    """SVG fragment with title, face, and bad-edge list; supports 150x108 tiles."""
    scale = width / 150
    logical_h = height / scale
    require(logical_h >= 100, "Thumbnail requires height/width >= 2/3")
    face_size = min(74, logical_h - 54)
    body = f'<rect x="1" y="1" width="148" height="{logical_h - 2:.2f}" rx="7" fill="white" stroke="#dce5ee"/>'
    body += text(8, 16, f'0x{state["key"]:02X}', 11, INK, 'font-weight="700"')
    body += text(142, 16, f'{state["group"]} bad', 9, RED, 'text-anchor="end"')
    body += face(state, (150 - face_size) / 2, 29, face_size)
    body += text(75, round(logical_h - 8, 2), 'bad: ' + ', '.join(f'e{i}' for i in state["bad_edges"]),
                 9, RED, 'text-anchor="middle"')
    return f'<g transform="translate({x},{y})"><g transform="scale({scale:.6f})">{body}</g></g>'


def cube_input(state):
    """Only the z=0 face has data; other corners are intentionally unlabelled."""
    scale, origin = 128, (153, 218)
    body = (f'<polygon points="{points([CORNERS[i] for i in FACE_TO_CUBE], scale, origin)}" '
            'fill="#e7f0f8" stroke="none"/>')
    for a, b in CUBE_EDGES:
        body += line(project(CORNERS[a], scale, origin), project(CORNERS[b], scale, origin), "#b8c7d4", 1.3)
    for i in range(4):
        ca, cb = CORNERS[FACE_TO_CUBE[i]], CORNERS[FACE_TO_CUBE[(i + 1) % 4]]
        a, b = project(ca, scale, origin), project(cb, scale, origin)
        body += line(a, b, RED if i in state["bad_edges"] else "#849daf", 3.4 if i in state["bad_edges"] else 1.5)
        for t in hits(state, i):
            px, py = a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1])
            body += circle(px, py, 3.3, "white", RED if i in state["bad_edges"] else TEAL)
    for local, cube_index in enumerate(FACE_TO_CUBE):
        px, py = project(CORNERS[cube_index], scale, origin)
        body += circle(px, py, 5, INK if state["corners"] & (1 << local) else "white")
        dx, dy, anchor = ((-8, 15, "end"), (8, 3, "start"), (7, 16, "start"), (-8, 16, "end"))[local]
        body += text(round(px + dx, 2), round(py + dy, 2), f"c{local}", 11, INK, f'text-anchor="{anchor}"')
    body += text(40, 345, "仅标 z=0 面的输入", 13, INK)
    body += text(40, 369, "其余顶点、边：未指定", 12, MUTED)
    body += text(40, 393, "立方体线框只用于定位", 12, MUTED)
    return body


def diagram(state):
    n = state["group"]
    bad = ", ".join(f"e{i}" for i in state["bad_edges"])
    body = text(20, 31, f'{n} 条矛盾边 / {EXPECTED_COUNTS[str(n)]} 个编码', 24, INK, 'font-weight="700"')
    body += text(978, 30, f'代表输入 0x{state["key"]:02X}', 17, RED, 'text-anchor="end"')
    body += text(20, 58, "矛盾条件：端点异号，但 pair bit=1 要求两次穿越。每次穿越都会翻转内外。", 15, MUTED)
    for x, w in ((18, 270), (300, 256), (570, 412)):
        body += f'<rect x="{x}" y="80" width="{w}" height="340" rx="12" fill="white" stroke="#dce5ee"/>'
    body += text(36, 107, "① 立方体中的位置", 16, INK, 'font-weight="700"')
    body += cube_input(state)
    body += text(318, 107, "② 面输入与边方向", 16, INK, 'font-weight="700"')
    body += face(state, 355, 159, 146, True)
    body += text(318, 366, f'矛盾边：{bad}', 12, RED)
    body += text(318, 391, f'请求交点数：{state["counts"]}', 12, MUTED)
    edge = state["bad_edges"][0]
    start = (state["corners"] >> edge) & 1
    end = (state["corners"] >> ((edge + 1) % 4)) & 1
    label = lambda inside: "内" if inside else "外"
    body += text(588, 107, f"③ 放大 e{edge}：c{edge} → c{(edge + 1) % 4}", 16, INK, 'font-weight="700"')
    body += text(590, 144, "沿箭头方向走，两次穿越后回到原符号：", 13, MUTED)
    xs = (616, 718, 820, 936)
    body += line((xs[0], 186), (xs[-1], 186), INK, 1.7)
    # Put the arrowhead before the terminal circle so it remains visible.
    body += arrow((860, 186), (906, 186), INK, 1.7)
    body += circle(xs[0], 186, 7, INK if start else "white")
    for px in xs[1:3]:
        body += circle(px, 186, 4, "white", RED, 1.8)
    body += circle(xs[-1], 186, 7, INK if start else "white")
    for px, line1, line2 in ((616, f"起点：{label(start)}", f"c{edge}"),
                             (718, "第 1 次", f"变成{label(1 - start)}"),
                             (820, "第 2 次", f"变回{label(start)}"),
                             (936, f"应当：{label(start)}", "终点")):
        body += text(px, 217, line1, 12, INK, 'text-anchor="middle"')
        body += text(px, 240, line2, 12, MUTED, 'text-anchor="middle"')
    body += line((590, 269), (962, 269), "#e1e8ef", 1)
    body += text(590, 299, "但编码给定的终点是：", 14, INK)
    body += circle(825, 294, 8, INK if end else "white", RED, 2)
    body += text(844, 300, label(end), 18, RED, 'font-weight="700"')
    body += text(590, 339, f"应当为{label(start)}，却写成{label(end)} → 输入自相矛盾", 15, RED, 'font-weight="700"')
    body += text(590, 371, "此输入不生成连接线、闭环或曲面。", 13, MUTED)
    body += text(590, 394, "API：std::invalid_argument", 13, MUTED)
    body += text(20, 448, "实心角点=内；空心角点=外；小圆=请求交点；红色=矛盾边。局部 c0,c1,c2,c3 对应立方体顶点 0,1,3,2。", 12, MUTED)
    return svg_document(1000, 470, body, f"非法输入：{n} 条矛盾边，代表 0x{state['key']:02X}")


def svg_document(width, height, body, title):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
            f'viewBox="0 0 {width} {height}" role="img"><title>{html.escape(title)}</title>'
            '<rect width="100%" height="100%" fill="#f6f8fb"/>'
            '<g font-family="Microsoft YaHei, Segoe UI, Arial, sans-serif">' + body + '</g></svg>\n')


def overview(states, group):
    cols, w, h, gap, margin = 8, 150, 130, 12, 20
    width = margin * 2 + cols * w + (cols - 1) * gap
    height = 107 + math.ceil(len(states) / cols) * (h + gap) + 8
    body = text(margin, 36, f"{group} 条矛盾边 / {len(states)} 个非法输入", 25, INK, 'font-weight="700"')
    body += text(margin, 62, "面角点：c0 左下，c1 右下，c2 右上，c3 左上；e0,e1,e2,e3 依次沿 c0→c1→c2→c3→c0。", 14, MUTED)
    body += text(margin, 86, "实心=内，空心=外，小圆=请求交点，红色边=异号端点却请求两次穿越。key=角点位 | (pair 位 << 4)。", 14, MUTED)
    for i, state in enumerate(states):
        body += thumbnail(state, margin + (i % cols) * (w + gap), 107 + (i // cols) * (h + gap))
    return svg_document(width, height, body, f"全部 {len(states)} 个含 {group} 条矛盾边的输入")


def readme(data):
    return """# 非法输入图集：174 个面编码

这里的“矛盾”只指 **端点异号，pair bit 却为 1（请求两次穿越）**。
这 174 个编码不是未实现的合法 case，也不表示网格裂缝或算法错误。

## 编码和图例

- `key = corners | (pairs << 4)`：低四位是角点，高四位是边的 pair 标志。
- 从正面看，`c0` 左下、`c1` 右下、`c2` 右上、`c3` 左上。
- `e0: c0→c1`、`e1: c1→c2`、`e2: c2→c3`、`e3: c3→c0`。
- 实心角点是内，空心角点是外；小圆表示请求的交点；红色只表示矛盾边。
- pair=0 时，同号端点请求 0 个交点、异号端点请求 1 个；pair=1 始终请求 2 个。
- 每次穿越翻转内外；两次穿越后应当与起点同号，因此异号端点 + pair=1 不可能成立。
- 双交点固定放在沿边方向 0.32、0.68 处，只是示意位置。

## 按矛盾边数分类

| 矛盾边数 | 编码数量 | 代表解释图 | 完整矩阵 |
|---|---:|---|---|
| 1 | 104 | [0x11](example-1.svg) | [104 个](group-1.svg) |
| 2 | 60 | [0x91](example-2.svg) | [60 个](group-2.svg) |
| 3 | 8 | [0x75](example-3.svg) | [8 个](group-3.svg) |
| 4 | 2 | [0xF5](example-4.svg) | [2 个](group-4.svg) |
| 合计 | **174** | | |

每类解释图包含立方体线框定位、局部面、矛盾边的两次穿越过程。
立方体仅承载一个面的输入：局部角点 c0,c1,c2,c3 对应立方体顶点 0,1,3,2；其余角点与边未指定。
对这些非法输入，图中不构造面连接、闭环或曲面。

全部 256 个编码中，82 个合法、174 个非法；分类为 104 + 60 + 8 + 2。
`invalid-XX.svg` 的 XX 是两位十六进制 key。机器可读全集见 [invalid.json](invalid.json)。
输入合法性逐项与 `tools/generate_double_crossing_tables.py` 的 `crossing_counts` 交叉验证。
非法输入由公开 API 拒绝，抛出 `std::invalid_argument`。

## 与“内外翻转后连接不同”的区别

旧版图册的 7 组翻转对照，两侧都是合法输入；它们展示查表连接关系的差别。
那些对照保留在 PDF 附录，不计入这里的 174 个非法输入。

## 重新生成

```sh
python tools/render_double_crossing_invalid.py
python tools/render_double_crossing_invalid.py --check
```

生成器只依赖 Python 标准库，产物可逐字节复现。
"""


def generate():
    data = build_invalid()
    files = {f'invalid-{s["key"]:02X}.svg': svg_document(300, 260, thumbnail(s, width=300, height=260),
             f'非法面输入 0x{s["key"]:02X}：{s["group"]} 条矛盾边') for s in data["states"]}
    by_key = {s["key"]: s for s in data["states"]}
    for n, key in enumerate(data["representatives"], 1):
        files[f"example-{n}.svg"] = diagram(by_key[key])
        files[f"group-{n}.svg"] = overview([s for s in data["states"] if s["group"] == n], n)
    files["invalid.json"] = json.dumps(data, ensure_ascii=False, indent=2) + "\n"
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
                 if not (args.output / name).exists() or (args.output / name).read_bytes() != content.encode("utf-8")]
        if stale:
            parser.exit(1, "Missing or stale output: " + ", ".join(stale) + "\n")
        print(f"Verified {len(files)} deterministic artifacts; 174 invalid inputs = 104 + 60 + 8 + 2.")
    else:
        args.output.mkdir(parents=True, exist_ok=True)
        for name, content in files.items():
            (args.output / name).write_bytes(content.encode("utf-8"))
        print(f"Generated {len(files)} artifacts in {args.output}")


if __name__ == "__main__":
    main()
