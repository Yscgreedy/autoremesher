# tools/metrics — 网格质量度量与前后对比

AutoRemesher 架构改造的**验收仪表**。纯 numpy 实现（不依赖 scipy），用于在改造前后对
remesh 产物做可复现的量化对比，并为 `docs/ARCH-REFACTOR-MERGE-PLAN.md` 中的
验收门与合入检查提供数字依据。

- 工具：`tools/metrics/mesh_metrics.py`
- 自检：`tools/metrics/test_metrics.py`（60 条断言，合成网格已知答案）
- 样本输出：`tools/metrics/selfcheck/*.json`

## 1. 依赖与环境

| 项 | 值 |
|---|---|
| Python | 3.12.14（DSH 工作区自带：`C:\Users\<user>\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe`） |
| numpy | 2.3.5 |
| scipy | 不存在（本工具刻意不依赖；连通分量、KD 网格哈希均自实现） |

在 PowerShell 中设置 `$env:PYTHONIOENCODING='utf-8'` 可避免中文/特殊字符在 cp936 控制台乱码。

**两个解释器，别用错**（2026-11-13 本机实测）：

| 解释器 | 版本 | numpy | 能否跑本工具 |
|---|---|---|---|
| 系统 `python` | 3.14.8 | **无** | ❌ `import numpy` 直接 `ModuleNotFoundError`（`mesh_metrics.py:34` / `test_metrics.py:16`） |
| DSH 自带 | 3.12.14 | **2.3.5** | ✅ `test_metrics.py` 全绿（60/60） |

```bash
PY="C:/Users/yschan/.dsh/dsh-runtimes/dsh-primary-runtime/dependencies/python/python.exe"
PYTHONUTF8=1 "$PY" tools/metrics/test_metrics.py     # 期望：passed 60 / 60
```

Windows 控制台（cp936）下中文帮助会乱码，**必须**带 `PYTHONUTF8=1`（或 `PYTHONIOENCODING=utf-8`）：

```text
$ python mesh_metrics.py --help                 # 乱码：中文变成 ????
AutoRemesher ??????????????numpy-only...
$ PYTHONUTF8=1 python mesh_metrics.py --help    # 正常
AutoRemesher 网格质量度量（numpy-only）：拓扑/几何指标 + 参考网格偏差与覆盖率 + 基线对比。
```

**在仓库内调用不要假设 `artifacts/` 存在** —— 本仓库当前没有这个目录，也不需要它：

- `mesh_metrics.py measure` 只吃「一个目标 OBJ（+ 可选 `--ref` 输入 OBJ）」，输出写到 `--json` 指定的任意路径（`/tmp`、`build/`、任何临时目录都行），**没有任何隐含的仓库路径依赖**。
- `metrics_meta.json` **不是 `measure` 写的**（工具只写 `--json` 那一份）；它是基线采集批处理脚本的产物，只有做 `artifacts/` 全套 A/B 时才需要。
- `emit_comparison.py` 需要 `<root>/artifacts/{baseline,after}/<case>/` 整批数据；缺数据时按设计 `exit 2`（前置不可生成）。**没有 artifacts 批次时不必调用它**，`mesh_metrics.py compare` 已经足够做两两判定。

## 2. 用法

### 2.1 单网格度量

```bash
python tools/metrics/mesh_metrics.py TARGET.obj
python tools/metrics/mesh_metrics.py TARGET.obj --json out.json
python tools/metrics/mesh_metrics.py TARGET.obj --ref INPUT.obj --json out.json
python tools/metrics/mesh_metrics.py TARGET.obj --ref INPUT.obj --samples 50000   # 大参考网格限采样
```

| 选项 | 说明 |
|---|---|
| `--ref REF.obj` | 参考（输入）网格，计算偏差与覆盖率 |
| `--json [PATH]` | 输出 JSON；只写 `--json` 时落到 `<target>.metrics.json` |
| `--samples N` | 输出顶点采样上限（默认全部）；确定性等距采样 |
| `--ref-samples N` | 参考顶点采样上限（默认同 `--samples`） |
| `--max-ring N` | 网格哈希扩环上限（默认 8），极端离散点可调大 |
| `--quiet` | 静默：不打印报告，只写 JSON / 只用退出码 |

