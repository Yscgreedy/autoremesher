# 回归锚点（master 现值，2026-11-15 实测）

> 机读版本：[regression-anchors.json](regression-anchors.json)（同一份数据）。
> 这一版换了两件事，旧数字不能直接比：**--target-quads 默认走 exact 闭环**，
> 且**输出 OBJ 现在带 vt/vn**（sha 覆盖整个文件）。旧值仍可复现，见 §4。

## 1. 怎么测的

```bash
IN=E:/ComfyUI-aki-v3/zcode_work/char3d/remesh
BIN=build/cli/dist/autoremesher.exe     # cmake -S . -B build/cli -DCMAKE_BUILD_TYPE=Release
for i in 1 2 3 4 5; do
  "$BIN" --input "$IN/body_vox.obj" --output /tmp/anchors/body_vox.r$i.obj \
    --target-quads 15000 --sharp-edge 60 --adaptivity 0.3
done
sha256sum /tmp/anchors/body_vox.r*.obj
```

每个用例跑 **5 次**，记录 sha256、字节数、面数。默认参数就是被测行为：exact 目标数、
vt/vn 导出、有机预设。

## 2. 锚点（exact 默认）

| case | flags | quads | non-quads | faces | bytes | distinct sha / 5 | mode sha256 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| body_vox | --target-quads 15000 --sharp-edge 60 --adaptivity 0.3 | 15351 | 94 | 15445 | 5715702 | 1 | 7343baa9d9cf94cc… |
| body_clean | --target-quads 15000 --sharp-edge 60 --adaptivity 0.3 --anisotropy 1 | 15503 | 64 | 15567 | 5750486 | 1 | bf024a592411e69e… |
| head_in | --target-quads 10000 --sharp-edge 60 --adaptivity 0.5 --anisotropy 1 | 10032 | 438 | 10470 | 3856875 | 1 | 35c49fee5491aebe… |

目标数达成率：body_vox 15351/15000 = +2.3%，body_clean +3.4%，head_in +0.3%。

## 3. 判定规则

| 用例 | 判据 | 理由 |
| --- | --- | --- |
| 全部三个用例 | **逐字节**：sha 等于 mode_sha256 | 2026-11-15 实测 5/5 稳定，head_in 也不再漂移 |
| 任何结构改动后 | 先跑 body_vox 与 body_clean 比 sha | 二者不等就是真回归 |

head_in 之前出现的两种 sha（455748/455750 字节）是 TBB 并行归约顺序造成的；这一版实测
5/5 一致，但再出现漂移仍按"结构相同 + sha 取众数"处理，别单次就判红。

## 4. 与 1.2.0 输出的关系（已验证）

旧基线（未开 exact、无 vt/vn）逐字节可复现，用：

```bash
"$BIN" --input "$IN/body_vox.obj" --output /tmp/anchors/legacy.obj \
  --target-quads 15000 --sharp-edge 60 --adaptivity 0.3 \
  --legacy-target-quads --no-uv --no-normals
# body_vox   898e2881a6ac1a1e…  (与旧 regression-anchors.json 一致)
# body_clean 876092b1e07d7681…  (一致)
# head_in    bf0a28677bc1c89b…  (一致)
```

三个用例都在 2026-11-15 实测命中，说明 exact 闭环、日志改造与 UV 导出都没有动几何。

## 5. 明确的坑

1. **别把这一版和旧 sha 混用**：默认参数已经变了，而且文件里多了 vt/vn。
2. **对比几何用 --no-uv --no-normals**：否则 vt 的行数变化会把 sha 判红。
3. **输入不在仓库**：artifacts/（140 MB）刻意不提交，锚点只记录输入的 sha256。
4. **默认参数 ≠ 锚点参数**：CLI 默认 --target-quads 20000 / --sharp-edge 60 / --adaptivity 0.5，上表用的是显式参数；写基线脚本时把参数抄全。
