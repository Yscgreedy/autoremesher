# AutoRemesher 提速改造记录

> 基线：本机 MSVC 19.51 (VS18) / Ninja / Release（`/O2 /GL /Qpar /fp:fast /LTCG`），与原 `autoremesher.pro` 的 MSVC 选项一致。
> 测试素材：`E:/ComfyUI-aki-v3/zcode_work/char3d/remesh/*.obj`（body_vox / head_in / head_clean）。
> 对照方式：`git archive HEAD` 出的**原始源码**用同一份 bench 工程、同一编译器编出来的二进制，与改造后二进制逐模型对比。

---

## 1. 结果总览

| 模型 | 参数 | 原始 (s) | 改造后 (s) | 加速 |
|---|---|---|---|---|
| body_vox.obj（78k 三角，1 岛） | 15k quads, adaptivity 0.3 | 8.276 | **3.54** | **2.34×** |
| head_in.obj（400k 三角，**8979 岛**） | 10k quads, adaptivity 0.5 | 12.739 | **4.54** | **2.81×** |
| head_clean.obj（400k 三角，1 岛） | 20k quads, adaptivity 0.3 | 41.182 | **8.75** | **4.71×** |

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


---

## 6. 第三轮：奇异点简化与包围盒查询

第一、二轮之后 head_clean（单岛，9.2 s）的大头变成：`Simplifying singularities` 876 ms、`Rounding cover` 832 ms、`Correcting field curl` 818 ms、`Projecting vertices` 805 ms、`Eliminating cover constraints` 505 ms。这轮先打前两处结构性浪费。

### 6.1 `cancelPair` 的每候选全网格数组

`src/AutoRemesher/singularitysimplifier.{h,cpp}`

每个候选点对原来要：分配并填满两个 `vector<size_t>(faceCount)` 的距离数组（400k 面 = 3.2 MB × 2）、再全表扫描求交、再分配 `inRegion`(F)、`affected`(V)、`jump`(3F)、`saved`，以及 `pathBetween` 的 `previous`(V)。候选数量在千级，等于把整张网格来回扫了几千遍。

改法：**全部换成成员级复用 + 世代戳（generation stamp）**
- `m_ballDistance` / `m_ballStampFirst` / `m_ballStampSecond`：BFS 只写访问到的面，成员判定 `stamp == generation` 是 O(1)，清空也是 O(1)；
- 两球求交改为**遍历较小的那个球**，而不是遍历全部面；
- `m_regionStamp` / `m_affectedStamp` / `m_jumpStamp` / `m_pathStamp` + `m_pathPrevious` 同理；
- `vertexCharges()` 每次返回 `vector<int>`（V = 20 万时一次 800 KB 拷贝，每个候选调用 4 次）改为 `chargesCached()` 返回 `const&`；
- `region` 在求交后**排序**，让 Gauss-Seidel 的遍历顺序与原来"升序扫描全表"一致——否则结果会随两球哪个更小而漂移（这点一开始就漂了 12 个四边形）。

**效果**：body_vox 208 → **94 ms**，head_clean 876 → **491 ms**，输出与改造前逐位一致（13071/417/13583）。

### 6.2 AABB 树加直接盒查询

`thirdparty/isotropicremesher/axisalignedboundingboxtree.h`

原来"查一个盒子"的做法是**临时造一棵单盒树**再调 `test()`——每个顶点、每次投影都分配节点。新增 `query(box, result)`，直接下降已有树、逐层比盒，零分配。用在两处：
- `IsotropicRemesher::projectVertices`（每个顶点一次 → 每个顶点省一棵树的构造）；
- `QuadExtractor::smoothAndProject`（每个投影点一次）。

**效果**：`Projecting vertices` 805 → **774 ms**（这块真正的成本是 `closestPointOnTriangle` 的候选数，不是查询本身，下一步要改成最近三角形的最优优先下降）。

### 6.3 一个自己造的并发 bug（记录）

把 `smoothAndProject` 的候选缓冲提到 lambda 外，结果被 `tbb::parallel_for` 多线程共享 → 立刻访问越界。改成 `static thread_local`。教训：这个函数体虽然"读旧写新"，但**临时缓冲不是线程安全的**。

