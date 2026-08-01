# Blender 严格等价减面器开发总结

## 目标与结论

本项目提供一个不依赖 Blender、`bpy` 或 Blender Python 运行时的 C++17 三角网格减面器。实现目标不是仅获得相似几何，而是在固定输入协议下复现 Blender 4.0.2 Decimate Collapse 的结果，包括最终 PLY 文件的逐字节 SHA-256。

参考版本固定为 Blender 4.0.2 commit：

```text
9be62e85b7270d3d2e5bcc846420b91bab3988f9
```

独立实现的验收源码来自：

```text
f681987c3e6615790499297245f4a8ace8501290
```

当前仓库首次发布提交为：

```text
b1325cde8ece2664725fa6db4cd3ff7f2f730700
```

固定 128 件正式集合中，每件均独立减到请求的 300 万面。连续处理时间不包含事后的 SHA-256 和 PLY 校验：

| Worker | 连续处理墙钟 | 吞吐 | 相对同并行 Blender | SHA-256 | 峰值内存 | OOM |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 8 | 74.48 s | 6186.89 件/小时 | 4.42x | 128/128 | 58.05 GiB | 0 |
| 16 | **38.25 s** | **12047.04 件/小时** | 4.34x | 128/128 | 69.71 GiB | 0 |
| 32 | 40.73 s | 11314.58 件/小时 | 4.23x | 128/128 | 86.46 GiB | 0 |

当前推荐使用 16 worker。8 到 16 worker 的吞吐扩展约为 1.95x；32 worker 受内存带宽和并发开销影响，墙钟回退到 40.73 秒。

所有三档共 384 份输出均与 Blender Baseline 逐文件 SHA-256 一致，输出面数范围为 2,999,999 到 3,000,000。三档 hash manifest 的 SHA-256 均为：

```text
6dc47d31543a09eed0e0694c4d437c2f0485a6ab10580e52517a3ff017d6fda9
```

## 为什么不能直接并行单个 Mesh

Blender Collapse 减面使用全局最小堆选择下一条待折叠边。一次 collapse 会修改：

- 顶点位置、法线和 quadric；
- BMesh disk/radial 拓扑关系；
- 相邻边的 cost；
- 堆中边的位置及之后的并列顺序。

因此，单个 mesh 内部并行 collapse 会改变全局边选择顺序，最终几何通常也会变化。严格等价场景下，可靠的扩展方式是：

1. 保持单个 mesh 串行；
2. 在多个独立 mesh 之间使用多进程并行；
3. 固定 worker pool；
4. 绑定物理核并跨 NUMA 节点交错分配；
5. 限制并发数，避免内存带宽和 cgroup 内存上限成为瓶颈。

`standalone_batch_runner` 实现了这一连续处理模型，并将减面处理墙钟与事后 SHA/PLY 验收时间分开统计。

## 严格一致的关键实现

### 1. 初始 Edge Map 顺序

最早的误区是认为只要边集合相同，edge ID 如何分配并不重要。实际上，Blender 后续的 disk、radial 和 heap 顺序都依赖初始 edge ID。

实现必须复现 Blender 4.0.2 `BKE_mesh_calc_edges` 的 Map 行为：

- 边端点规范化为 `v_low, v_high`；
- hash 为 `(v_low << 8) ^ v_high`；
- 小于 1000 个 face 使用 1 个 Map；
- 大于等于 1000 个 face 使用 8 个 Map；
- 分桶规则为 `v_low & 7`；
- 每个 Map 按 `faces * 2 / map_count` 预留；
- 维持 50% load factor 和 2 的幂 slots；
- probing 先执行 `perturb >>= 5`，再执行 `hash = 5 * hash + 1 + perturb`；
- 每个 face 从最后一条边向第一条边遍历；
- 最终按 Map 编号、slot index 升序序列化。

使用普通 `unordered_map`、按首次出现顺序编号，或者对边排序，都会改变 collapse 序列。

### 2. BLI Heap 的并列行为

Blender 的 `BLI_heap` 只比较 cost，不使用 edge ID 作为 tie-break。需要复现的细节包括：

- 相等 cost 插入时继续向上交换；
- `sift_down` 只使用严格 `<`；
- 左右子节点按 left、right 顺序比较；
- `remove` 将目标节点逐层交换到根，再执行 `pop_min`；
- `update` 只在新 cost 严格小于或严格大于旧 cost 时写入；
- 相等、`+0/-0` 或 NaN/unordered 时保持不变。

如果改成 `(cost, edge_id)` 排序，虽然看起来更稳定，但会与 Blender 不一致。

Version 4 将逐层 swap 优化为 hole relocation，减少 heap 和 position 数组的重复写入，但保持最终数组排列和上述所有比较语义不变。除了完整网格 SHA 验证外，还执行了 100,000 步随机 heap 操作级对照，覆盖 equal-cost、`+0/-0`、无穷、NaN、insert、update、remove 和 pop。

### 3. BMesh Disk/Radial 顺序

BMesh 不能简化成无序邻接集合。严格一致依赖：

- vertex disk cycle 的 head、next、previous；
- edge radial cycle 的 head、next、previous；
- append、remove 和 splice 的具体顺序；
- collapse 后 incident edge 和 outer edge 的 heap update 顺序。

开发早期曾出现：

```text
internal error: edge radial loop lost source vertex
```

根因不是几何公式，而是 vertex splice 时更新 loop 顶点、disk 和 radial 的先后顺序与 Blender 不同。最终实现保持稳定、永不重编号的 32 位 vertex、edge、face 和 loop slot，并逐步复现 BMesh 的修改顺序。

