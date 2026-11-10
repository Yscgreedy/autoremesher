# AutoRemesher 重拓扑算法清单

> 对象：E:/AI/autoremesher，核心实现为 src/AutoRemesher/*（L1/L2，零 Qt 依赖）+ thirdparty/isotropicremesher。
> 方法：逐文件通读源码，行号取自当前工作区实际内容。只列真正参与「三角网格 → 四边形网格」这条链的算法，UI/渲染/OBJ IO 不含在内。

---

## 0. 算法族谱（一句话）

它是 **Instant Meshes / MIQ 系** 的独立 C++ 重实现，四段式：

```
① 尺度 + 等距三角重网格          ② 交叉场（4-RoSy）        ③ 整数 quad cover            ④ 整数等值线抽取 + 拓扑修缮
  面积反推目标边长 h              曲率张量初始化            生成树刷场                   UV 整数等值线求交
  QEM 预降采样（meshopt）         4 周期坐标稀疏最小二乘     角点 90° 旋转 + 特征边约束    饥饿锥补线
  半边 op-loop（split/collapse/  锐边硬约束                 curl correction（CLS）       图清理 + 环搜索造面
  flip/relax/project）           奇异点配对消去（charge）   seam（内球 + 悬垂修剪）      补洞 + 非流形清理
  曲率自适应目标边长场                                      MILS（4-pass 消元 + 取整）    平滑 + 投影回原曲面
                                                                                          12 个专项修缮 pass
```

数学骨架：z_f = (cos4θ_f, sin4θ_f) 的纤维丛表示 + 整数平移变量 t ∈ 2Z 的稀疏二次规划 + 整数取整（rounding）。
最后把 u,v 的整数等值线当作网格边、把等值线交点当作顶点 —— 这正是 Bommes 等人的 *Mixed-Integer Quadrangulation* 路线；
而「连接图 + 环搜索 + 贪心补洞」这一段是 libQEx 式的抽取路线。

---

## 1. 阶段总表

| # | 阶段 | 算法 | 实现位置 | 输入 → 输出 | 量级 |
|---|---|---|---|---|---|
| 0 | 尺度 | 面积反推目标边长（正三角面积系数） | autoremesher.cpp:157-165 | 面积 A、目标三角数 N → h | O(F) |
| 1 | 分岛 | BFS 洪泛 + 64 位有向边键排序表 | meshseparator.cpp:94-139 | 三角表 → 连通岛集合 | O(E log E) |
| 2 | 压实 | 哈希 old→new 索引重映射 | autoremesher.cpp:609-636 | 岛三角 → 岛局部索引空间 | O(F) |
| 3 | 预降采样 | **QEM 边塌缩**（meshoptimizer，带顶点锁） | autoremesher.cpp:177-302 | 过密岛 → 目标 4 倍三角数 | O(F log F) |
| 4 | 等距重网格 | **半边 op-loop**：split / collapse / flip / tangential relax / project | isotropicremesher.cpp:248-380 | 任意三角网格 → 边长≈h 的三角网格 | O(F·iters) |
| 5 | 自适应场 | 平均法向变化率 + 75 分位参考 + 面积归一 | autoremesher.cpp:330-451 | 三角网格 → 每顶点目标边长 | O(V) |
| 6 | 参数化编排 | 法向 → 尺度场 → 拓扑 → 场 → 奇异点 → 各向异性 → cover | parameterizer.cpp:294-434 | 岛 → 每角 UV + 奇异点 | 见下 |
| 7 | 交叉场 | **4-RoSy 稀疏最小二乘**（曲率张量初始化 + 5 次不动点） | framefield.cpp:86-223 | 拓扑 → 每面单位向量 | O(F) 稀疏解 |
| 8 | 奇异点 | charge 计算 + 多源 BFS 配对 + 沿路径整数搬运 | singularitysimplifier.cpp:263-467 | 场 → 简化后的场 | O(k²·C) |
| 9 | 各向异性 | 边二面角 dyadic 张量场 + 12 次邻域平均 + 比值开四次方 | parameterizer.cpp:146-219 | 场 → su/sv 每面 | O(12V) |
| 10 | Cover | **MIQ**：生成树刷场 → 角点旋转 → curl 修正 → seam → 整数 cover | quadparameterizer.cpp:643-715 | 岛 + 场 → 整数 UV | 串行瓶颈 |
| 11 | 整数求解 | **MILS**：4-pass 消元 + kernel 投影 + 逐轮固定取整 | mixedintegerleastsquares.cpp:209-487 | 约束系统 → 整数解 | O(iter·nnz) |
| 12 | 抽取 | 整数等值线求交 → 连接图 → 环搜索造面 | quadextractor.cpp:48-289, 486-932 | UV → 多边形网格 | 最坏 O(V·d^6) |
| 13 | 修缮 | 补洞、非流形、平滑投影、12 个专项 pass | quadextractor.cpp:1295-1668, 1670-4553 | 多边形网格 → 干净四边形网格 | 多次全网格扫描 |

---

## 2. 阶段 A：尺度、分岛、预降采样

### A1 目标边长由面积反推
```
h = sqrt( (A / N) / (0.86602540378 × 0.5) )
```
0.866×0.5 就是正三角形「面积 / 边长²」系数的一半（= √3/4），所以 h 正是「要得到 N 个正三角形所需的边长」。
它不是体素大小，下游全部流程都把它当**目标边长**用（autoremesher.cpp:157）。

### A2 连通岛拆分（BFS）
meshseparator.cpp:94-139：把有向边 (from,to) 打包成单个 uint64（from<<32 | to），tbb::parallel_sort 排序后二分查邻面，
省掉 map<pair,size_t> 的每边一个红黑树节点；顶点索引超 2^32 时回退到树实现。洪泛本身串行，保持面序稳定。
动机：多部件模型必须按岛独立参数化，否则一个岛的 UV 尺度会污染另一个岛。

### A3 QEM 预降采样（meshoptimizer）
autoremesher.cpp:177-302：
1. 触发条件 triangles > 岛目标三角数 × 8（decimateTriggerRatio，:88），目标是 × 4（:89）—— 先粗降一次，避免等距重网格在千万面片上白跑。
2. 顶点先 meshopt_generateVertexRemap 焊接，再 meshopt_remapIndexBuffer / remapVertexBuffer。
3. 锐边保护：markSharpEdgeVertices（:91-135）把相邻面二面角 ≥ sharpEdgeDegrees 的边两端顶点打上 meshopt_SimplifyVertex_Priority 锁；
   边→面对用 ((min<<32)|max, face) 打包 + parallel_sort 配对，仍是排序而非哈希。
4. meshopt_simplifyWithAttributes(..., meshopt_SimplifyRegularize, &err)：二次误差度量（QEM）边塌缩，正则项抑制三角形质量退化。

---

## 3. 阶段 B：等距三角重网格

isotropicremesher.cpp:248-380 是 Botsch–Kobbelt 式局部 op 循环，每轮 5 步：

| 步 | 算法 | 判据 | 位置 |
|---|---|---|---|
| split | 长边中点分裂 | len > 4/3 × target（有顶点目标场时取两端均值） | isotropicremesher.cpp:382 / isotropichalfedgemesh.cpp:297 |
| collapse | 短边塌缩 | len < 4/5 × target，且两端非特征点 | :414 / isotropichalfedgemesh.cpp:659 |
| flip | 边翻转改度数 | 度偏离 Σ(v−6)²（边界用 4）必减；拒绝退化与翻面 | isotropichalfedgemesh.cpp:550 |
| relax | 切向松弛 | 移到一环重心，再投影回切平面 projectPointOnLine(v, c, c+n) | :501 |
| project | 投影回原曲面 | AABB 树最近三角形（或 PN 细分面） | isotropicremesher.cpp:489 |

关键细节：
- **度数判据**用「理想度数 6 / 边界 4」的平方偏差做全局比较（flipEdge:577-591），并额外做「顶点对已相连」与「新旧法向点积 ≤ 0」两道几何否决（:596-620）。
- **塌缩目标点**在有平滑法向时用 PN-Triangle 边界曲线求值（pnTriangleEdgeMidpoint，:51-59），否则取中点；塌缩前测一环边长上界。
- **特征保护**：featureEdges(radians) 按二面角标 featured，featureBoundaries() 标边界；特征点不塌缩、不投影（isotropichalfedgemesh.cpp:806-846）。
- **AABB 树**：最长轴、按中心值中位划分、叶子 ≤ 20（axisalignedboundingboxtree.cpp:71-141），用于最近点查询与盒范围查询。
- **平滑法向**：面积 × 角度的统一加权（computeSmoothVertexNormals，:71-148），再按 smoothNormalDegrees 二次剔除偏离过大的面。
  这是「平滑法向」参数真正起作用的地方：法向一旦平滑，projectVertices 就投到 PN 细分曲面上，成品更圆润。

### B5 曲率自适应目标边长场
autoremesher.cpp:330-451：
```
c_v   = Σ_u acos(n_v·n_u) / Σ_u |v−u|            // 每单位长度的平均法向变化率（对坏三角形鲁棒）
c_ref = 75 分位(c_v 的非零集)                     // nth_element，防止少量尖锐点把整体判成平坦
imp_v = 1 + min(adaptivity,2)·7·min(4, c_v/c_ref)²
h_v   = h · clamp( sqrt(mean(imp)/imp_v), 0.35, 3.0 )
```
面积加权归一这一步保证 ∫ area/h² 与均匀场一致 —— 即**不改变三角面预算**，只把面从平坦区搬到细节区。

---

## 4. 阶段 C：交叉场（4-RoSy）

framefield.cpp:86-223，逐面未知量 z_f = (cos4θ_f, sin4θ_f)（周期 4 表示，规避 θ 的 2π 绕回）：

1. **局部标架** (t_f, b_f, n_f)：取面第一条边投到切平面（createFacetTangentBasis，:48）。
2. **曲率张量初始化**：每条内部边把 length·dihedral 加权的方向 dyadic d dᵀ（6 个独立分量）累加到两端顶点
   （accumulateCurvatureTensor，:66；累加在 :112-123），面张量 = 三顶点张量之和，加 1e-6·tr 正则后用
   Eigen::SelfAdjointEigenSolver 求特征分解，取**特征值绝对值最大**的特征向量作主方向（:132-151），
   并把 |λ1|−|λ2| 归一化作为该面的置信度 certainty（:151-158）—— 即「曲率越大方向越可信」。
3. **全局平滑**：每条内部边写一对耦合方程，传输旋转 transport = −4·(θ_g−θ_f)，
   即 (R(transport)·z_f − z_g) = 0（:186-190）；再以 certainty² 为权重把 z_f 拉回初值（:191-199）。
   整体是 2F 未知量的稀疏最小二乘（ConstrainedLeastSquares），做 **5 次不动点迭代**（同一矩阵、只换右端项，:202-214）
   —— 相当于隐式平滑，比 Jacobi 收敛快。
4. **锐边硬约束**：二面角 > sharpEdgeDegrees 的边，把「沿边方向」直接固定为场分量（locked + addConstraint，:101-110、:174-178）。

---

## 5. 阶段 D：奇异点简化

singularitysimplifier.cpp：

- **charge（指标）**：绕顶点把每对相邻面的 90° 失配累加再 mod 4（vertexCharges:81-130；单角失配 quarterTurn:59-71，
  turns = round((θ_g−θ_f−connection)/(π/2))）。结果 1/2/3 分别是 5/6/7 价奇异点，0 是正则点。
- **配对候选**：以所有奇异点的面扇为种子做**多源 BFS**（simplify:417-453），两源相遇即产生候选 (a,b,hops)；
  过滤条件：hops ≤ maximumPairDistance（默认 6）、两者 charge 之和 ≡ 0 mod 4（能相消）；按距离升序、贪心不重入。
- **消去一对**（cancelPair:263-395）：
  1. 两个半径 hops+margin 的面球求交得到可动区域，锐边/边界面冻结；
  2. 区域内 BFS 找顶点路径（pathBetween），沿路径把 carried = (charge_a+1)%4 − 1 作为**整数 90° 跳变**写进边失配表（jump）；
  3. 自由面上做**边长加权 Gauss-Seidel 平均**（Σ w(θ_g−connection−jump·π/2)/Σw，:361-382，迭代上限 max(256, 40r²)）；
  4. 验收：a、b 的 charge 归零且受影响奇异点数不增，否则整体回滚（:386-391）。
- 本质是「不移动顶点、只改场角度的指标搬运」，与 Instant Meshes 的 singularity 处理同源。

---

## 6. 阶段 E：各向异性张量场

parameterizer.cpp:146-219：
1. 每条内部边构造 weight·d dᵀ，weight = length × |dihedral|，累加到两端顶点（6 分量）。
2. **12 次 Jacobi 式邻域平均** T ← 0.5·T + 0.5·avg(T_neighbors)（:179-194）。
3. 面张量 = 三顶点之和，沿场方向 b 与其正交方向 b⊥ 求二次型 T_uu、T_vv（quadraticForm，:140）。
4. 各向异性比：ρ = ((T_vv+ε)/(T_uu+ε))^(1/4)，再取 ρ^anisotropy，钳制到 [1/√maxAspectRatio, √maxAspectRatio]
   （maxAspectRatio = 2.3，:112）；su = ρ，sv = 1/ρ。开四次方是因为张量是二次型、缩放是长度量。
5. 另有一段 computeConformalScaling（:42-138）：cotan Laplacian + 高斯曲率亏损约束 + 目标尺度拟合的 CLS
   （保角/保面积尺度场），**调用点已被注释**（:388-389），属死代码 —— 对应 Instant Meshes 的 conformal 分支。

---

## 7. 阶段 F：Quad Cover（MIQ）

quadparameterizer.cpp:643-715 的七段：

### F1 生成树刷场
brushFieldAlongSpanningTree:190-218：从面 0 做 BFS，沿每条树边用 edgeQuarterTurn（:42-75，枚举 4 个 90° 旋转取角度误差最小者）
把父面场「刷」到子面，保证 UV 连续可积的初始猜测。

### F2 角点旋转 index
computeCornerRotations:220-231：每个角点记录跨该边的 90° 旋转 r_c ∈ {0,1,2,3}，它是后续所有 seam / 整数约束的载体。

### F3 特征边 U/V 约束
edgeConstraint:80-94：二面角 ≥ 硬边的边，若与场方向夹角 < 10° 则整条边 UV 必须沿同一坐标（ConstraintU/V）；
下游 setVariablePeriod 把该坐标周期降为 1（硬整数线）—— 「特征线必然落到四边形边上」的机制。

### F4 curl correction（3F 未知量 CLS）
applyCurlCorrection:267-373：未知量是每面的 (角度修正, log su, log sv)。
对每条边写「两侧切向 UV 增量在旋转后应相等」的线性方程（request 对 su/sv 做一阶展开，:290-330），
加 1e-4 正则，锚定面写硬约束；解完把角度钳在 ±0.3 rad、缩放钳在 ±log1.5，并按面积比重新归一 su·sv（:355-372）。

### F5 seam 计算
computeSeam:375-427：
1. 只在 rotation == 0 的边上做 BFS，得到「可展内球」insideBall；
2. 球外与边界边全标为 seam；
3. **悬垂割缝修剪**：while(changed) 反复检查「seam 边且 rotation==0 且 seam 端点度数为 1」的边，把它收回内球（:406-425）
   —— 去掉没有拓扑必要的割缝，减少 UV 自由度。

### F6 整数 cover 系统
solveQuadCover:447-608，变量 2C（每角 UV）+ 2C（每角整数平移 t，周期 2）：

| 约束 / 能量 | 形式 | 作用 |
|---|---|---|
| 每边 × 每坐标 | u_n − u_c − (b·e)/su = 0，权重 su·sv | 让 UV 增量 = 边在场上投影 / 目标边长（:471-487） |
| 固定 u_0, v_0 | 硬约束 | 消除全局平移（:488-489） |
| seam 边 | t_c = R(r)·t_oc（4 种旋转分支） | 跨割缝的整数平移关系（:505-525） |
| 非 seam 边 | t_c = 0 | 无割缝处平移一致（:500-503） |
| 顶点 wheel | Σ R(r_i)·t_i = 0 | 绕一个顶点的 holonomy 必须闭合（:526-572） |
| 特征边 | u_c − u_n = 0 或 v_c − v_n = 0 | 硬坐标（:573-587） |

### F7 MILS 整数求解
mixedintegerleastsquares.cpp，4 个 pass（finalizeConstraints:209-241）：
- **pass0**：单变量约束 → 该变量直接从 m0 删除（周期吸收）；
- **pass1**：两变量 ±1 系数约束 → 并查集式合并（m_m1，带符号缩放），周期取大者；
- **pass2/3**：把约束投到 m2，按 period×|a| 选最小元做主元做**列消去**（applyRemoveColumn:341-357），再重投一轮；
- buildKernel:243 重建 kernel 基，能量全部映射到 kernel 空间（:228-241）；
- solveIteration:449-487：每次把「已足够接近整数」的 kernel 变量固定为 period·round(v/period) 硬约束，其余用 CLS 重解，直到全固定或到 100 轮。
- **缺陷**：:456-469 的 threshold = max(d+0.001, 1.0) 而 d ∈ [0,0.5]，阈值恒为 1.0 → 首轮固定全部整数变量，
  「逐轮取整」退化成一次四舍五入。

### F8 UV 吸附与奇异点判定
buildResultUv:610-639：|x−round(x)| < 0.01 的坐标吸附成整数（让等值线恰好穿过角点，避免退化三角形）；
顶点 wheel 旋转和 ≠ 0 且非边界 ⇒ 奇异点。

---

## 8. 阶段 G：整数等值线抽取 + 拓扑修缮

### G1 等值线求交与去重
extractConnections:759-932：
- 对每个三角，u=const 与 v=const 的**整数等值线**与三条边求交（线性插值同时得 3D 位置与 2D UV），
  处理「整条边落在等值线上」（edgeCollapsed）与 ratio==0/1 的端点退化；
- 交点用 PositionKey（固定 1e-5 量化成整数三元组做 std::map 键，positionkey.cpp:26-42）去重
  —— 相邻三角形共享的等值线端点因此粘成一个顶点；
- **第二遍切分**：把 u 线被 v 线截断成多段（:876-929），保证每个连接都是「网格边」级别的短段。

### G2 饥饿锥补线
holdSingularLines:934-1257：奇异点如果只有 1 个分支（valence 不足，锥「饿死」），
从它出发在 10 环邻域里沿场方向以 30° 锥搜索最近边交点（maxWalkSteps = 32），
找到后用 splitAt 把那条已有连接**劈成两段**并插入新点，从而给奇异点补出足够分支（:974-1015、:1211-1250）。

### G3 连接图清理

| 算法 | 位置 | 判据 |
|---|---|---|
| 简化图（合并 2 度点） | :302-331 | 度为 2 且两邻居不同 → 连成一条 |
| 塌短边 | :427-455 | len < 0.01 × 平均边长 |
| 塌三角形 | :365-426 | 3 点互为邻居 → 塌最短边 |
| 去掉单端点 | :333-364 | 度为 1 的悬垂链整条回溯删除 |

### G4 环搜索造面（全项目最重的循环）
extractMesh:486-757：for round = 0..4，每轮只收**固定环长**的面：round0→4、round1→5、round2→6、round3→7、round4→3（triangleRound = 4）。
- 4 层到 7 层嵌套 DFS 沿邻接表枚举「回到起点」的简单环，深度即环长；
- 三道否决：角点三元组复用（isFaceCornerExist）、半边双向占用（isFaceHalfEdgeExist）；
- **朝向判定** calculateSide（:516-528）：环面法向（扇形三角法向之和）与源三角形法向点积 > 0.259（≈75°）取正向，
  < −0.259 取反向，否则丢弃 —— 防翻面；
- 先出四边形，再补五/六/七边形，最后才允许三角形：**优先保证四边形占比**。

### G5 补洞与非流形清理
- searchBoundaries:1398：半边集合里找有向环，next 不唯一即放弃；
- fixHoleWithQuads:1311：贪心补洞 —— 枚举洞上四元组 (i,j,k,h)，评分用边方向点积（越接近平面越好），
  再做「与已有四边形冲突」和「其它点落在四边形平面内」两项否决，逐次吃掉四个点；环长 > 65 直接放弃（:1300-1303）；
- removeNonManifoldFaces:1451：边→面计数 > 2 或顶点开边界计数异常的面整片删除；removeIsolatedFaces:1439 再做连通分量过滤。

### G6 平滑 + 投影
smoothAndProject:1541-1668：5 次 v ← v + 0.5(重心 − v)（Jacobi，边界/单用边顶点锁死），
每次之后用 AABB 树在「平均边长 → 8 倍」的倍增半径里找候选三角形，
closestPointOnTriangle（Ericson 重心区域判定，:1480-1540）取最近点投影回原曲面。
TBB 并行：读旧写新、投影只读树。

### G7 12 个专项修缮 pass
统一模式：先建 edge→faces / vertex→faces 邻接，用**评分函数**挑最优操作，操作后做局部 smoothAroundVertices
（:3882，从种子长 N 环面片、只动内部顶点）。

| pass | 处理对象 | 评分 / 判据 | 位置 |
|---|---|---|---|
| splitSevenEdgeFaces | 7 边面 | 对角线 flowScore（方向流一致性）+ cornerScore（角点平直度） | :4211 |
| splitSixEdgeFaces | 6 边面 | 同上，选一条可行对角线切两半 | :1670 |
| cleanupTriangles | 三角形 | 沿「四边形带」一次性塌掉整条梯子，避免生成扇形薄片 | :3941 |
| mergeSharedFiveEdgeFaces | 相邻五边形对 | 合并成 6 边面再切分 | :4318 |
| switchHighValenceEdges | 高价位顶点 | 六边形换对角线：不动点只改连接（valenceScore + cornerScore） | :3667 |
| convertTriangleAndFiveEdgeFans | 三角 + 五价扇 | 成对转换，边界必须简单、防「埋边」 | :1777 |
| collapseThreeValenceDiagonals | 3 价顶点 | 对角线塌陷，校验唯一性与边计数 | :2036 |
| mergeDoubleSharedEdgeQuads | 共享两条边的四边形对 | 合并成一个面再切 | :2302 |
| mergeThreeAndFiveValenceTriangles | 3 价 + 5 价相邻 | 成对消去 | :2482 |
| collapseThreeValenceCorners | 3 价角点 | 角点塌陷改度数 | :2849 |
| splitHighValenceTriangleFans | 高价三角扇 | 插入顶点拆分 | :3106 |
| collapseThreeValenceEdgePairs | 3 价边对 | 成对塌缩 | :3371 |

顺序刻意安排：先把三角形变五边形（cleanupTriangles 注释写明「五边形是三角形的归宿」），再用合并把五边形变四边形；
switchHighValenceEdges 放最后，只重连四边形对。

---

## 9. 数值基础设施

| 组件 | 算法 | 说明 | 位置 |
|---|---|---|---|
| ConstrainedLeastSquares | **带等式约束的稀疏最小二乘**：仿射替换消元 → 正规方程 AᵀA + ridge → SimplicialLDLT；不可消元时退化为 Lagrange 乘子 + SparseQR 取独立约束 | 相对 ridge = 1e-10 × mean(diag)，结果与模型单位无关 | constrainedleastsquares.cpp:172-341, 344+ |
| MixedIntegerLeastSquares | 4-pass 消元 + kernel 投影 + 逐轮固定取整 | 见 F7 | mixedintegerleastsquares.cpp |
| SurfaceMesh | **corner（半边）拓扑**：oppositeCorner 配对 + 顶点角环链；normalAngle 给带符号二面角 | 非流形/方向冲突处「降级为边界」 | surfacemesh.cpp:32-143 |
| AxisAlignedBoudingBoxTree | AABB **BVH**，最长轴按中心划分，叶子 20 | 最近点与盒查询 | axisalignedboundingboxtree.cpp |
| PositionKey | 1e-5 定点量化 + 字典序 | 等值线交点去重的哈希替代 | positionkey.cpp |
| 并行 | TBB parallel_for（法向/曲率/场初始化/平滑/投影）+ 岛级并行 + parallel_sort 建邻接 | 岛内求解器仍串行（Eigen 单线程） | 全局 |

---

## 10. 参数 → 算法旋钮

| 参数 | 影响的算法 | 具体作用 |
|---|---|---|
| targetTriangleCount | A1 → 全体 | 决定 h，进而决定四边形密度 |
| edgeScaling | F6 权重 su, sv | scale = scaling × 平均边长，整体缩放 UV（等于改目标四边形边长） |
| adaptivity | B5 曲率场（+ 死代码 E5） | 曲率处加密，面积预算守恒 |
| anisotropy | E 各向异性 | ρ^anisotropy，把各向同性四边形拉成长条 |
| sharpEdgeDegrees | A3 锁点 / B3 特征边 / C4 场硬约束 / F3 U-V 硬约束 | 一条阈值贯穿四层，保证特征线对齐 |
| smoothNormalDegrees | B4 平滑法向 + PN 细分投影 | 投到 PN 曲面，成品更圆 |
| maximumSingularityPairDistance | D 配对 | 只允许近距离奇异点相消，防止场被拉坏 |

---

## 11. 已知实现缺陷（会影响上面的算法结论）

1. **MILS 取整阈值退化**（mixedintegerleastsquares.cpp:456-469）：maximumIterations=100 形同虚设，整数 cover 实际是一次四舍五入 —— 复杂拓扑更易残留非整数 UV（靠 G1 的 0.01 吸附兜底）。
2. **removeIsolatedFaces 只保留最大岛**（quadextractor.cpp:1439-1449）：多零件模型的小零件在抽取后可能整块消失。
3. **PositionKey 固定绝对 1e-5**（positionkey.cpp:26）：小尺度模型（<1e-3 单位）交点被压成少数整数点、连接图塌陷；大坐标在 32 位 long 上溢出。
4. **smoothCrossField 在本流水线永不执行**（quadparameterizer.cpp:680-681 只在 guidance==nullptr 时跑，而 parameterizer.cpp:367 总传非空场），实际起作用的是 F1 的生成树刷场。
5. **computeConformalScaling 是死代码**（parameterizer.cpp:42-138，调用点 :388 被注释），尺度场只用 B5 的启发式曲率场。

---

## 12. 一句话总结

重拓扑质量由三件事决定：
**B 段把三角网格变成均匀铺底**（半边 op-loop + QEM 预降采样 + 曲率自适应边长）；
**C/D/F 段把「要往哪切」变成一个整数规划**（4-RoSy 场 + 奇异点指标搬运 + MILS 整数 cover）；
**G 段把整数解翻译成干净四边形**（等值线求交 → 环搜索 → 12 个拓扑修缮 pass → 投影回曲面）。

---

## 13. 并行与硬件加速现状（CPU-only）

**结论：全部算法都在 CPU 上跑，没有 GPU 计算。** OpenGL 3.3 core 只负责把结果画到屏幕上（9 个 GLSL 全是 vertex/fragment/geometry，无 compute shader、无 SSBO）；无 CUDA / OpenCL / SYCL / Vulkan compute / OpenMP。

### 13.1 并行层级

| 层 | 粒度 | 机制 | 覆盖范围 | 位置 |
|---|---|---|---|---|
| L1 岛级 | 一个连通岛 = 一个 task | tbb::parallel_for | 岛上下文构建、等距重网格、参数化+抽取（**主加速来源**） | autoremesher.cpp:609 / 738 / 876 |
| L2 阶段内 | 顶点 / 面 / 角点 / 边 | tbb::parallel_for | 面法向、顶点法向、曲率、尺度场、各向异性初始化、场的局部标架与特征分解、40 轮场平滑、角点约束、5 轮 Laplacian 平滑与投影、锐边标记与边排序 | parameterizer.cpp:233,271,323,337；framefield.cpp:93,113,125,161,216；quadparameterizer.cpp:115,134,153,180,239；quadextractor.cpp:1644,1656；autoremesher.cpp:99,109,123,345,370,383,421 |
| L3 串行内核 | — | 无 | **等距重网格整个 op-loop**（thirdparty 无任何并发）、Eigen 稀疏求解（SimplicialLDLT / SparseQR / SparseLU，无 OpenMP、无 setNbThreads）、quad cover 约束装配、MILS 全部、奇异点 BFS/配对/平滑、等值线求交与 std::map 去重、7 层嵌套环搜索、补洞、12 个修缮 pass | 见各节 |
| L4 编译器/SIMD | 循环 / 128-bit 向量 | -O3 -flto -funroll-loops -march=x86-64-v2（Linux）；/O2 /GL /Qpar /fp:fast（MSVC 自动并行化）；macOS 额外 -DNL_USE_BLAS + Accelerate 框架，CLS 优先用 Eigen::AccelerateLLT | Eigen 默认 SSE2/AVX 向量化；TBB scalable allocator | autoremesher.pro:75-105；constrainedleastsquares.cpp:66-110 |

### 13.2 线程模型

GUI 线程 + 3 个 QThread（render / preview / quad，moveToThread 后只跑一个任务）+ TBB 线程池，三层嵌套共存。
但 **L1 的并行度 = 岛的个数**：常见模型只有一个连通岛 → 外层并行完全退化为单线程，只剩 L2 的数据并行，
而 L2 覆盖的都是 O(V)/O(F) 的轻量计算，真正的墙（solveQuadCover、holdSingularLines、环搜索、isotropic op-loop、Eigen 分解）全在 L3。

可用 phaseReport 里的 `Cores kept busy across the parallel phase = accumulated / wall`（autoremesher.cpp:1030-1040）实测这个数字：
单岛模型通常落在 1.0–2.0，多零件模型才能接近核数。

### 13.3 提速改造进度
已按 §13.2 的结论开工，只改 CPU 热点、不动并行度，两轮共 2.29× / 2.79× / 4.48×：
body_vox 15k（单岛）8.28 → 3.61 s，head_in（8979 岛）12.74 → 4.57 s，head_clean（400k 三角 / 20k quads，单岛）41.18 → 9.20 s，非四边形占比维持 3% 左右。
第一轮去掉重复计算与分配（奇异点电荷增量、索引复用、哈希容器），第二轮把三个清理 pass 与饥饿锥扫描改成**增量维护**（邻接建一次、只补改动面、面索引稳定、全局哈希去重）。
逐项改动、失败与修正记录、复现命令见 [OPTIMIZATION_LOG.md](OPTIMIZATION_LOG.md)。

### 13.4 结论

- **GPU 加速**：没有。GPU 仅在预览/成品渲染阶段参与；headless 模式甚至可用 `QT_QPA_PLATFORM=offscreen QT_OPENGL=software` 纯软件跑完。
- **并行**：有，但是"岛级任务并行 + 阶段内数据并行"的 CPU 方案，求解器与几何拓扑操作全部串行。
- 若要进一步提速，优先级是：并行化 L3 的四个串行核（cover 求解按割缝连通块分块、cancelPair 增量维护 charge、computeSeam 改队列式一轮、holdSingularLines 按锥分块），而不是上 GPU——因为 L3 是稀疏不定拓扑上的图算法，GPU 收益远低于先把岛数和并行度问题解决。
