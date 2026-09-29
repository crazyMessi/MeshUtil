#!/usr/bin/env python3
"""Build the classified vector atlas: valid cases, invalid inputs, appendix.

Requires ReportLab (pip install reportlab). Pass --cjk-font if an embedded
Chinese TrueType font cannot be located automatically. All illustrations
remain vector graphics; no browser, network access, or raster conversion.
"""

import argparse
import json
import math
import re
import xml.etree.ElementTree as ET
from pathlib import Path

from reportlab.lib import colors
from reportlab.lib.pagesizes import A4, landscape
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.cidfonts import UnicodeCIDFont
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen import canvas

from render_double_crossing_cases import cube, svg_document, text
from render_double_crossing_conflicts import face as connected_face, A_COLOR, B_COLOR
from render_double_crossing_invalid import build_invalid, thumbnail

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "output/pdf/MeshUtil_double_crossing_cases_and_conflicts.pdf"
INK = "#20364e"
MUTED = "#65798d"
TEAL = "#008d91"
PAPER = "#f4f7fa"
W, H = landscape(A4)
CJK = "AtlasChinese"


def setup_font(path=None):
    global CJK
    candidates = [Path(path)] if path else [
        Path("C:/Windows/Fonts/simhei.ttf"),
        Path("C:/Windows/Fonts/msyh.ttc"),
        Path("/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc"),
    ]
    for candidate in candidates:
        if candidate.is_file():
            pdfmetrics.registerFont(TTFont(CJK, str(candidate)))
            return
    if path:
        raise FileNotFoundError(path)
    # Built-in CID fallback; embedding a local TrueType font is preferable.
    pdfmetrics.registerFont(UnicodeCIDFont("STSong-Light"))
    CJK = "STSong-Light"


def color(value):
    if value.startswith("#") and len(value)==4:
        value="#"+"".join(ch*2 for ch in value[1:])
    return colors.HexColor(value) if value.startswith("#") else colors.toColor(value)


def font_for(value, bold=False):
    return CJK if any(ord(ch) > 255 for ch in value) else ("Helvetica-Bold" if bold else "Helvetica")


def label(pdf, x, y, value, size=12, fill=INK, bold=False):
    value = str(value)
    pdf.setFillColor(color(fill))
    pdf.setFont(font_for(value, bold), size)
    pdf.drawString(x, H-y, value)


def paragraph(pdf, value, x, y, width, size=11, leading=18, fill=MUTED):
    """Wrap CJK prose by measured glyph width instead of arbitrary lengths."""
    for block in value.split("\n"):
        line = ""
        for char in block:
            trial = line + char
            if line and pdfmetrics.stringWidth(trial, font_for(trial), size) > width:
                label(pdf, x, y, line, size, fill)
                y += leading
                line = char
            else:
                line = trial
        if line:
            label(pdf, x, y, line, size, fill)
        y += leading
    return y