### 2.2 基线 vs 候选对比

```bash
python tools/metrics/mesh_metrics.py compare baseline.json candidate.json
python tools/metrics/mesh_metrics.py compare baseline.json candidate.json --json cmp.json
python tools/metrics/mesh_metrics.py            # 无参：比较当前目录 baseline.json 与 candidate.json
```

输出并排表 + 回归/告警清单；**退出码 0 = 无回归，1 = 检出回归**，可直接用于 CI 门禁。

| 选项 | 默认 | 说明 |
|---|---|---|
| `--quad-tol PP` | 1.0 | 四边面占比允许下降的**百分点**（绝对值） |
| `--dev-tol R` | 0.02 | `deviation_p95` 允许的**相对**增长 |
| `--shell-tol R` | 0.02 | `components_largest_face_ratio` 允许的下降（绝对值，即 2 pp） |

## 3. 指标定义

输入：OBJ 的 `v` / `f`。支持 n 边形、1 基与负（相对）索引、同文件多对象（`o`/`g`）、
`f v/vt/vn` 与 `f v//vn`（只取顶点号）、「首尾重复顶点」写法（`v1 v2 v3 v4 v1`）。
面以 CSR（`face_starts` + `face_idx`）存储，全流程向量化。

| 指标 (JSON 键) | 定义 |
|---|---|
| `vertices` / `faces` | 顶点行数 / 面数 |
| `quads` / `quad_ratio_pct` | 4 顶点面数与占比 = quads / faces |
| `tri_faces` / `ngon_faces` | 三角面数 / 顶点数 > 4 的面数 |
| `face_size_histogram` | 面顶点数直方图（如 `{"3":99,"4":4810,"5":233}`） |
| `edges_unique` | 无向唯一边数（按 `min,max` 顶点号归一） |
| `edges_interior` | 恰被 2 个面共享的边（流形内部边） |
| `edges_boundary` | 只被 1 个面使用的边 |
| `edges_non_manifold` | 被 **> 2** 个面共享的边；`edge_multiplicity_max` 给出最大重数 |
| `boundary_loops` | 边界边图（顶点为节点）的连通分量数，即边界环数；`boundary_vertices` 为边界顶点数 |
| `components` | **面经共享边**相连得到的连通分量数（顶点只共点不算连通，即 shell 数） |
| `components_largest_face_ratio` | 最大分量的面数 / 总面数（主壳占比，1.0 = 单一壳；0.95 表示最大壳只占 95% 的面）。**仅适用于单部件输入** |
| `components_top2_face_ratio` | 前 2 大分量面数之和 / 总面数。**多部件输入的 G4 主判据** |
| `components_top3_face_ratio` | 前 3 大分量面数之和 / 总面数（比 top2 稳健：第三个大块也能兜住） |
| `components_face_histogram` | 各分量面数**降序**取前 10，如 `[4899, 15, 11, 10, …]`；第二个数开始即碎片规模 |
| `components_small_face_count` | 面数 **≤ 3** 的分量个数（三角/四边级的漂浮碎片计数） |
| `degenerate_faces` | 面积 ≤ 1e-12·diag² 的面，或面内出现重复顶点下标的面 |
| `valence_histogram` | 顶点价（入射唯一边数）直方图；四边网格最关键的局部质量指标 |
| `valence4_ratio` / `irregular_vertices` | 价 4 顶点占比 / 非价 4 顶点数 |
| `normal_inconsistent_edges` | 相邻面法向夹角 > 90° 的**面邻接对**数（流形网格即边数）。用于抓翻面/折叠；对非四边输入，> 90° 也可能是真实锐折（如耳廓、鼻孔） |
| `max_dihedral_deg` | 观测到的最大相邻面法向夹角（180° = 完全翻转） |
| `bbox` | 包围盒 min/max/size 与对角线 `diagonal`（仅统计被面引用的顶点） |
| `area_total` | 表面积（每面用 Newell 公式求和，n 边形一次算完，非三角化近似） |
| `area_by_diag2` | 面积 / diag²，无量纲，便于跨尺度比较 |
| `edge_length` | 唯一边的 min / p05 / median / mean / p95 / max / std |
| `triangles_after_fan` | 扇形三角化后的三角面数（参考对比用同一口径） |
| `unreferenced_vertices` | 未被任何面引用的顶点数（悬空点，通常应为 0） |
| `parse_seconds` / `compute_seconds` / `total_seconds` | 解析 / 指标 / 全流程耗时 |