### 4. 拓扑退化判定

collapse 不能只检查边是否 manifold。Blender 的拓扑门槛要求：

- collapse 边有 1 或 2 个 radial face；
- 两个端点的所有 incident edge 都必须有效且最多有 2 个 face；
- 两端点去掉彼此后的共享邻居集合；
- 必须与 collapse 边 radial face 中的 opposite vertex 集合完全相等。

这是真正的集合相等语义，需要正确处理重复邻居和重复 opposite vertex。Version 4 用预分配的 generation stamp 数组替代每个候选边上创建多个 `std::set`，但仍保持去重、交集和集合相等语义。

### 5. Flip 与近退化面判定

对 collapse 两端关联的每个保留 face，需要以 float32 中间值复现 Blender 的 cross/dot 判定。以下变化都可能导致边被接受或拒绝的结果不同：

- 调换 cross product 的操作数；
- 使用 double 保存所有中间值；
- 改变加法结合顺序；
- 使用不同 epsilon；
- 允许编译器融合乘加。

构建使用 `-ffp-contract=off`，避免 FMA 改变舍入。

### 6. QEM 浮点边界

Quadric 系数和主求解过程使用 double，但 Blender 会在特定边界回到 float：

- 顶点位置和法线来自 float；
- heap cost 是 float；
- normal alignment 和 edge length fallback 使用 float；
- optimized position 在更新拓扑和插值前按 Blender 路径转换；
- singular solve epsilon、boundary weight 和 topology fallback epsilon 固定。

不能简单地认为“全用 double 更准确”就会更接近 Blender。严格一致关注的是相同舍入路径，而不是单步数值精度更高。

### 7. 目标面数取整

Blender modifier 先计算 float ratio，再乘回输入面数。因此请求 3,000,000 面时，少数网格的有效停止目标会变成 2,999,999。

实现复现了：

```text
ratio = float(double(requested_faces) / double(input_faces))
effective_target = size_t(float(input_faces) * ratio)
```

如果直接使用整数 requested target，tiny corpus 和正式集合都会出现分叉。

### 8. 重复面和零面积面

Blender 的 Mesh-to-BMesh 路径会接受部分重复面、反向重合面和零面积面。提前清理这些面会改变：

- 初始 edge map；
- radial face 数量和顺序；
- vertex normal 与 quadric；
- topology gate；
- 最终 collapse 序列。

因此，输入读取器只拒绝协议明确不支持的数据，不擅自做几何修复或去重。

### 9. PLY 逐字节输出

几何等价不等于文件 SHA 等价。最终输出还必须保持：

- `binary_little_endian 1.0`；
- `comment Created in Blender version 4.0.2`；
- 顶点属性为 `float x/y/z`；
- face 属性为 `property list uchar uint vertex_indices`；
- 顶点和面顺序；
- `+0.0` 的符号位行为；
- header 换行和二进制 payload 布局。

输出通过同目录临时文件写入，再原子 rename，避免失败任务留下可被误判为成功的半文件。

## Version 4 性能优化

Version 4 不改变算法决策，只优化热路径中的内存访问和分配。

### Topology Generation Stamp

原实现每个候选边会构造多个 `std::set` 并执行 `set_intersection`。Version 4 使用每顶点 generation stamp：

- 标记第一个端点的邻居；
- 第二个端点将共享邻居转换为 shared 状态；
- radial opposite vertex 消费 shared 状态；
- 最终检查是否存在未匹配 shared 邻居。

这样消除了大量红黑树节点分配，同时保持集合语义。

### Collapse Scratch Buffer

以下短生命周期容器改为 `QemDecimator::Impl` 级复用缓冲：

- collapse radial loops；
- splice edge pairs；
- vertex face fan loops。

复用只减少分配，不改变元素插入顺序、遍历顺序、splice 顺序或 heap update 顺序。

### Heap Hole Relocation

原 heap 调整每层使用 swap，会反复写入两个 heap entry 和两个 position。hole relocation 沿相同路径移动父或子节点，最后一次写入被移动 entry：

- 最终 heap 排列不变；
- 每层 position 写入减少；
- equal-cost、NaN 和 remove-to-root 语义不变。

## 验证方法

严格一致不是通过少量视觉对比确认，而是分层验证：

1. 固定 Blender 4.0.2 commit 和运行参数；
2. tiny differential corpus 12/12 最终 PLY SHA-256；
3. 7 个正式异常样本 7/7 SHA-256；
4. heap 新旧实现 100,000 步操作级对照；
5. 固定 128 件正式集合；
6. 8、16、32 worker 三档严格串行运行；
7. 每档 128/128 成功；
8. 每份输出 SHA-256 等于 Blender Baseline；
9. 核对面数范围、缺失输出和 cgroup OOM kill delta；
10. 将 SHA/PLY 验收时间与连续处理墙钟分开统计。

性能测试必须串行执行不同版本和不同 worker 档位，避免多个测试相互争抢 CPU、内存带宽和 page cache。

## 使用与适用边界

该实现针对 geometry-only 的 strict binary triangle PLY：

- 顶点仅包含 `float x/y/z`；
- face 必须是三角形；
- face index 支持受控的 32 位有符号或无符号声明；
- 不支持 UV、颜色、材质、shape key、vertex group、symmetry、delimiter、triangulation/rejoin 或自定义权重。

严格兼容性固定在 Blender 4.0.2 和当前导入、modifier、导出设置。升级 Blender 版本或扩展输入属性后，必须重新做 differential trace 和正式 SHA 验收。

构建、安装和批处理命令见仓库根目录 `README.md`。