### 本轮之后

body_vox **3.54 s**（2.34×）、head_in **4.54 s**（2.81×）、head_clean **8.75 s**（4.71×）。剩余大头是盖层求解（round/curl/eliminate 合计 ≈ 2.1 s @ head_clean）与 `projectVertices` 的候选三角形数量，二者都需要动算法而不是减开销。


---

## 7. 高面数目标下的表现

同样的三个模型，把目标四边形数拉高（`targetTriangleCount = 2 × quads`，其余参数不变），当前构建与 HEAD 原始源码同机同编译器对照：

| 模型 | 目标 quads | 产出 quads | 产出率 | 当前耗时 | 原始耗时 | 加速 | 峰值内存 |
|---|---|---|---|---|---|---|---|
| body_vox（78k 三角） | 15k | 8267 | 55% | 3.54 s | 8.28 s | 2.34× | 214 MB |
| body_vox | 50k | 19308 | 39% | 9.63 s | 30.19 s | **3.13×** | 528 MB |
| body_vox | 200k | 67228 | 34% | 52.19 s | — | — | 1913 MB |
| head_clean（400k 三角） | 20k | 13071 | 65% | 8.75 s | 41.18 s | 4.71× | 458 MB |
| head_clean | 50k | 33669 | 67% | 25.87 s | 210.98 s | **8.15×** | 949 MB |
| head_clean | 100k | 50453 | 50% | 42.28 s | — | — | 1425 MB |

两个结论：

1. **目标越高，改造的收益越大**（4.7× → 8.15×）。原因很直接：老代码每个被接受的拓扑操作都要重建整张邻接表、重扫全部面，操作数本身随密度增长，于是它的成本是"操作数 × 面数"；增量维护把它压回"操作数 × 局部"。原实现在 head_clean 50k 上要为同一个模型花 3.5 分钟。
2. **产出率随目标升高而下降**（55% → 39% → 34%；65% → 67% → 50%）。要拿到 N 个四边形，得按 1.5~3 倍 N 去要。这不是本次改动的退化（原始实现同样如此：head_clean 50k 目标原始产出 33685，当前 33669），而是抽取阶段在高密度下丢掉更多面（清理 pass 合并/删除的面占比上升）。若流水线需要"精确 N 个四边形"，应在外面做一次二分或按产出率预放大。

### 高密度下的瓶颈换了位置

head_clean 100k（42.3 s）阶段分布：

| 阶段 | 耗时 | 占比 |
|---|---|---|
| `Correcting field curl` | 9025 ms | 21% |
| `Rounding cover to integers` | 6129 ms | 15% |
| `Projecting vertices` | 4138 ms | 10% |
| `Eliminating cover constraints` | 1821 ms | 4% |
| `Solving frame field` | 1833 ms | 4% |
| `Holding singular lines` | 1322 ms | 3% |
| 抽取其余（edges/mesh/清理） | ≈ 6.3 s | 15% |
| 等距重网格其余 | ≈ 4.2 s | 10% |

即 **盖层求解（curl + rounding + eliminate ≈ 17 s）已成为第一大头**，与低目标时"清理 pass 占一半"完全不同——因为它是 3F 未知量的稀疏分解，复杂度对手数是超线性的（20k 目标下这三项只要 1.8 s）。

### 负结果：用 CG 替换直接分解是错的

试过给 `ConstrainedLeastSquares` 加预条件共轭梯度（对角预条件，容差 1e-9），在行数 ≥ 20 万时替换 `SimplicialLDLT`：

| head_clean 100k | 直接分解 | CG |
|---|---|---|
| `Correcting field curl` | 9025 ms | 32976 ms |
| `Rounding cover` | 6129 ms | 72709 ms |
| 总计 | 42.3 s | 137.7 s |

慢 3.3 倍。这些系统的正则只有 `1e-10 × 平均对角`，条件数极差，CG 迭代到上限也没收敛；直接分解的填充反而对这类二维网格拉普拉斯很友好。已回退，默认继续用 `SimplicialLDLT`/AccelerateLLT。