### 3.1 参考网格对比（`--ref`）

```
ref 平均边长 avg_e（唯一边均值） → cell = 2 · avg_e
三角形按包围盒登记到覆盖的所有 cell（均匀网格哈希）
点在 cell 内查候选三角形；未命中按 Chebyshev 环 r = 1,2,… 向外扩环
首个命中环 r 之后继续扫 r + 1 环（margin，防大三角形跨 cell 误判）
```

| 指标 | 定义 |
|---|---|
| `ref.deviation.mean/p95/max` | **输出顶点 → 参考表面**最近距离（点到三角形精确距离：平面投影 + 三段边兜底）的 mean / p95 / max |
| `ref.deviation.mean_rel/p95_rel/max_rel` | 同上除以**目标包围盒对角线**，无量纲 |
| `ref.coverage.covered_ratio` | **参考顶点被输出覆盖**的比例：ref 顶点到输出表面最近距离 ≤ 阈值（阈值 = ref 平均边长） |
| `ref.coverage.ref_dev_mean/p95/max` | 参考顶点 → 输出表面的最近距离统计（即反向偏差） |
| `ref.deviation.unresolved` / `coverage.unresolved` | 扩环到上限仍未找到候选的点数（正常应为 0） |

采样：默认全量；`--samples` / `--ref-samples` 用 `np.linspace` 等距取样，结果确定可复现。

### 3.2 回归判定（compare）

| 级别 | 条件 |
|---|---|
| **回归** | `quad_ratio_pct` 下降 > `--quad-tol`（默认 1 pp） |
| **回归** | `edges_non_manifold` 增加 |
| **回归** | `ref.deviation.p95` 相对增长 > `--dev-tol`（默认 2%） |
| **回归** | `components` **增加**（壳裂开） |
| 告警 | `components` 减少（壳合并，通常是有意的，但仍需确认） |
| 告警 | `components_largest_face_ratio` 下降 > `--shell-tol`（默认 2 pp，主壳被碎片吃掉） |
| 告警 | `components_small_face_count` 增加 |
| 告警 | `boundary_loops` / `degenerate_faces` / `ngon_faces` / `normal_inconsistent_edges` 增加 |
| 告警 | `area_total` 相对变化 > 1% |
| 告警 | 候选缺 `ref.deviation.p95`（未带 `--ref` 运行） |

## 4. 自检结果

命令（DUT 为历史产物，只读，未修改源素材）：

```powershell
$py='C:\Users\<user>\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe'
$out='E:\ComfyUI-aki-v3\zcode_work\char3d\out'; $rem='E:\ComfyUI-aki-v3\zcode_work\char3d\remesh'
& $py tools\metrics\mesh_metrics.py "$out\head_quad_10k.obj"  --json tools\metrics\selfcheck\head_quad_10k.json --quiet
& $py tools\metrics\mesh_metrics.py "$out\head_quad_20k.obj"  --json tools\metrics\selfcheck\head_quad_20k.json --quiet
& $py tools\metrics\mesh_metrics.py "$out\body_quad_15k.obj"  --json tools\metrics\selfcheck\body_quad_15k.json --quiet
& $py tools\metrics\mesh_metrics.py "$out\head_quad_10k.obj" --ref "$rem\head_clean.obj" --json tools\metrics\selfcheck\head_quad_10k_ref_head_clean.json --quiet
& $py tools\metrics\mesh_metrics.py "$out\head_quad_20k.obj" --ref "$rem\head_clean.obj" --json tools\metrics\selfcheck\head_quad_20k_ref_head_clean.json --quiet
& $py tools\metrics\mesh_metrics.py "$out\head_quad_10k.obj" --ref "$rem\head_clean.obj" --samples 50000 --json tools\metrics\selfcheck\head_quad_10k_ref_s50k.json --quiet
```

