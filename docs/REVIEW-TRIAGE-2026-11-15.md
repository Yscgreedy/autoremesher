# 用户评价核对（2026-11-15）

对象：master（`62441b0e`，AutoRemesher 1.2.0 CLI，build/cli/dist/autoremesher.exe 2026-11-13）
方法：代码审计（file:line）+ 实跑回归（sphere / 开口球 / multi_part），加上三路只读审计。
实跑产物：`build/audit-tmp/`（A.obj / s20k*.obj / MP*.obj / *.json 指标）。

结论一句话：**"顺的地方" 4 条里 3 条成立、1 条夸错了对象；10 条不满里 4 条是旧版本/误读，6 条是真缺口**，
真缺口里有 5 项是"数据已经在内存/stderr 里，只差一次结构化导出"。

---

## 〇、修复进展（2026-11-15，P0 已落地）

| 项 | 状态 | 落地内容 |
|---|---|---|
| 输入健康度 | 已修 | 核心 `analyzeTopology()` 统计边界边/边界环/非流形边/连通分量/退化面；CLI 在开跑前出 `warning`（JSON）+ stderr 一行，`result.topology.input/output` 双向对比 |
| 洞与 patch 静默 | 已修 | `QuadExtractor::ExtractionStats` 计 filled/ignored/facesAdded 与 kept/dropped；>65 边的洞出 `holes_left_open` 警告；实测 multi_part 输出 452 洞已补 / 3 洞留空 / 25 个 patch 被丢 |
| 机器可读输出 | 已补 | `--json` 新增 `input` 事件与 `warning` 事件；`result` 增 `requestedQuads/quadsError/islands/coresBusy/topology/holes/patches/phases`，保留 v1 扁平键 |
| --quiet/--verbose | 已修 | 核心不再直写 stderr，改走 `AutoRemesher::Log` 汇；`--quiet` 实测 0 行；`--verbose` 不再重复打印；`--log-level quiet|normal|debug` |
| target-quads 口径 | 已修（默认翻转） | CLI 与 GUI 默认走 exact（实测 12k→12531、20k→20380、multi 6k→6064）；旧路径保留为 `--legacy-target-quads`（0.7N..1.05N）；`|err|>10%` 出 `target_quads_off` 警告；回归锚点已按新默认重测 |
| --model-type 死开关 | 已修 | 改成真正的 preset 表（organic/hard-surface），显式传的旋钮仍优先；`--preset` 为新名，`--model-type` 为别名；GUI 下拉框现在会把四个滑块拨到同一张表 |
| 回归护栏 | 已加 | `tools/smoke_cli.py`（JSON 契约 + quiet 静默 + 目标数 ≤15%），已接入 gates.yml；`--legacy-target-quads` 可复现旧 MD5（F7DC332D…） |

P1 进展：**UV/vn 导出**、**pole 分布 + edge-loop 走态度量**（tools/metrics，poles/簇/最近距离、环转向角与横向二面角）、**对称**（`--symmetry x|y|z` + `--symmetry-plane`：平面裁切 + 中缝焊接 + 正半镜像，构造性对称；实测 0 unmatched、0 开边、0 非流形；平面不可在原点时用偏移参数）均已落地。仍未做：单岛并行（P2）、偏差进核心/GUI（P2）、UV v2 逐角映射（消 seam 断口）。

## 一、"顺的地方"核对

| 评价 | 判定 | 证据 |
|---|---|---|
| 快：26cm 鞋 6–9 s | 成立 | sphere 12k 2.38 s、20k 4.55 s；阶段计时显示 84% 耗在 Parameterize（2.00/2.38 s），与"等"的感受一致 |
| 四边面率高 | 成立 | sphere 12k：9520 面全 quad，n-gon 0；multi_part 6000：6064 quad + 602 n-gon（90.9%） |
| hard-surface 模式保形不错 | **不成立（开关是死的）** | `--model-type` 只写 `m_modelType`（autoremesher.h:76-79、291），**全仓无任何读取点**；实测 organic 与 hard-surface 输出 md5 完全相同（F7DC332D…）。upstream/master 与 Qt 时代同样只存不读（mainwindow 的 combo 早已注释掉）。保形是默认参数的效果，不是这个开关 |
| CLI 完整可脚本化 | 成立 | 17 个开关 + `--json` 五类事件 + `--threads` + 退出码 0/1/2/3（main.cpp:125-162） |

---

## 二、前置环节 7 条

