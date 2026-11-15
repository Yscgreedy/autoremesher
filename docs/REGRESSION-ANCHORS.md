# 回归锚点（master 现值，2026-11-15 整体回归）

> 机读版本：[regression-anchors.json](regression-anchors.json)。

## 0. 这一版为什么又变了（先读这段）

2026-11-15 的整体回归结论：**几何没有回归**。

- `body_vox`、`body_clean` 的**几何通道**（`v` 行 + `f` 的顶点索引）与 UV v2 之前的锚点输出**逐字节一致**；
- `head_in` 的几何在同一二进制下 5 次里有 2 种取值，逐点最大差 **4.4e-16**（TBB 归约顺序，老问题）；
- 整文件 sha 变了，因为 **UV 通道变了**：UV v2 之后 vt 的取值与条数都不同（per-corner → 每面 chart，规范化口径也改了）。

所以锚点现在记**两个判据**：

| 判据 | 用途 |
| --- | --- |
| `geometry_mode_sha256` | **真正的回归判据**：几何一变就是回归 |
| `mode_sha256`（整文件） | 变更 UV/法线写出方式时会变，属于"需要显式重生成锚点"的变更，不是回归 |

## 1. 怎么测的

```bash
IN=E:/ComfyUI-aki-v3/zcode_work/char3d/remesh
BIN=build/cli/dist/autoremesher.exe
for i in 1 2 3 4 5; do
  "$BIN" --input "$IN/body_vox.obj" --output /tmp/anch/body_vox.r$i.obj \
    --target-quads 15000 --sharp-edge 60 --adaptivity 0.3
done
```

默认参数即被测行为：exact 目标数、`--uv-mode corner`、vt/vn 都写。

## 2. 锚点

| case | flags | faces | quads | vertices | vt | vn | 整文件 sha（命中） | 几何 sha |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- | --- |
| `body_vox` | `--target-quads 15000 --sharp-edge 60 --adaptivity 0.3` | 15445 | 15351 | 15481 | 61856 | 15481 | `54160deab29f7f79…`（5/5） | `96ec7580cde4f100…`（1 种） |
| `body_clean` | 同上 + `--anisotropy 1` | 15567 | 15503 | 15577 | 62318 | 15577 | `b0edc826b9fe7095…`（5/5） | `4489568b7cc4bdf8…`（1 种） |
| `head_in` | `--target-quads 10000 --sharp-edge 60 --adaptivity 0.5 --anisotropy 1` | 10470 | 10032 | 10717 | 42106 | 10717 | `256c58080c39c3da…`（4/5，2 种） | `ff1928359906db02…`（2 种，差 ≤4.4e-16） |

耗时（同一台机器、单跑）：body_vox ≈ 5.3 s，body_clean ≈ 6.0 s，head_in ≈ 7.8 s。

## 3. 结构/质量基线（`tools/metrics/mesh_metrics.py`）

| case | 四边面率 | n-gon | 非流形边 | 边界环 | 分量 | valence4 | 偏差 p95(rel) | 覆盖率 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `body_vox` | 99.39% | 85 | 8 | 0 | 1 | 96.89% | 8.86e-4 | 98.22% |
| `body_clean` | 99.59% | 57 | 20 | 0 | 1 | 97.83% | — | — |
| `head_in` | 95.82% | 332 | 48 | 1 | 82 | 81.61% | 2.26e-3 | 77.03% |

`head_in` 是刻意保留的脏输入（82 个分量、1 条边界环），数字低是输入本身的问题，不是回归；它的价值在于稳定的结构字段。

## 4. 判定规则

| 场景 | 判据 |
| --- | --- |
| 任何几何/数值改动 | `geometry_mode_sha256` 对得上 → 通过；body_vox/body_clean 任一几何 sha 变化即回归 |
| 只改 UV/法线写出 | 整文件 sha 会变：确认顶点/面计数与几何 sha 不变后，重生成本文档 |
| `head_in` | 几何 sha 取众数即可；那 2 种取值相差 ≤4.4e-16，别单次判红 |

## 5. 与 1.2.0 输出的关系（已验证）

`--legacy-target-quads --no-uv --no-normals` 仍逐字节复现 1.2.0：

```text
body_vox   898e2881a6ac1a1e…
body_clean 876092b1e07d7681…
head_in    bf0a28677bc1c89b…
```

## 6. 明确的坑

1. 整文件 sha ≠ 几何回归判据（见 §0）。
2. 对比几何时用 `--no-uv --no-normals`。
3. 输入不在仓库（140 MB），锚点只记录 sha256。
4. `--exact-target-quads` 现在是默认；写基线脚本时不要再叠加它，也不要期待旧的四边数。