### 4.1 三个产物的关键指标

| 指标 | head_quad_10k | head_quad_20k | body_quad_15k |
|---|---:|---:|---:|
| 顶点 | 5357 | 14788 | 8272 |
| 面 | 5142 | 14730 | 8214 |
| 四边面 | 4810 | 14303 | 8129 |
| **四边面占比** | **93.54%** | **97.10%** | **98.97%** |
| 三角面 / n>4 面 | 99 / 233 | 85 / 342 | 17 / 68 |
| 非流形边 (>2 面) | 124 | 188 | 12 |
| 边界边 / 边界环 | 178 / 5 | 95 / 3 | 77 / 1 |
| **连通分量** | **70** | **1** | **1** |
| 退化面 | 22 | 2 | 0 |
| 价 4 顶点占比 | 69.12% | 86.48% | 94.96% |
| 最大价 | 10 | 9 | 7 |
| 表面积 / 对角线 | 1.911416 / 1.336342 | 2.108098 / 1.336563 | 0.538798 / 1.056276 |
| 边长 median / p95 | 0.017164 / 0.035202 | 0.011722 / 0.020478 | 0.007932 / 0.013289 |
| 法向不一致边 | 1049 | 1163 | 139 |
| 耗时（无 ref） | 0.033 s | 0.075 s | 0.037 s |

与既有 `report_*.txt` 交叉核对一致：`report_head_10k` 记 Quads 4810 / Non-quads 332 / Vertices 5357；
`report_head_20k` 记 14303 / 427 / 14788；`report_body_15k` 记 8129 / 85 / 8272 —— 本工具分别得到
4810+332=5142、14303+427=14730、8129+85=8214 面，完全对齐。

> **顺带发现（历史产物缺陷，供改造验收参考）**：`head_quad_10k.obj` 是 **70 个边连通壳**
> （主壳 4899 面 + 69 个 2–15 面的漂浮小块）、22 个退化面、124 条非流形边、5 个边界环；
> `head_quad_20k.obj` 已收敛为 1 个壳（2 退化面），但非流形边反而更多（188）。
> 交叉验证：用独立 Python BFS/并查集重算 `head_quad_10k` 的连通分量同样得到 70，
> 边重数直方图 `{1:178, 2:10076, 3:124}` 与本工具一致。

### 4.2 `--ref` 对比（ref = `head_clean.obj`，199731 顶点 / 399708 三角面，平均边长 0.004122，cell 0.008244）

| 指标 | head_quad_10k | head_quad_20k |
|---|---:|---:|
| dev mean（输出→参考） | 0.001077 | 0.000778 |
| dev p95 | 0.002950 | 0.001920 |
| dev max | 0.011938 | 0.005469 |
| dev mean / 目标对角线 | 0.000806 | 0.000582 |
| dev p95 / 目标对角线 | 0.002207 | 0.001437 |
| **覆盖率**（thr 0.004122） | **60.97%** (n=199670) | **84.96%** (n=199731) |
| ref→out mean / p95 / max | 0.005369 / 0.019095 / 0.077391 | 0.002968 / 0.011306 / 0.058399 |
| 耗时：偏差 / 覆盖率 / 合计 | 2.34 s / 21.02 s / **23.67 s** | 7.21 s / 23.54 s / **31.03 s** |
| `--samples 50000` 合计 | **7.09 s**（覆盖率 60.94%，与全量偏差 0.03 pp） | — |

耗时结论：**参考侧开销 = 一次 40 万三角形网格构建（约 2–3 s）+ 参考顶点数 × 扩环查询**，
与目标网格大小关系不大。日常对比用 `--samples 50000`（≈7 s，覆盖率误差 < 0.05 pp）；
M0 冷基线或发布验收用全量（20–30 s）。

