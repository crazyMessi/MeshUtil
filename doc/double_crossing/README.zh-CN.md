# 双交点网格连接模块

[完整接口与编码说明（English）](README.md)

[23 个 case 的立方体示例](cases/README.md) · [交互浏览页](cases/index.html) · [全部示例总览](cases/overview.svg)

[174 个非法输入图例](invalid/README.md) · [7 组内外翻转后的连接差异](conflicts/README.md) · [下载完整 PDF 图册](../../output/pdf/MeshUtil_double_crossing_cases_and_conflicts.pdf)

图册按“合法输入”“非法输入”两章组织，附录集中展示内外翻转后的连接差异。
174 个非法编码按矛盾边数分为 1 / 2 / 3 / 4 条四类，数量分别为 **104 / 60 / 8 / 2**。
附录的 7 组对照两侧都是合法输入，与这 174 个非法编码是两回事。

## 定位

这是一个面向粗网格曲面重建的 **C++17 连接关系模块**，位于“mesh 与网格求交”和“生成三角形”之间。它接受角点内外状态，以及每条 grid edge 上 0、1、2 个不同交点的信息，输出网格面上的连接路径和 cube 的闭合边界环。

它与 MeshUtil 的减面器是两个独立工具。当前模块提供拓扑连接这一步，不是输入 GT mesh 就能直接得到重建 mesh 的完整程序。

## 解决什么问题

薄片可能在同一条网格边中间被穿过两次，而边的两个端点都在外部：

```text
外部 ---- p0 ---- 内部 ---- p1 ---- 外部
```

只依赖角点符号的普通 Marching Cubes 看不见这对交点，薄片可能消失。本模块在不细分网格的前提下，把 p0、p1 保留为不同节点，并给出面内连接规则。

前提是调用方已经知道这两个交点，例如通过 GT mesh 与网格边求交得到。仅有 SDF 角点值，无法凭本模块恢复未被采样记录的交点。

## 已提供的能力

- 单头文件入口：`#include <meshutil/double_crossing.hpp>`。
- CMake 目标：`MeshUtil::double_crossing`，支持源码引用与安装后 `find_package`。
- 面查表：覆盖 82 个符号与交点数相容的状态，另外 174 个编码明确拒绝。
- cube 闭环：组合六个面的路径，支持全部 36,450 个符号与交点数相容的编码。
- 两种等价表格式：默认 1,024 B 直接查询表，以及 348 B 对称压缩表。使用两种模式的程序可能同时保留两套数组。
- 本地独立的表生成脚本、提交进仓库的表、示例和穷举测试；运行时只依赖 C++ 标准库。
- **一条 edge 超过两个交点会抛出异常，不会默认合并、丢弃或取最外两个点。**

## 为什么有 174 个非法编码

一个面用 4 个角点位与 4 个双交点位，共 8 位编码，因此共有 256 种位组合。
双交点位为 1 表示该边有两个交点。每穿越一次曲面，内外状态就翻转一次，
所以两次穿越后会回到原来的状态：**端点异号，却要求两个交点，就是输入矛盾**。

| 边的端点符号 | 双交点位为 0 | 双交点位为 1 |
| --- | --- | --- |
| 相同 | 0 个交点，合法 | 2 个交点，合法 |
| 相反 | 1 个交点，合法 | 非法 |

256 个编码中，82 个符合规则，剩余 **256 - 82 = 174** 个会被 API 以
`std::invalid_argument` 拒绝。它们不是等待实现的合法 case。
[非法输入图集](invalid/README.md) 展示全部编码及其中的矛盾边。

## 最小示例

```cpp
#include <meshutil/double_crossing.hpp>
#include <array>

namespace dc = meshutil::double_crossing;

// 8 个角点都在外部；4 条 x 方向边各穿过薄片两次。
std::array<unsigned, 12> counts{2,2,2,2, 0,0,0,0, 0,0,0,0};
auto result = dc::cube_boundary_loops_from_counts(0, counts);
// result.loops 是两个独立的四节点环，对应薄片的两侧。
```

这里输出的是符号节点和连接关系。边节点的实际位置需要从原始交点列表中取出，环内的三角形也需要之后生成。

```bash
cmake -S . -B temp_output/build -DCMAKE_BUILD_TYPE=Release -DMESHUTIL_BUILD_EXAMPLES=ON
cmake --build temp_output/build --parallel
ctest --test-dir temp_output/build --output-on-failure -j 1
./temp_output/build/double_crossing_example
```

## 黄色点是什么

当一段曲线的两个端点位于同一条网格边时，直接连线会退化到这条边上。表中的黄色标记表示曲线要经过网格 face 内部的转折点，以保留薄片前端。

有 GT mesh 时，可从 **GT mesh 的边与网格 face 的交点**中寻找候选转折点。这个模块只给出“需要转折”的标记，不选择坐标；一个标记也可以展开成多个几何点。相邻 cell 必须使用相同的物理交点和黄色点，才能一致拼接。

## 不包括什么

当前没有实现 mesh/grid 求交、重复命中去重、切触及网格角点退化处理、内外分类、黄色点选取、三角化、法线或文件导出，也没有 CUDA kernel。调用方需把这些步骤接在模块前后。

闭环成立不等于任意 GT 的拓扑被正确保留，也不保证生成的三角形无自交或构成流形。完全位于 cell 内、没有穿过任何网格边的分量也不在编码中。论文的局部薄层假设和连接歧义仍然限制适用范围。

## 来源与验证

连接规则独立重建自 Müller 2009 年论文 [Fast and Robust Tracking of Fluid Surfaces](https://matthias-research.github.io/pages/publications/surfaceTracking.pdf) 的 Figure 5、6 和 §3.1–3.3，并非作者原始源码。

采用论文选择的第 1、2、3、4、11、12 列，共 23 个代表模板；通过旋转、镜像展开。内外翻转不作为压缩对称性，因为它会改变部分状态的连接选择。

测试检查全部 256 个面编码、两种压缩格式、旋转/镜像的有向连接，以及所有 36,450 个相容 cube 编码的交点覆盖和闭环结构。这是离散结构验证；还没有完整真实 mesh 重建测试或 GPU 性能基准。

生成表与检查可复现性：

```bash
python3 tools/generate_double_crossing_tables.py
python3 tools/generate_double_crossing_tables.py --check
```
