# 双交点连接 PDF 图册

[打开 / 下载 PDF](MeshUtil_double_crossing_cases_and_conflicts.pdf)

图册共 **21 页**，使用可缩放的矢量图形，包含：

- 第 1 页：阅读说明、符号和示例几何约定。
- 第 2-13 页：全部 23 个命名代表 case，每页最多两个立方体。
- 第 14 页：冲突定义与全部 7 组对照的页码索引。
- 第 15-21 页：7 组内外翻转对照，覆盖全部 14 个连接配对改变的面状态；每页包含两个立方体及对应二维面。

这里的冲突表示两个互补输入的配对选择不同；交点位置固定，但角点内外状态翻转。
图册包含 23 个代表面模板的立方体延伸及 7 组对照，不是对 36,450 个立方体编码的逐一枚举。
交点和黄色转折点的位置为示意坐标，不代表 GT mesh 的重建。

## 重新生成

在仓库根目录运行：

```bash
python tools/render_double_crossing_cases.py
python tools/render_double_crossing_conflicts.py
python -m pip install reportlab
python tools/build_double_crossing_pdf.py
```

PDF 生成器直接读取前两步的 SVG 和 JSON，保留矢量线条、文字与透明填充。
默认查找本地中文 TrueType 字体（Windows 的 SimHei / Microsoft YaHei，或 Linux 的 WenQuanYi）；
也可传入 `--cjk-font /path/to/font.ttf`。找到本地字体时会嵌入子集，未找到时使用标准中文 CID 字体。
相同输入、字体和 ReportLab 版本下输出确定性 PDF。

图形数据检查：

```bash
python tools/generate_double_crossing_tables.py --check
python tools/render_double_crossing_cases.py --check
python tools/render_double_crossing_conflicts.py --check
```

本版已逐项核对全部 7 对的端点配对、黄色点、模板归属和旋转/镜像分类，并渲染检查全部 21 页。