def svg_into(pdf, source, x, y, width, height):
    """Render the small, explicit SVG vocabulary emitted by our generators.

    Unsupported tags/transform functions fail loudly instead of silently
    disappearing from the PDF. y is measured down from the page's top.
    """
    root = ET.fromstring(source) if isinstance(source,str) and source.lstrip().startswith('<svg') else ET.parse(source).getroot()
    view = list(map(float, root.attrib.get("viewBox", "0 0 1 1").split()))
    factor = min(width/view[2], height/view[3])
    pdf.saveState()
    pdf.translate(x + (width-view[2]*factor)/2, H-y)
    pdf.scale(factor, -factor)
    pdf.translate(-view[0], -view[1])

    def walk(element, inherited):
        tag = element.tag.rsplit("}", 1)[-1]
        if tag in ("title", "desc"):
            return
        style = dict(inherited)
        style.update(element.attrib)
        for item in element.attrib.get("style", "").split(";"):
            if ":" in item:
                k, v = item.split(":", 1)
                style[k.strip()] = v.strip()
        pdf.saveState()
        transform = element.attrib.get("transform", "")
        for kind, values in re.findall(r"([A-Za-z]+)\(([^)]+)\)", transform):
            nums = [float(v) for v in re.split(r"[ ,]+", values.strip())]
            if kind == "translate":
                pdf.translate(nums[0], nums[1] if len(nums)>1 else 0)
            elif kind == "scale":
                pdf.scale(nums[0], nums[1] if len(nums)>1 else nums[0])
            else:
                raise ValueError(f"Unsupported SVG transform: {kind}")
        fill, stroke = style.get("fill", "black"), style.get("stroke", "none")
        do_fill, do_stroke = fill != "none", stroke != "none"
        if do_fill:
            pdf.setFillColor(color(fill))
        if do_stroke:
            pdf.setStrokeColor(color(stroke))
        opacity = float(style.get("opacity", 1))
        pdf.setFillAlpha(opacity * float(style.get("fill-opacity", 1)))
        pdf.setStrokeAlpha(opacity * float(style.get("stroke-opacity", 1)))
        pdf.setLineWidth(float(style.get("stroke-width", 1)))
        pdf.setLineJoin({"miter":0,"round":1,"bevel":2}.get(style.get("stroke-linejoin"),0))
        pdf.setLineCap({"butt":0,"round":1,"square":2}.get(style.get("stroke-linecap"),0))
        if "stroke-dasharray" in style:
            pdf.setDash([float(v) for v in re.split(r"[ ,]+", style["stroke-dasharray"])])
        n = lambda key, default=0: float(element.attrib.get(key, default))
        if tag in ("svg", "g"):
            for child in element:
                walk(child, style)
        elif tag == "rect":
            rw = view[2] if element.attrib.get("width") == "100%" else n("width")
            rh = view[3] if element.attrib.get("height") == "100%" else n("height")
            if n("rx"):
                pdf.roundRect(n("x"), n("y"), rw, rh, n("rx"), stroke=do_stroke, fill=do_fill)
            else:
                pdf.rect(n("x"), n("y"), rw, rh, stroke=do_stroke, fill=do_fill)
        elif tag == "circle":
            pdf.circle(n("cx"), n("cy"), n("r"), stroke=do_stroke, fill=do_fill)
        elif tag == "line":
            pdf.line(n("x1"), n("y1"), n("x2"), n("y2"))
        elif tag in ("polygon", "polyline"):
            coords = [float(v) for v in re.split(r"[ ,]+", element.attrib["points"].strip())]
            if len(coords) < 4 or len(coords)%2:
                raise ValueError("Invalid SVG polyline")
            p = pdf.beginPath()
            p.moveTo(*coords[:2])
            for i in range(2,len(coords),2):
                p.lineTo(*coords[i:i+2])
            if tag == "polygon":
                p.close()
            pdf.drawPath(p, stroke=do_stroke, fill=do_fill)
        elif tag == "text":
            if len(element):
                raise ValueError("SVG text spans are not supported")
            value = element.text or ""
            pdf.scale(1,-1)
            pdf.setFont(font_for(value, style.get("font-weight") in ("bold","700","600")), float(style.get("font-size",12)))
            anchor=style.get("text-anchor","start")
            draw={"start":pdf.drawString,"middle":pdf.drawCentredString,"end":pdf.drawRightString}[anchor]
            draw(n("x"),-n("y"),value)
        else:
            raise ValueError(f"Unsupported SVG tag: {tag}")
        pdf.restoreState()

    walk(root,{})
    pdf.restoreState()


def page_start(pdf, title, section, page, total):
    pdf.setFillColor(color(PAPER))
    pdf.rect(0,0,W,H,fill=1,stroke=0)
    label(pdf,34,30,"MESHUTIL / DOUBLE-CROSSING",9,TEAL,True)
    label(pdf,34,64,title,22,INK,True)
    pdf.setStrokeColor(color("#dce5ee"))
    pdf.line(34,32,W-34,32)
    label(pdf,34,H-17,section,9,MUTED)
    pdf.setFillColor(color(MUTED))
    pdf.setFont("Helvetica",9)
    pdf.drawRightString(W-34,17,f"{page:02d} / {total:02d}")