### 4.3 大输入 / 非四边输入

| 输入 | 顶点 / 面 | 指标 | 说明 |
|---|---|---|---|
| `head_in.obj` | 229282 / 399710（全三角） | 1.219 s（parse 0.69 + compute 0.51） | 8979 个壳、54014 条边界边、14520 个边界环 —— 未清理的原始生成网格 |
| `head_clean.obj` | 199731 / 399708（全三角） | 1.262 s | 1 个壳、4 条边界边、2 个边界环、3 条非流形边 |
| `body_vox.obj` | 39144 / 39146（全四边，体素提取） | 0.155 s | 1 个壳、四边面 100%、价 4 占比 89.65% |

`head_in.obj` 的 8979 个壳正是 `head_clean.obj` 清洗掉的东西 —— 这组数字可直接用作
改造计划里「输入清理」环节的验收基线。

### 4.4 单元自检

```bash
python tools/metrics/test_metrics.py     # 60/60 通过，退出码 0
```

覆盖：OBJ 解析特性（n 边形/负索引/`v//vn`/首尾重复/多对象/1 基转换）、
立方体（12 边、面积 6、价 3、闭合）、n×n 四边网格（E=2n(n+1)、边界 4n、内部 2n(n−1)、1 个边界环）、
两个孤岛（2 分量 / 2 边界环）、非流形（1 条三重边）、退化面、翻面（180°）、
偏差（自比 ≈0、平面平移 h 精确等于 h）、覆盖率（自比 100%）、compare 三档阈值、CLI 与退出码。

### 4.5 G4 判据：分量面数分布（artifacts/ 8 份实测，同 `--ref`、`--samples 20000`）

```powershell
# baseline 与 after 的 ref 一一对应；scaled10k 的 ref 是 1e-3 缩放的同一张头
#   head10k   -> remesh\head_in.obj
#   body15k   -> remesh\body_clean.obj
#   multi12k  -> artifacts\inputs\multi_part.obj
#   scaled10k -> artifacts\inputs\head_scaled_1e-3.obj
& $py tools\metrics\mesh_metrics.py measure artifacts\baseline\<case>\out.obj --ref <REF> --json artifacts\baseline\<case>\metrics.json --samples 20000 --quiet
```

| 用例 | 轮次 | faces | components | largest | **top2** | **top3** | small(≤3) | area | 覆盖率 |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| head10k | baseline | 5080 | 70 | 0.9522 | 0.9551 | 0.9573 | 48 | 1.92374 | 0.6111 |
| head10k | after | 5080 | 70 | 0.9522 | 0.9551 | 0.9573 | 48 | 1.92374 | 0.6111 |
| body15k | baseline | 10045 | 1 | 1.0000 | 1.0000 | 1.0000 | 0 | 0.547933 | 0.9552 |
| body15k | after | 10045 | 1 | 1.0000 | 1.0000 | 1.0000 | 0 | 0.547933 | 0.9552 |
| multi12k | baseline | 7225 | 142 | 0.4779 | 0.9334 | 0.9355 | 100 | 3.51790 | 0.4703 |
| multi12k | after | 7683 | 186 | 0.4592 | 0.8978 | 0.8999 | 110 | 3.53404 | 0.4680 |
| scaled10k | baseline | 2193 | 6 | 0.9922 | 0.9950 | 0.9973 | 2 | 1.07737e-06 | 0.2819 |
| scaled10k | after | 5204 | 70 | 0.9533 | 0.9562 | 0.9583 | 48 | 1.93362e-06 | 0.6080 |

前 10 分量面数：head10k `[4837, 15, 11, 10, 8, 8, 7, 7, 7, 7]`；
multi12k baseline `[3453, 3291, 15, 15, 11, 11, 10, 10, 8, 8]` / after `[3528, 3370, 16, 16, 12, 12, 11, 10, 10, 10]`；
scaled10k baseline `[2176, 6, 5, 4, 1, 1]` / after `[4961, 15, 11, 10, 8, 8, 7, 7, 7, 7]`。

