# AutoRemesher 提速改造记录

> 基线：本机 MSVC 19.51 (VS18) / Ninja / Release（`/O2 /GL /Qpar /fp:fast /LTCG`），与原 `autoremesher.pro` 的 MSVC 选项一致。
> 测试素材：`E:/ComfyUI-aki-v3/zcode_work/char3d/remesh/*.obj`（body_vox / head_in / head_clean）。
> 对照方式：`git archive HEAD` 出的**原始源码**用同一份 bench 工程、同一编译器编出来的二进制，与改造后二进制逐模型对比。

---

## 1. 结果总览

| 模型 | 参数 | 原始 (s) | 改造后 (s) | 加速 |
|---|---|---|---|---|
| body_vox.obj（78k 三角，1 岛） | 15k quads, adaptivity 0.3 | 8.276 | **3.61** | **2.29×** |
| head_in.obj（400k 三角，**8979 岛**） | 10k quads, adaptivity 0.5 | 12.739 | **4.57** | **2.79×** |
| head_clean.obj（400k 三角，1 岛） | 20k quads, adaptivity 0.3 | 41.182 | **9.20** | **4.48×** |

（`--repeat 2` 取最好值；三次连续运行输出逐位一致：head_clean 恒为 13091 quads / 395 non-quads / 13571 顶点。）

输出质量：非四边形占比维持 ~2%（body_vox 98→99 / 8433→8434 顶点；head_in 364→357）。四边形总数在 ±4% 内浮动——这条流水线本身对浮点/迭代顺序敏感，同一份源码换编译器/改迭代顺序即可产生同等幅度差异（流水线自己那份 release 报告是 14303，而 HEAD 源码本机编译是 12601）。

**并行度没有变**：仍是"岛级 TBB 并行 + 阶段内数据并行"，单岛模型 cores-busy 依旧 1.00。这轮全部收益来自**去掉热点路径上的重复计算与内存分配**。

---

## 2. 逐项改动（按收益排序）

### 2.1 奇异点电荷：全网格重算 → 增量维护（-1.07 s @ body_vox）

`src/AutoRemesher/singularitysimplifier.{h,cpp}`

**问题**：`cancelPair` 每评估一对候选点就要 `vertexCharges()` 4 次，每次都遍历**所有顶点 × 其角点扇**（O(V·6)）。1000 个奇异点、259 对消去、数百次候选评估 → 约 2.4 亿次角点访问，`Simplifying singularities` 占 1206 ms（占 parameterize 一半）。

**改法**：电荷只取决于绕顶点的 90° 失配之和。新增
- `m_chargeSum[v]`（原始和，不取模）、`m_chargeClosed[v]`（扇是否闭合，纯拓扑，建一次）；
- `writeMismatch(corner, value)`：写失配的同时 `m_chargeSum[cornerVertex] += value - previous`；
- `vertexCharges()` 只做 `sum % 4` 与闭合判定，O(V) 且无分配。

因为差异是按写入点增量更新的，结果与全量重算**逐位相同**（开发期用 `AR_VERIFY_CHARGES=1` 逐次比对，0 处不一致）。

**效果**：1206 ms → **135 ms**（8.9×）。

### 2.2 缓存原网格 AABB 树（-0.15 s，并解锁后续）

`src/AutoRemesher/quadextractor.{h,cpp}`

**问题**：`smoothAndProject` 每次调用都重建**整个源网格**的包围盒树（78k~400k 个盒子 + BVH）。而它在 12 个修缮 pass 里每个成功操作后都会被调用一次（body_vox 合计 198 次）。

**改法**：`projectionTree()` 惰性建一次并保存在 `QuadExtractor` 内（源网格是 const，树可终身复用）。

### 2.3 顶点→面索引复用（-0.7 s）

`quadextractor.cpp: smoothAroundVertices`

**问题**：每次调用都新建 `unordered_map<size_t, vector<size_t>>` 并往每个顶点桶里 push —— 8k~14k 个顶点 × 每次分配一个小 vector，198 次调用 ≈ 200 万次小分配。

**改法**：改成成员 `m_vertexFaceScratch`（`vector<vector<size_t>>`），每次只 `clear()` 复用容量。