def bookmark(pdf, title, key):
    pdf.bookmarkPage(key)
    pdf.addOutlineEntry(title,key,level=0,closed=False)


def compact_case(case):
    body = '<rect x="1" y="1" width="244" height="210" rx="10" fill="white" stroke="#dde5ed"/>'
    body += text(14, 24, f'CASE {case["name"]}', 15, INK, 'font-weight="700"')
    body += text(231, 24, f'{len(case["loops"])} loops', 10, MUTED, 'text-anchor="end"')
    body += cube(case, scale=94, origin=(123, 111))
    body += text(14, 190, f'face 0x{case["face_key"]:02X}   counts {case["face_counts"]}', 10, MUTED)
    body += text(14, 205, f'cube 0x{case["cube_corners"] | (case["cube_pairs"] << 8):05X}', 9, MUTED)
    return svg_document(246, 212, body, f'Case {case["name"]}')


def complement_tile(pair):
    body = '<rect x="1" y="1" width="374" height="202" rx="10" fill="white" stroke="#dde5ed"/>'
    body += text(14, 23, f'0x{pair["key_a"]:02X} / 0x{pair["key_b"]:02X}', 15, INK, 'font-weight="700"')
    body += text(361, 23, f'{pair["template_a"]} / {pair["template_b"]}', 11, MUTED, 'text-anchor="end"')
    body += connected_face(pair["case_a"], 48, 61, 94, A_COLOR)
    body += connected_face(pair["case_b"], 234, 61, 94, B_COLOR)
    body += text(95, 191, 'A 合法', 12, A_COLOR, 'text-anchor="middle"')
    body += text(281, 191, 'B 合法', 12, B_COLOR, 'text-anchor="middle"')
    return svg_document(376, 204, body, '合法输入的内外翻转连接对照')


