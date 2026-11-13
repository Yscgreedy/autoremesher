# 回归锚点（master 现值，2026-11-13 实测）

> 机读版本：[regression-anchors.json](regression-anchors.json)（同一份数据，逐用例含全部 5 次 sha）。
> 这份文件替代 arch-refactor 计划 §1.2 那组 `8101 / 9982 / 4720` 的数字：那是在 arch 侧 core 上测的，master 的 perf 改动（quadextractor 增量邻接、singularitysimplifier 复用 scratch、quadparameterizer 重写）已经让它们漂移了，实测 `body_vox` 现在是 **7961 quads / 92 non-quads / 1 component**。硬抄旧值当基线的人会得出「回归」的假结论。

## 1. 怎么测的

```bash
# 输入不在仓库里（140 MB），路径按需替换
IN=E:/ComfyUI-aki-v3/zcode_work/char3d/remesh
BIN=build/cli/dist/autoremesher.exe     # cmake -S . -B build/cli -DCMAKE_BUILD_TYPE=Release
for i in 1 2 3 4 5; do
  "$BIN" --input "$IN/body_vox.obj" --output /tmp/anchors/body_vox.r$i.obj \
    --target-quads 15000 --sharp-edge 60 --adaptivity 0.3
done
sha256sum /tmp/anchors/body_vox.r*.obj
```

每个用例跑 **5 次**，记录 sha256、字节数、面数。之所以要 5 次而不是 1 次：见 §3。

## 2. 锚点

| case | flags | input sha256 | output sha256 (mode) | quads | non-quads | distinct sha / 5 runs | bytes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `body_vox` | `--target-quads 15000 --sharp-edge 60 --adaptivity 0.3` | `8f04aa7772ade54f…` | `898e2881a6ac1a1e…` | 7961 | 92 | 1 | 701942 |
| `body_clean` | `--target-quads 15000 --sharp-edge 60 --adaptivity 0.3 --anisotropy 1` | `6df100080ef8f862…` | `876092b1e07d7681…` | 10131 | 74 | 1 | 890888 |
| `head_in` | `--target-quads 10000 --sharp-edge 60 --adaptivity 0.5 --anisotropy 1` | `86684c0ccbf8a576…` | `bf0a28677bc1c89b…` | 4718 | 389 | 2 | 455748..455750 |

结构字段（`tools/metrics/mesh_metrics.py measure --ref <输入>`，body_vox @15000 实测）：

| 字段 | 值 |
| --- | --- |
| `quads` | 7961 |
| `non_quad_faces` | 92 |
| `components` | 1 |
| `components_largest_face_ratio` | 1.0 |
| `edges_non_manifold` | 4 |
| `boundary_loops` | 0 |
| `degenerate_faces` | 4 |
| `valence4_ratio` | 95.07 % |
| `deviation_p95`（对输入） | 0.000939 |
| `coverage`（对输入） | 96.39 % |

## 3. 判定规则（为什么不能所有用例都比 sha）

| 用例 | 判据 | 理由 |
| --- | --- | --- |
| `body_vox` | **逐字节**：等于 `898e2881…` | 5/5 稳定（单连通分量） |
| `body_clean` | **逐字节**：等于 `876092b1…` | 5/5 稳定 |
| `head_in` | **结构 + 众数**：quads 4718 / non-quads 389 / faces 5107 必须相等；sha 取众数 `bf0a2867…`，出现 2 个不同 sha 属正常 | 70 个连通分量，TBB 并行归约顺序会让 4 行顶点坐标的最后 1~2 位有效数字漂移（455748 ↔ 455750 B）。**master 自己的二进制单独跑就能复现两种值**，不是哪个改动引入的 |

任何一次结构改动之后：先跑 `body_vox`/`body_clean` 比 sha，二者不等就是真回归；`head_in` 只看结构字段与 sha 众数，别拿单次 sha 当判据。

## 4. 明确的坑

1. **别用 arch 计划 §1.2 的数字**（8101 / 9982 / 4720）：口径与现值都不同，且那组值本身是「某次基线快照」，不是常量。
2. **`head_in` 不要比单次 sha**：会造成假红。要更强判定就用 `mesh_metrics.py compare` 的容差（见 `tools/metrics/README.md`）。
3. **输入文件不在仓库**：`artifacts/`（140 MB）刻意不提交，锚点只记录输入的 sha256，不复现输入本身。
4. **默认参数 ≠ 锚点参数**：CLI 默认 `--target-quads 20000 / --sharp-edge 60 / --adaptivity 0.5`，上表用的是显式参数；写基线脚本时把参数抄全。