参考输入自身拓扑（同一工具、无 `--ref`）：

| 输入 | faces | components | largest_face_ratio | small(≤3) |
|---|---:|---:|---:|---:|
| `head_in.obj`（= 1e-3 缩放同源） | 399710 | 8979 | 0.9656 | 8608 |
| `body_clean.obj` | 149710 | 1 | 1.0000 | 0 |
| `multi_part.obj` | 799420 | 17958 | 0.4828 | 17216 |

判读：

* **scaled10k 是最灵敏的用例**。1e-3 缩放下 baseline 只剩 **6 个分量 / largest 0.9922 / 2193 面 / 面积 1.077e-6 / 覆盖率 0.2819 / bbox diag 1.172e-3**，
  而该头的真值是面积 1.9237e-6、diag 1.33175e-3（正常尺度 head10k 逐项一致）——
  即 baseline 在 1e-3 尺度下**丢掉了 44% 的表面、bbox 缩了 12%**。after 恢复成
  **70 分量 / 0.9533 / 5204 面 / 面积 1.9336e-6（100.5%）/ 覆盖率 0.6080 / diag 1.33052e-3（99.9%）**，
  与正常尺度逐项对齐 → 尺度不变性回归被修好。
* **multi12k 不能只看 largest_face_ratio**：输入本身就是**两个几乎等大的部件**（`multi_part.obj` 前二 385976 / 385976，各占 0.4828），
  所以最大分量天然只有 ~0.48，largest 在这两轮里都动不了（0.4779 / 0.4592）。改用 top-k 才看得见差异：
  top2 0.9334 → 0.8978（−3.56 pp）、top3 0.9355 → 0.8999（−3.56 pp）、small 100 → 110、分量 142 → 186。
  结论：multi12k 上 after 的碎片化确实比 baseline 略多，且这个结论**只能由 top-k 给出**。

`compare` 自检（baseline vs after，`--json` 写临时目录）：head10k / body15k → `OK`（两轮逐字段相同，
退出码 0）；multi12k / scaled10k → `REGRESSION`（非流形边、分量数增加）。注意 scaled10k 的
`REGRESSION` 是**机械判定**：那里的「分量 6 → 70」正是修好的结果，判 G4 时必须和**正常尺度用例**比，
而不是只看 baseline→after 的方向。另：head10k / body15k 两轮**逐字段完全一致**
（largest / top2 / top3 / small / area / 覆盖率 / deviation 全部相同），说明改造在常规尺度下行为不变。

### 4.6 G4 推荐判据（实测校准）

三条，按输入形态选，不要混用：

1. **单部件 / 常规尺度**：`components_small_face_count ≤ 基线` **且** `components_top3_face_ratio ≥ 基线 − 2pp`。
   实测：head10k small 48→48、top3 0.9573→0.9573（Δ 0）；body15k 1 壳，top3 1.0000→1.0000（Δ 0）→ 两条都稳过。
2. **多部件输入**：**只用 top-k（k = 2/3），不要用 largest**。
   `multi_part.obj` 本身就是两个等大部件（各 0.4828），largest 上限就是 0.483，对碎片完全不敏感
   （142→186 分量只让 largest 动 1.87pp，却让 top2 动 3.56pp）。
   实测门槛：`top3 ≥ 基线 − 2pp` → multi12k 为 0.8999 vs 基线 0.9355，**差 3.56pp，不通过**（如实记录）。
3. **尺度不变性新用例**：`|largest(1e-3) − largest(1.0)| ≤ 0.02`（用同一张头在 1.0 与 1e-3 两档各跑一遍）。
   实测：baseline `|0.9922 − 0.9522| = 4.02pp`（**不合规**），after `|0.9533 − 0.9522| = 0.11pp`（**合规**）。
   这是三条里最灵敏的一条：另外两档配套证据也一致（面积 56% → 100.5%、覆盖率 0.2819 → 0.6080，
   正常尺度分别是 100% 与 0.6111）。