| # | 评价 | 判定 | 事实 / 证据 | 结论动作 |
|---|---|---|---|---|
| 1 | 不诊断输入健康度 | **成立（且比说的更糟）** | 输入校验只有"空顶点/空面/target=0"三项（autoremesher.cpp:552-564）；边界边、非流形、碎壳一律不查。实测：造一个 96 条边界边 / 1 个边界环的开口球喂进去 → 输出仍有 100 条边界边，**stderr 一个字没提**，反而刷了 6 行 "Found repeated halfedge"。岛数只有一行 `Islands: N`（autoremesher.cpp:1026-1028） | 新增输入体检 + 结果里带 in/out 拓扑对比 |
| 2 | 没有对称选项 | **成立** | src/ 全库 symmetry/symmetric/mirror 零命中；唯一"对称"是 cross field 的 4 重旋转不变性（quadparameterizer.cpp:624-635），与建模对称无关 | 需要实现（见 P1-3） |
| 3 | --target-quads 不是承诺 | **部分成立（已有开关，默认关且 help 不写精度）** | 默认路径实测达成率：12k→10 418（86.8%）、20k→14 766（73.8%）、multi_part 6k→4 317 面（72%）。原因有二：`voxelSize=sqrt((A/(2N))/0.4330)` 里的 0.4330 让 A/s²≈0.87N 先天低 13%（autoremesher.cpp:157-165）；adaptivity 把 `faceScaling` 乘进 footprint，平坦区 clamp 到 3，配额按 1/9 塌缩（parameterizer.cpp:269-289、quadparameterizer.cpp:698-720），而 s 仍取全网格平均边长、不做归一 → 大平面积主导的模型（鞋）掉到六折。代码自己写了 "can leave the requested count off by 2x"（autoremesher.h:101-104）。`--exact-target-quads` 实测 12k→12 531（+4.4%）、20k→20 380（+1.9%）、multi 6k→6 064（+1.1%），代价 +10~15% 时间 | 默认开 exact / help 写清 `±5% / ±2%` 口径 |
| 4 | 参数纠缠、没有预设 | **成立，且比说的更糟** | 无预设层；`--model-type` 两档里 hard-surface 空转（见上）→ 实际只有一档。穿插的四个旋钮确实是独立标量（main.cpp:528-539） | 需要实现（P1-2），同时删/实现死参数 |
| 5 | 输出没有 UV、没有法线 | **成立（数据在内存里，链路断在导出）** | writeObj 只写 v/f（main.cpp:384-393），实测 vt=0 vn=0。parameterizer 的 UV 仍在内存（`takeTriangleUvs`→`m_isotropicTriangleUvs`，autoremesher.cpp:863-867/930-940，访问器 autoremesher.h:187-195），但对应 isotropic 三角网而非输出四边形；crossPoints 是纯 Vector3（quadextractor.cpp:121-124），抽取后没有"顶点→UV"。导 vt 还缺三件：cross point 保留岛内 UV、每岛 UV 平移、合并时重排（autoremesher.cpp:966-985） | P1-1，性价比最高 |
| 6 | 单核跑 | **成立但机制要说清** | 岛是并行单位（autoremesher.cpp:741-745、920-923），单岛模型 = 1 核是设计结果。实测：sphere（1 岛）cores 1.00；multi_part 24.41 核 / 3.45 s。串行的正是重活：IsotropicRemesher 全串行、CLS/MILS 求解、cover 混合整数迭代 | 接受现状 + 文档写清；长期看两级分解（P2） |
| 7 | 没有机器可读输出 | **误读（已存在）** | `--json` 发 start/progress/report/result/error（main.cpp:450-480/511-520/591-607），result 带 quads/nonQuads/vertices/seconds；`--verbose` 时 report 事件含逐阶段毫秒与 cores busy。实测末行即 `{"type":"result",...}`，不需要正则 | 无需新增，只需加字段（P0-1） |

## 三、后置环节 3 条

| # | 评价 | 判定 | 事实 |
|---|---|---|---|
| 8 | 自检全靠外部 | **成立（分层是设计，但契约缺口真实）** | `tools/metrics/mesh_metrics.py` 已经能算 boundary_loops/vertices、edges_non_manifold、components、n-gon、valence 直方图、quad_ratio（实测 0 边界环 0 非流形 100% quad 都对）。但核心只把它当外部；内层已算好却卡在 stderr 的量：岛数、简化器 before/after、非流形/开边（`AR_TOPO_STATS` 才打）、奇异点坐标（parameterizer.cpp:418-432） |
| 9 | 没有偏差报告 | **部分成立** | `--ref` 给 out→ref 的 mean/p50/p95/max + 相对值，和反向覆盖率（实测 sphere 12k：dev_p95_rel 3.0e-3、coverage 100%）。缺：对称 Hausdorff 单值、逐点法线偏差；采样只在顶点上取，是精度上限（metrics README §5 六条局限） |
| 10 | 四边面率 ≠ 拓扑质量 | **成立** | valence 有（histogram/mean/valence4_ratio/irregular_vertices），**pole 分布无**（JSON 无 pole/singular 键），**edge loop 顺滑度无**（只有 boundary_loops 计数与 max_dihedral 间接信号） |

