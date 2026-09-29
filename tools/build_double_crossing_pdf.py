#!/usr/bin/env python3
"""Build the vector PDF atlas from the committed case and conflict diagrams.

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


def svg_into(pdf, path, x, y, width, height):
    """Render the small, explicit SVG vocabulary emitted by our generators.

    Unsupported tags/transform functions fail loudly instead of silently
    disappearing from the PDF. y is measured down from the page's top.
    """
    root = ET.parse(path).getroot()
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


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output",type=Path,default=DEFAULT_OUTPUT)
    parser.add_argument("--cjk-font",type=Path)
    args=parser.parse_args()
    setup_font(args.cjk_font)
    cases=json.loads((ROOT/"doc/double_crossing/cases/cases.json").read_text(encoding="utf-8"))["cases"]
    conflict_data=json.loads((ROOT/"doc/double_crossing/conflicts/conflicts.json").read_text(encoding="utf-8"))
    pairs=conflict_data["pairs"]
    if len(cases)!=23 or len(pairs)!=7:
        raise ValueError("Expected all 23 representative cases and all 7 complement pairs")
    total=1+math.ceil(len(cases)/2)+1+len(pairs)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    pdf=canvas.Canvas(str(args.output),pagesize=(W,H),pageCompression=1,invariant=1)
    pdf.setTitle("MeshUtil 双交点连接图册：23 个代表 case 与 7 组内外翻转对照")
    pdf.setAuthor("MeshUtil")
    pdf.setSubject("Source-backed face templates, cube examples and complement-pairing differences")
    page=1
    page_start(pdf,"双交点连接图册","阅读说明 / 2026-09-29",page,total)
    bookmark(pdf,"阅读说明","guide")
    label(pdf,36,106,"23 个代表 case + 7 组内外翻转对照",18,TEAL)
    paragraph(pdf,"按项目源码查表规则绘制，所有图形均为可缩放矢量。",36,133,750,12)
    svg_into(pdf,ROOT/"doc/double_crossing/cases/case-3d.svg",36,171,355,305)
    label(pdf,427,190,"如何读图",16,INK,True)
    y=paragraph(pdf,"实心角点表示内部，空心角点表示外部；小圆点是边交点；黄色点是面内转折点。彩色线区分闭合边界环，半透明面表示示意曲面。",427,220,370,12,20)
    y=paragraph(pdf,"每个二维面复制到 z=0 与 z=1，四条 z 方向边均无交点，六面查表后形成完整立方体。这是面模板的一种三维延伸。",427,y+14,370,12,20)
    paragraph(pdf,"23 是按旋转/镜像归并后的面模板数。展开后有 82 个合法面状态，完整立方体有 36,450 个合法编码。",427,y+14,370,12,20)
    paragraph(pdf,"坐标约定：单交点 t=0.5，双交点 t=0.32/0.68，黄色点向面内偏移 0.20。几何为示意，不是 GT mesh 重建。",36,510,760,10.5,17)
    label(pdf,36,546,"github.com/crazyMessi/MeshUtil",10,TEAL)
    pdf.linkURL("https://github.com/crazyMessi/MeshUtil",(36,H-550,250,H-534),relative=0)
    pdf.showPage()

    for offset in range(0,len(cases),2):
        page+=1
        subset=cases[offset:offset+2]
        title="代表模板 / "+" + ".join(c["name"] for c in subset)
        page_start(pdf,title,"23 个面模板各自延伸的立方体示例",page,total)
        bookmark(pdf,"Case "+" / ".join(c["name"] for c in subset),f"cases-{offset}")
        for index,case in enumerate(subset):
            x=36+index*393
            svg_into(pdf,ROOT/f'doc/double_crossing/cases/case-{case["name"]}.svg',x,100,372,319)
            label(pdf,x+12,452,f'面角点掩码 {case["face_corners"]:04b}  |  双交点掩码 {case["face_pairs"]:04b}',11)
            label(pdf,x+12,477,"闭环节点数："+(" / ".join(str(len(l)) for l in case["loops"]) or "0"),11)
            note="全部内部；没有边界环。" if case["name"]=="12a" else "全部外部；没有边界环。" if case["name"]=="1a" else "黄色点为示意转折位置。" if any(int(n)>=24 for n in case["nodes"]) else "连接和节点编号可在交互浏览页中对照。"
            paragraph(pdf,note,x+12,505,342,10.5,17)
        if len(subset)==1:
            label(pdf,454,206,"下一节：连接冲突对照",18,TEAL)
            paragraph(pdf,"固定边交点位置，将所有角点内外状态翻转，再比较两次查表结果。全部 7 组变化会逐组列出。",454,243,330,13,22)
        pdf.showPage()

    page+=1
    page_start(pdf,"内外翻转时，哪些连接会改变？","7 组对照覆盖全部 14 个冲突状态",page,total)
    bookmark(pdf,"冲突索引与定义","conflict-index")
    paragraph(pdf,"这里的“冲突”指：固定面坐标与边交点位置，将四个角点的内外位全部翻转后，查表选择的无向配对改变。两侧是不同输入，不是同一个输入返回了两个矛盾答案。",36,99,764,12,20)
    paragraph(pdf,"全部 14 个状态均为两内两外，合为 7 对，再按旋转/镜像归成 3 类。每对闭环数量相同，仍需比较连接到哪些交点。",36,157,764,12,20)
    headings=[(48,"对照"),(178,"面编码 A / B"),(366,"边交点数 e0,e1,e2,e3"),(600,"两侧闭环数"),(723,"页码")]
    pdf.setFillColor(color("#e2eef2"));pdf.roundRect(36,H-229,770,31,5,fill=1,stroke=0)
    for x,t in headings: label(pdf,x,219,t,10.5,INK,True)
    for i,pair in enumerate(pairs):
        yy=251+i*32
        if i%2==0:
            pdf.setFillColor(colors.white);pdf.rect(36,H-yy-10,770,31,fill=1,stroke=0)
        a,b=pair["case_a"],pair["case_b"]
        values=[(48,f'{i+1:02d}'),(178,f'0x{pair["key_a"]:02X} / 0x{pair["key_b"]:02X}'),
                (366,str(a["face_counts"])),(600,f'{len(a["loops"])} / {len(b["loops"])}'),(723,str(page+i+1))]
        for x,t in values:label(pdf,x,yy,t,11)
    paragraph(pdf,"规则原因：当前策略在 0/1/2 个内部角点时配对外部边界区间，在 3/4 个内部角点时配对内部区间。两内两外翻转后仍有两个内部角点，所选区间可能改变。",36,505,760,10.5,17)
    pdf.showPage()

    for i,pair in enumerate(pairs):
        page+=1
        title=f'冲突对照 {i+1:02d} / 0x{pair["key_a"]:02X} 与 0x{pair["key_b"]:02X}'
        page_start(pdf,title,"固定交点位置，翻转全部角点内外位；配对变化，环数量不变。",page,total)
        bookmark(pdf,title,f"conflict-{i+1}")
        svg_into(pdf,ROOT/f'doc/double_crossing/conflicts/{pair["name"]}.svg',36,81,770,477.4)
        pdf.showPage()
    pdf.save()
    print(f"Created {args.output}: {page} pages; 23 case diagrams + 7 conflict comparisons; vector graphics.")


if __name__=="__main__":
    main()