> 判据 2/3 存在张力：multi12k 的 top3 掉了 3.56pp，如果直接当硬门，after 会被判不合格；
> 而 scaled10k 的分量暴涨恰恰是修复。所以 G4 的「回归」必须绑定**对照用例**（正常尺度 vs 极端尺度、
> 单部件 vs 多部件），不要对 baseline→after 的方向一刀切。

## 5. 已知限制

1. `components` 用「面经共享边连通」定义；仅共用一个顶点的两组面会被判为 2 个分量（这是 shell 的正确口径）。
2. `boundary_loops` 是边界边图的顶点连通分量数；两环在单一顶点相切（pinch）会合并计为 1 环。
3. `normal_inconsistent_edges` 对非四边/高曲率输入（如耳廓、鼻孔、体素台阶边）会自然出现
   > 90° 的折角，这属于几何事实而非错误 —— 用它做前后对比（同输入同参数）才有效。
4. 点到曲面距离按扇形三角化口径计算；n 边形非平面时与「真曲面」有微小差异（远小于格距）。
5. 网格哈希的扩环策略是启发式（首个命中环 + 1 环）；`unresolved` 非 0 时调 `--max-ring`。
6. 只读 OBJ；不解析材质/纹理，`vt`/`vn` 仅计数。

## 6. 与前后对比任务（task-4）的对接

```bash
# 基线（改造前）
python tools/metrics/mesh_metrics.py ARTIFACT.obj --ref INPUT.obj --json artifacts/baseline/ARTIFACT.json
# 改造后（同输入、同参数）
python tools/metrics/mesh_metrics.py ARTIFACT.obj --ref INPUT.obj --json artifacts/after/ARTIFACT.json
# 判定（退出码 1 = 有回归，可直接卡 CI）
python tools/metrics/mesh_metrics.py compare artifacts/baseline/ARTIFACT.json artifacts/after/ARTIFACT.json --json artifacts/after/cmp-ARTIFACT.json
```

注意：基线/候选 JSON 必须来自**同一 `--ref`、同一 `--samples`**，否则覆盖率不可比。
`metadata`（ref 路径、采样上限）都在 JSON 的 `ref` 段里，可直接核对。

## 7. 合入后的落地状态（2026-11-13）

本目录自 `arch-refactor` 原样搬迁，**四个可执行文件与 10 份自检样本均与 arch 侧逐字节一致**（`mesh_metrics.py` sha256 = `9456ba7b…d0b`），以此保证 `metrics_meta.json` 里记录的 `mesh_metrics_sha256` 判据继续有效；换工具版本会让既有 JSON 全部不可比 —— **改这个工具就必须整批重测基线**。

| 检查 | 命令 | 结果 |
|---|---|---|
| 自检 | `PYTHONUTF8=1 "$PY" tools/metrics/test_metrics.py` | `passed 60 / 60` |
| remesh 冒烟 | `autoremesher.exe --input body_vox.obj --output out_15k.obj --target-quads 15000 --sharp-edge 60 --adaptivity 0.3` | `quads 7961 / nonQuads 92 / vertices 8073 / 3.656 s` |
| 度量冒烟 | `measure out_15k.obj --ref body_vox.obj --samples 20000 --json out_15k.metrics.json` | `quads=7961, non_quad_faces=92, components=1, largest=1.0, edges_non_manifold=4` |
| 对比：有回归 | `compare out_15k.metrics.json out_3k.metrics.json` | `Verdict: REGRESSION`，退出码 **1**（`quad ratio drop 1.48 pp`、`deviation p95 +220.81%`） |
| 对比：无回归 | `compare out_3k.metrics.json out_15k.metrics.json` | `Verdict: OK`，退出码 **0** |
| 自比对 | `compare out_15k.metrics.json out_15k_copy.metrics.json` | `Verdict: OK`（`No regression detected.`），退出码 0 |

两个口径提醒：