---

## 四、确实是缺口 → 改进清单（按性价比）

### P0（诉讼级：数据已经在手里，只差导出；1–2 天）
1. **`result` 事件补结构化字段**：`islands`、`coreBusy`、`phases:[{name,ms,accumulated}]`、`targetRequested/targetActual`、`topology:{boundaryEdgesIn/Out, loopsIn/Out, nonManifoldIn/Out, components}`、`holes:{filled,ignored,facesAdded}`、`patch:{kept,dropped}`。phaseReport 已是结构化数据（autoremesher.cpp:1019-1087），只是被拼成字符串。
2. **输入体检**：remesh 前算 boundary edges/loops、non-manifold、components、degenerate、bbox 尺度；超阈值出 `{"type":"warning"}` + stderr 一行。实测那条 96 边界边的开口球就是活证据。
3. **洞与 patch 汇总**：`fixHoles` 已有逐洞 `Fixing hole (graded) at length:N` / `Ignore long hole at length:N` / `added X face(s)`（quadextractor.cpp:1410-1432），聚合成计数即可；注意 fixHoles 被调两次（225/267）→ 日志会翻倍，需去重。>65 边的洞默认不补，必须显式 warning。
4. **修 `--quiet` / `--verbose`**：核心无条件把 phaseReport 打到 stderr（autoremesher.cpp:1090-1091），CLI 再打一遍（main.cpp:595-597）→ verbose 时每行重复；`--quiet` 实测仍有 72 行 stderr；QuadExtractor 裸 cerr（quadextractor.cpp:220/222/293/295/2835-2870）刷屏。统一到一个 log level。
5. **target-quads 口径**：默认开 exact，或 `|err|>10%` 自动重解一次；help/README 写明"默认 ±5~28%，exact ±5%"。
6. **删或实现 `--model-type`**：现在是骗人的空开关（且上游一直如此），要么接上 sharp-edge/anisotropy 组合，要么移除并换成真预设。

### P1（1–2 周）
1. **vt/vn 导出**：cross point 保留岛内 UV → 每岛 UV 平移 → 合并重排；vn 从 quad 面重算。这是下游最痛的一项（要重展一次 UV）。
2. **预设层**：`--preset organic|hardsurface|prop|cloth|terrain`，映射到 adaptivity/anisotropy/sharp-edge/edge-scaling 的组合，并把 `--model-type` 变成 preset 的别名。
3. **对称**：先做后处理（镜像 + 中缝焊接 + 对称校验报告），再做帧场/cover 的对称约束。这是评价里说的"最大质量杠杆"，也是最贵的。
4. **pole / edge-loop 指标**进 metrics：奇异点已由 parameterizer 收集（index+坐标），落成分布/聚类即可；edge loop 顺滑度用沿环二面角序列的均值/方差 + 与主曲率方向的夹角。

### P2（结构性）
1. 单岛并行：只有"精确两级分解"一条路（OPTIMIZATION_LOG §8.1-8.4 已论证）。
2. 偏差进核心/GUI：把 metrics 的 TriGrid 采样搬进 C++，或 GUI 直接调 `mesh_metrics.py`；同时把顶点采样改成面采样、加对称 Hausdorff。
3. 岛数口径统一：metrics 的 components = 面经共享边，core 的 MeshSeparator = 顶点连通 → 同一模型两边数字不同，跨工具对比前需对齐。

---

## 五、核对时顺手发现的 bug / 文档债
1. ~~`--model-type` 全链路空转~~ → 已修：改成 preset 表，GUI 下拉框会同步四个滑块。
2. ~~`--verbose` 阶段报告打印两遍；`--quiet` 不静音核心与 extractor 的裸 cerr~~ → 已修（core 走 Log 汇，实测 quiet 0 行、verbose 无重复）。
3. ~~docs/cli-protocol.md:47 承诺 `{"type":"log"}` 事件，代码从未发出~~ → 已修：文档改为实际发出的 `start/input/warning/progress/report/result/error`。
4. `fixHoles` 单次运行调用两次，逐洞日志与"补了几个"的计数会翻倍。
5. docs/ARCH-REFACTOR-MERGE-PLAN.md §0 称 S2 门禁 R1–R8 已完成，docs/merge/02 §2.2 实测只实现 R1/R2 —— 两处口径冲突。