**下一步要提速只能是算法层面**：curl correction 的未知量是每面 (角度, log su, log sv)，但它只服务于"让切向 UV 增量可积"；可以把三变量解耦（角度单独一解、各向异性用局部闭式近似），或在多岛情形把求解按岛切块并行（当前岛内串行）。取整（MILS）同理：修好阈值退化的同时要接受它真的会多跑几轮，需要先做质量/耗时权衡实验。


---

## 8. 能不能"按割缝切块并行"？——实测与结论

### 8.1 先说事实：割缝并没有把网格切开

`computeSeam` 的语义是"这些边上的整数平移不再强制为 0，而是 `t_c = R(r)·t_oc`"，它**放松约束**，并不删除边。按"只走非缝边"求连通块（`AR_DIAG_CHARTS=1` 打印）：

| 模型 | 面数 | 缝边数 | 连通块数 | 最大块占比 |
|---|---|---|---|---|
| body_vox 15k | 37368 | 6586 | 7 | **100.0%** |
| head_clean 20k | 68394 | 18276 | 285 | **99.6%** |
| head_clean 100k | 263260 | 36258 | 4 | **100.0%** |

缝边很多，但**不成闭合割线**，所以最大块几乎包含全部面。按割缝并行的第一手结论：**切不出块，也就谈不上并行**。

### 8.2 退一步：假设真的切得开，结果会不会变？会

1. **整数取整是顺序敏感的启发式**。MILS 的 4-pass 消元（m0 删单变量、m1 并查集合并、m2 列消去）按"发现顺序"构造 kernel 基，之后逐轮把接近整数的变量固定。块的划分或合并顺序一变，kernel 基就变，取整路径跟着变——这是组合问题，不是线性方程，没有"解唯一"的保证。
2. **浮点装配顺序**。线性最小二乘本身有唯一解（SPD + ridge），但装配、求和、遍历顺序改变最后几位；这条流水线对最后几位敏感——本轮就实测到：只把容器从有序换成哈希（迭代顺序变化，操作计数不变），四边形数漂了 ±10。
3. **近似分区（Schwarz 类）不是"最后几位"**。切向 UV 增量必须沿整条缝可积，跨缝耦合丢掉就等于换了一个优化问题，几何会实质漂移。

### 8.3 什么并行是"免费"的：逐元素独立算子

给 `arbench` 加了 `--threads N`（TBB `task_scheduler_init`）和几何哈希（顶点坐标 + 面索引的 FNV-1a，打印 `hash`），实测：

| 模型 | 1 线程 | 4 线程 | 32 线程 |
|---|---|---|---|
| head_clean 20k | 13071 quads, hash `5eb6f40c1448e6c2` | 同 | 同 |
| body_vox 15k | 8267 quads, hash `aba46e497f590607` | — | 同 |

**三种线程数输出逐位一致**。说明现有的并行区（法向、曲率场、各向异性初始化、Jacobi 平滑、投影、锐边标记、面尺度场）是顺序无关的，线程数不影响结果。

### 8.4 若要并行盖层求解，只有一条保结果的路

**精确两级分解**：块内 LDLT 分解 + 界面 Schur 补**精确**求解（不是迭代近似），且块的编号、界面的合并顺序固定 → 数学等价，浮点上固定求和顺序即可逐位一致。代价是实现复杂度与界面规模决定收益；本项目的缝结构（8.1）先得改成真正的闭合割线，才有块可分。

或者换思路：**降低问题规模**而不是并行——curl correction 的每面 3 个未知量里只有角度真正影响可积性，解耦后规模降到 1/3、条件数大幅改善；但那属于换算法（质量会有可感知变化），要单独做质量/耗时权衡实验，不能用"结果不变"来承诺。

### 8.5 回归手段

任何声称"保结果"的改动都可以这样验：

```bat
build\bench\arbench.exe --input model.obj --target-quads 20000 --threads 1
build\bench\arbench.exe --input model.obj --target-quads 20000 --threads 32
:: 两次的 quads / non-quads / vertices / hash 必须完全一致
```