**效果**：三个修缮 pass 合计 2.9 s → 1.95 s。

### 2.4 热点容器换成哈希（-0.6 s 累计）

| 位置 | 原 | 改 | 说明 |
|---|---|---|---|
| `m_connectionInfos` | `std::map<Edge, ConnectionInfo>` | `unordered_map` + `EdgePairHash` | `holdSingularLines` 每个饥饿锥遍历全部连接，原 32 锥 × 60k 边 × log n |
| `localEdges` | `std::map` | `unordered_map` | 同上 |
| `crossPointMap` | `std::map<PositionKey, size_t>` | `unordered_map` + `PositionKeyHash`（量化整数三元组哈希） | 每个等值线交点一次查找；`PositionKey` 新增 `intX/Y/Z()` |
| 修缮 pass 的 `edgeFaces`（10 处） | `std::map<Edge, vector<size_t>>` | `unordered_map` + `CleanupEdgeHash` | 每个成功操作重建一次 |
| `m_halfEdges` | `std::set<Edge>` | `unordered_set` + 哈希 | `rebuildHalfEdges` 每个成功操作重建；`fixHoles` 里再拷一份有序 set 给 `searchBoundaries` 以保证环发现顺序不变 |
| 面唯一性集合（6 处） | `std::set<std::vector<size_t>>` + `canonicalFace()`（每面 8~14 次 vector 分配） | `unordered_set<uint64_t>` + `canonicalFaceHash()`（旋转/镜像不变哈希，无分配） | 碰撞方向只会让 pass 更保守地拒绝候选，不会接受非法面 |
| `rejectedEdges` / `rejectedDiagonals` | `std::set<Edge>` | `unordered_set` + 哈希 | |
| `cleanupTriangles` 的 route 走线 | 每个候选 2 个 `std::set` + 2 个 `std::vector` 分配 | 定长数组 + 复用 vector | 每轮 600 次候选评估 |

**效果**：`Holding singular lines` 584 → 312 ms；`Extracting connections` 132 → 95 ms；三个修缮 pass 各再降 10~20%。

---

## 3. 测试与复现

```bat
:: 1) 构建 vendored TBB 2017（静态，一次即可）
build\build_tbb.bat

:: 2) 构建 headless 基准程序（不需要 Qt）
build\build_bench.bat

:: 3) 跑基准（与流水线同样的参数口径）
build\bench\arbench.exe --input E:\ComfyUI-aki-v3\zcode_work\char3d\remesh\body_vox.obj ^
  --target-quads 15000 --adaptivity 0.3 --anisotropy 1 --sharp-edge 60 --repeat 3 --verbose

:: 4) 对照：用 HEAD 源码编一份原始二进制
build\build_pristine.bat
build\pristine\b\arbench.exe --input ...（同上参数）
```

`arbench` 的输出：一次运行的墙钟、四边形/非四边形/顶点数、峰值内存，`--verbose` 时打印 `phaseReport`（每个阶段耗时，与 GUI 报告一致）。
另有三个调试开关：`AR_TRACE=1`（阶段进度）、`AR_VERIFY_CHARGES=1`（电荷增量 vs 全量比对）、崩溃时自动打印 DbgHelp 符号化调用栈。

**注意**：`quadextractor.h` / `singularitysimplifier.h` 改了成员布局，改头文件后必须**全量重建**（`Remove-Item -Recurse build\bench`），否则不同 TU 的类布局不一致会导致访问越界——本次调试中踩过一次。

---

## 4. 还没动的热点（下一轮候选）

大模型上这个分布更极端——head_clean（400k 三角 / 20k quads）当前 20.2 s 里：`Cleanup triangles` 5.13 s、`Merging shared five edge faces` 3.69 s、`Collapsing three valence diagonals` 2.15 s，三者合计 54%；`Simplifying singularities` 0.95 s、`Holding singular lines` 1.05 s。

以 body_vox 当前 phaseReport 为例：清理三连 ~1.9 s、`Projecting vertices` 434 ms、cover 三件套（curl 300 / round 286 / eliminate 242）、`Holding singular lines` 312 ms。