def invalid_rows(states):
    rows = []
    for group in range(1, 5):
        subset = [s for s in states if s["group"] == group]
        for offset in range(0, len(subset), 5):
            rows.append((group, offset, len(subset), subset[offset:offset+5]))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--cjk-font", type=Path)
    args = parser.parse_args()
    setup_font(args.cjk_font)
    cases = json.loads((ROOT/"doc/double_crossing/cases/cases.json").read_text(encoding="utf-8"))["cases"]
    pairs = json.loads((ROOT/"doc/double_crossing/conflicts/conflicts.json").read_text(encoding="utf-8"))["pairs"]
    invalid = build_invalid()
    stored = json.loads((ROOT/"doc/double_crossing/invalid/invalid.json").read_text(encoding="utf-8"))
    if stored != invalid:
        raise ValueError("Invalid-input diagrams are stale; regenerate them first")
    if len(cases) != 23 or len(pairs) != 7:
        raise ValueError("Expected 23 representative cases and seven legal complement pairs")
    by_name = {c["name"]: c for c in cases}
    valid_pages = [
        ("合法模板 / 0 个内部角点", [("0 内 / 4 外", ["1a", "1b", "1c"]), ("0 内 / 4 外（续）", ["1d", "1e", "1f"])]),
        ("合法模板 / 1 个与 3 个内部角点", [("1 内 / 3 外", ["2a", "2b", "2c"]), ("3 内 / 1 外", ["11a", "11b", "11c"])]),
        ("合法模板 / 2 个内部角点", [("内部角点相邻", ["3a", "3b", "3c"]), ("内部角点相邻（3d） / 对角（4a）", ["3d", "4a"])]),
        ("合法模板 / 4 个内部角点", [("4 内 / 0 外", ["12a", "12b", "12c"]), ("4 内 / 0 外（续）", ["12d", "12e", "12f"])]),
    ]
    drawn_names = [name for _, rows in valid_pages for _, names in rows for name in names]
    if sorted(drawn_names) != sorted(by_name):
        raise ValueError("Valid-case classification must cover each seed exactly once")
    rows = invalid_rows(invalid["states"])
    index_pages = math.ceil(len(rows)/4)
    appendix_start = 10 + index_pages
    total = 9 + index_pages + math.ceil(len(pairs)/4)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    pdf = canvas.Canvas(str(args.output), pagesize=(W, H), pageCompression=1, invariant=1)
    pdf.setTitle("MeshUtil 双交点图册：合法模板与非法输入分类")
    pdf.setAuthor("MeshUtil")
    pdf.setSubject("23 valid templates; all 174 invalid encodings; seven legal complement comparisons")
    page = 1
    page_start(pdf, "双交点图册 / 先看输入是否合法", "阅读说明 / 256 = 82 + 174", page, total)
    bookmark(pdf, "阅读说明与分类", "guide")
    paragraph(pdf, "一个正方形面用 4 个角点位 + 4 个 pair 位编码。每次穿越都会翻转内外，两次穿越后回到原符号。", 36, 102, 768, 12, 19)
    label(pdf, 36, 122, "key = corners | (pairs << 4)：低四位记录角点内外，高四位记录各边是否请求两个交点。", 10, MUTED)
    headings = [(49, "边两端符号"), (256, "pair = 0"), (503, "pair = 1（请求两次穿越）")]
    pdf.setFillColor(color("#e2eef2")); pdf.roundRect(36, H-163, 770, 30, 5, fill=1, stroke=0)
    for x, value in headings:
        label(pdf, x, 153, value, 12, INK, True)
    for yy, values in [(191, ["同号", "0 个交点：合法", "2 个交点：合法"]),
                       (225, ["异号", "1 个交点：合法", "非法：两次翻转无法接到异号终点"])]:
        for (x, _), value in zip(headings, values):
            label(pdf, x, yy, value, 11.5, "#c53d42" if value.startswith("非法") else INK)
    label(pdf, 36, 269, "第一章  合法输入", 16, TEAL, True)
    paragraph(pdf, "82 个合法编码经面内旋转、镜像归并为 23 个代表模板。按内部角点数排列，展示每个模板的一种立方体延伸。", 36, 294, 646, 11.5, 18)
    label(pdf, 734, 288, "02-05 页", 11, TEAL)
    label(pdf, 36, 344, "第二章  非法输入", 16, "#c53d42", True)
    paragraph(pdf, "174 个输入按矛盾边数分成 104、60、8、2 个。先看四张分步解释图，再查全部编码矩阵；它们没有可绘制的合法曲面。", 36, 369, 646, 11.5, 18)
    label(pdf, 734, 363, f"06-{appendix_start-1:02d} 页", 11, "#c53d42")
    label(pdf, 36, 419, "附录  合法输入的内外翻转连接差异", 16, TEAL, True)
    paragraph(pdf, "7 组对照的两侧都属于合法输入。连接选择不同，不计入 174 个非法编码。", 36, 444, 646, 11.5, 18)
    label(pdf, 734, 438, f"{appendix_start:02d}-{total:02d} 页", 11, TEAL)
    paragraph(pdf, "图例：大实心角点=内，大空心角点=外，小圆=边交点，黄色=示意转折点；红色只标非法输入中的矛盾边。合法图中的不同线色区分闭环。", 36, 493, 765, 10.5, 17)
    paragraph(pdf, "合法立方体：同一面输入复制到 z=0 和 z=1，竖边无交点。几何位置仅作示意，不代表真实网格重建。", 36, 535, 765, 10, 16)
    pdf.showPage()

    for title, groups in valid_pages:
        page += 1
        page_start(pdf, title, "第一章 / 23 个代表模板；counts 按 e0,e1,e2,e3 排列", page, total)
        bookmark(pdf, title, f"valid-{page}")
        for row_index, (caption, names) in enumerate(groups):
            top = 98 + row_index*230
            label(pdf, 36, top-8, caption, 11, TEAL)
            for col_index, name in enumerate(names):
                svg_into(pdf, compact_case(by_name[name]), 36+col_index*260, top, 246, 212)
        if page == 4:
            paragraph(pdf, "按内部角点数分组后，再按角点相邻或对角、双交点边的位置区分模板。每张图都按源码六面查表结果绘制。", 565, 400, 230, 12, 22)
        pdf.showPage()

    for group, key in enumerate(invalid["representatives"], 1):
        page += 1
        title = f"非法输入 / {group} 条矛盾边，共 {invalid['counts_by_bad_edges'][str(group)]} 个编码"
        page_start(pdf, title, "第二章 / 红色边的端点异号，却同时请求两个交点", page, total)
        bookmark(pdf, title, f"invalid-example-{group}")
        paragraph(pdf, "先看线框中是哪一个面，再看面上哪些边自相矛盾，最后沿一条红边数两次穿越。", 36, 103, 770, 12, 20)
        svg_into(pdf, ROOT/f"doc/double_crossing/invalid/example-{group}.svg", 36, 125, 770, 362)
        paragraph(pdf, "只要有一条矛盾边，整个面输入就非法。多条矛盾边是同一条规则被重复违反；公开接口会抛出 std::invalid_argument。", 36, 513, 770, 11.5, 19)
        pdf.showPage()

    for offset in range(0, len(rows), 4):
        page += 1
        subset = rows[offset:offset+4]
        groups = sorted({r[0] for r in subset})
        category = "、".join(map(str, groups))
        page_start(pdf, f"非法编码索引 / {category} 条矛盾边", "第二章 / c0 左下，c1 右下，c2 右上，c3 左上；e0 起沿角点顺序；红色为矛盾边", page, total)
        bookmark(pdf, f"非法编码索引 {offset//4+1:02d}", f"invalid-index-{offset//4+1}")
        body = ""
        for row_index, (group, start, count, items) in enumerate(subset):
            yy = row_index*116
            body += text(0, yy+10, f'{group} 条矛盾边 / 本类 {count} 个 / 第 {start+1}-{start+len(items)} 个', 10, "#a63238")
            for col_index, state in enumerate(items):
                body += thumbnail(state, col_index*154, yy+15, width=150, height=100)
        svg_into(pdf, svg_document(770, 464, body, "非法输入完整分类索引"), 36, 88, 770, 464)
        pdf.showPage()

    for offset in range(0, len(pairs), 4):
        page += 1
        page_start(pdf, "附录 / 内外翻转后的连接差异", "附录 / 两侧都合法；共 7 对，按旋转与镜像可归为 3 类", page, total)
        bookmark(pdf, f"合法翻转对照 {offset+1}-{min(offset+4, len(pairs))}", f"appendix-{offset}")
        paragraph(pdf, "固定边交点位置，仅翻转所有角点的内外位。配对可能改变，但两侧都满足穿越次数规则。", 36, 100, 770, 11.5, 18)
        for index, pair in enumerate(pairs[offset:offset+4]):
            svg_into(pdf, complement_tile(pair), 36+(index%2)*394, 120+(index//2)*220, 376, 204)
        if offset == 4:
            label(pdf, 447, 381, "与非法输入的区别", 16, TEAL)
            paragraph(pdf, "这些是两个不同的合法输入，各自有确定的查表结果。174 个非法编码则连端点符号与交点数都无法同时满足。", 447, 414, 326, 12, 22)
            paragraph(pdf, "黄色点是面内转折点。完整立方体对照、交互图和可单独下载的 SVG 均保留在仓库中。", 447, 503, 326, 10.5, 17)
        pdf.showPage()
    if page != total:
        raise ValueError(f"Page count mismatch: {page} != {total}")
    pdf.save()
    print(f"Created {args.output}: {page} pages; 23 valid cubes + 174 invalid inputs + 7 legal complement pairs; vector graphics.")


if __name__ == "__main__":
    main()