1. **"non-quads" 在指标 JSON 里的键名是 `non_quad_faces`**（不是 `non_quads`）；CLI 的 `--json` 事件里叫 `nonQuads`。两者数值一致（本次均为 92），但键名不同，脚本里别混用。
2. `compare` 的退出码就是判据（1 = 有回归），可直接卡 CI；但**基线与候选必须同 `--ref`、同 `--samples`**，否则覆盖率那几列不可比。

## 8. 输入约定（OBJ / metrics.json / run.json）

三个层次的输入，**只有第 1、2 层是必需的**；第 3 层只有在你想复刻 `artifacts/` 那套 A/B 汇总件时才需要。

### 8.1 OBJ（必须）

- 任意路径的 Wavefront OBJ：支持 n 边形、1 基与负（相对）索引、同文件多对象（`o` / `g`）、`f v/vt/vn` 与 `f v//vn`（只取顶点号）、「首尾重复顶点」写法。只读几何；`vt`/`vn` 仅计数。
- 仓库里**没有**内置样例模型，也没有 `artifacts/inputs/`；把 OBJ 放哪儿都行，命令里给绝对路径最省事（Windows 路径用正斜杠或转义反斜杠）。
- 参考网格（`--ref`）必须是**同一模型的输入网格**，且与候选同 `--samples`；`ref` 段会记录 `ref.file`、`ref.ref_stats`、`deviation`、`coverage`，可事后核对口径。

### 8.2 metrics.json（必须，由 `measure` 产生）

- 结构：顶层是单网格指标（`vertices`/`faces`/`quads`/`non_quad_faces`/`face_size_histogram`/`edges_*`/`valence_histogram`/`components*`/`boundary_loops`/`degenerate_faces`/`bbox`/`area_total` …），带 `--ref` 时多一个 `ref` 段（`ref_stats` / `deviation` / `coverage`）。完整键表见 §3。
- `compare` 只依赖这份 JSON；**退出码即判据**（1 = 有回归，0 = 通过），因此可以直接当 CI 门。
- 唯一性：同一 OBJ + 同一参数 → 同一 JSON（`--samples` 的等距采样是确定性的），所以 JSON 可以入库当基线。

### 8.3 run.json / metrics_meta.json（可选，仅 artifacts 批次）

这两个文件**没有任何工具在本仓库里生成**，它们是采集脚本的落盘约定，`emit_comparison.py` 读取它们来生成三份汇总件。要做整套 A/B 时按下列键写：

| 文件 | 键 | 用途（`emit_comparison.py` 读法） |
|---|---|---|
| `run.json` | `input` | 输入 OBJ 路径（`:365`），用于「输入碎片化」列与短名 |
| `run.json` | `wall_seconds` | 进程墙钟（`:30`、`:437`） |
| `run.json` | `sha256_out` | 输出 OBJ 的记录值（`:32`、`:244`）；生成器会**自己 hash `out.obj` 再比**，不一致 → exit 3 |
| `run.json` | `run2.sha256_out` / `reproducible_sha256` | 两次运行是否逐字节一致（`:246-251`） |
| `run.json` | `captured_at` | 采集时点（`:596`） |
| `metrics_meta.json` | `mesh_metrics_sha256` | 度量工具版本指纹（`:140`）；必须等于当前 `mesh_metrics.py` 的 sha256，否则整批不可比（D7） |
| `metrics_meta.json` | `samples` | 采样口径（`:270`）；各 case 必须一致 |
| `report.txt` | `Total time:` | 程序内计时（`:31`、`:297-301`）；与 `wall_seconds` **不同源**，不可互相印证 |

目录形状（`emit_comparison.py:408-412`）：

```text
artifacts/baseline/<case>/{out.obj,report.txt,stdout.txt,stderr.txt,run.json,run2/,metrics.json,metrics_meta.json}
artifacts/after/<case>/{out.obj,report.txt,stdout.txt,stderr.txt,run.json,metrics.json,metrics_meta.json}
artifacts/baseline/SUMMARY.md   artifacts/after/SUMMARY.md   artifacts/COMPARISON.md
```

缺任何一件 → `emit_comparison.py --check` 按设计 `exit 2`（前置不可生成），不是 bug。