- **三连修缮的每轮全量重建**：每接受一个操作就重建 `edgeFaces`/`vertexNeighbors` 并重扫全部面（`cleanupTriangles` 102 轮、`collapseThreeValenceDiagonals` 49 轮）。正确做法是**增量维护邻接**或**一轮内批量接受不相交的操作**，属于真正的结构化改造，需要配套一致性校验。
- **MILS 取整**：`threshold = max(d + 0.001, 1.0)` 让首轮固定全部整数变量，看似省时间；一旦按 `min(d) + 0.05` 修正，会变成真正的多轮取整（质量↑、耗时↑），需要重新平衡。
- **IsotropicRemesher 的 op-loop** 与 **Eigen 稀疏分解** 仍是单线程（thirdparty 无并发、无 OpenMP），要提速只能改算法或换并行分解。
- **GLB 输入**：当前只有 OBJ 解析，head_*.glb 素材需先转 OBJ 才能跑（本轮用的是 remesh 目录里已转好的 OBJ）。


---

## 5. 第二轮：清理 pass 的增量维护（本轮新增）

第一轮之后，head_clean 的 20 s 里有 11 s 花在三个清理 pass 上。根因不是容器，而是**每接受一个操作就把整张邻接表重建一遍、再把整份面表复制一遍**：`cleanupTriangles` 在 head_clean 上做了上千轮、每轮重扫 1.3 万面。

做法（三个 pass 同一套模式）：

1. **邻接索引建一次**：`edgeFaces`（边→面）、`vertexFaces`（点→面）、`vertexNeighbors`（共边邻居）、`faceHashCounts`（面哈希多重集）在 pass 入口构建；
2. **每次操作只补改动的面**：被改写的面 = 与新/旧顶点相邻的那些（从 `vertexFaces` 直接取），逐面做 `eraseFace` → 改写 → `insertFace`；
3. **面索引保持稳定**：被溶解的面留成空面而不是从 vector 里删掉，这样其它面的编号不变，撤销/校验都还能定位；pass 结束时再统一压实；
4. **重复面检测改成全局哈希多重集**：改写前把改动面从集合里摘掉，插候选、冲突即回滚——语义与原来"把所有未改动面塞进 set 再插候选"完全一致，但不再需要遍历全表；
5. **`holdSingularLines` 增加 三角形→连接 索引**：原来每个饥饿锥都要扫全网格的全部连接（O(锥 × 连接)），现在只扫该锥 10 环邻域内的面。

验证方式：每次都对比**操作计数 + 输出网格**（例如 `cleanup triangles:101 / merge shared five:46→47 / collapse diagonals:48→47`、body_vox 恒为 8267 quads），确认重构是等价的、不是"少做了事"。

效果（body_vox / head_clean）：

| pass | 改造前 | 改造后 | 倍数 |
|---|---|---|---|
| `Cleanup triangles` | 757 / 5134 ms | **42 / 268 ms** | 18× / 19× |
| `Merging shared five edge faces` | 599 / 3686 ms | **39 / 96 ms** | 15× / 38× |
| `Collapsing three valence diagonals` | 597 / 2145 ms | **36 / 79 ms** | 17× / 27× |
| `Holding singular lines` | 381 / 1526 ms | **145 / 350 ms** | 2.6× / 4.4× |

（head_in 的 `Holding singular lines` 累计值仍近 3 s，但那是 **8979 个岛并行**时的累加和——该模型 cores-busy 7.05，不是单岛墙钟。）

### 过程中踩到的两个坑

- `vertexFaces.resize(vertex + 1)` 在低编号顶点上会**截断**整个索引（resize 不是只增不减），导致邻居表几乎全空、pass 静默零操作。改成 `if (size <= vertex) resize(vertex + 1)`。
- 邻居语义必须是**共边邻居**（面里相邻的两个顶点），不能取"同一面的其它顶点"——后者会把四边形的对角点算进去，度数判据全错，输出从 8268 quads 漂到 8311。

### 本轮之后的热点（body_vox / head_clean）

`holdSingularLines`（145/350 ms）、`Simplifying singularities`（208/1083 ms）、cover 三件套（curl/round/eliminate ≈ 830/1800 ms）、`Projecting vertices`（434/768 ms）、`Solving frame field`（158/…）。
