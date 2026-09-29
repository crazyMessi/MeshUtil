# 双交点连接分类图册

[打开 / 下载 PDF](MeshUtil_double_crossing_cases_and_conflicts.pdf)

图册以“输入是否合法”为主线，使用可缩放矢量图形，集中组织为三部分：

1. **第一章：编码规则与合法 case。** 说明角点、边交点和内外变化规则；23 个命名代表模板按内部角点数分为 0 个、1 个、2 个相邻、2 个对角、3 个、4 个六组。
2. **第二章：174 个非法输入。** 按 1 / 2 / 3 / 4 条矛盾边分组，分别包含 **104 / 60 / 8 / 2** 个编码。每类先解释典型例子，再用矩阵列全该类编码。
3. **附录：内外翻转后的连接差异。** 集中展示 7 对互补输入；两侧均合法，覆盖 14 个连接配对改变的面状态。

共 **20 页**，页码导航：

| 页码 | 内容 |
| --- | --- |
| 1 | 编码规则、图例与目录 |
| 2-5 | 全部 23 个合法代表模板的立方体 |
| 6-9 | 四类非法输入的分步解释：立方体定位、局部面、矛盾边 |
| 10-18 | 全部 174 个非法编码，按矛盾边数连续排列 |
| 19-20 | 7 组合法输入的内外翻转连接对照 |

**非法输入**指同一条边的端点符号相反，却将双交点位设为 1。
两次穿越会翻转内外状态两次，因此端点必须同号。这 174 个编码会被 API 拒绝，
不是未实现的合法 case。附录只说明内外翻转会改变部分合法输入的连接选择，
不属于这类输入矛盾。

图册中的立方体用于定位源面，合法 case 则沿 z 方向延伸为立方体示例。
它不是对 36,450 个立方体编码的逐一枚举；交点与黄色转折点为示意坐标，
不代表 GT mesh 的实际重建结果。非法输入不能生成相容曲面。

## 配套图集

- [23 个合法代表模板](../../doc/double_crossing/cases/README.md)
- [174 个非法输入及典型图](../../doc/double_crossing/invalid/README.md)：[1 条矛盾边](../../doc/double_crossing/invalid/group-1.svg) · [2 条](../../doc/double_crossing/invalid/group-2.svg) · [3 条](../../doc/double_crossing/invalid/group-3.svg) · [4 条](../../doc/double_crossing/invalid/group-4.svg)
- [7 组内外翻转后的连接差异](../../doc/double_crossing/conflicts/README.md)

## 重新生成

在仓库根目录运行：

```bash
python tools/render_double_crossing_cases.py
python tools/render_double_crossing_invalid.py
python tools/render_double_crossing_conflicts.py
python -m pip install reportlab
python tools/build_double_crossing_pdf.py
```

PDF 生成器读取图集数据并复用同一套绘图函数，保留矢量线条、文字与透明填充。
默认查找本地中文 TrueType 字体（Windows 的 SimHei / Microsoft YaHei，或 Linux 的 WenQuanYi）；
也可传入 `--cjk-font /path/to/font.ttf`。找到本地字体时会嵌入子集，未找到时使用标准中文 CID 字体。
相同输入、字体和 ReportLab 版本下输出确定性 PDF。

图形数据检查：

```bash
python tools/generate_double_crossing_tables.py --check
python tools/render_double_crossing_cases.py --check
python tools/render_double_crossing_invalid.py --check
python tools/render_double_crossing_conflicts.py --check
```

保留 PDF 文件名及 `conflicts/` 目录是为了兼容已有链接；它们不改变正文对非法输入和合法连接差异的区分。
