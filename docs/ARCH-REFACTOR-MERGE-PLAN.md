# arch-refactor 分阶段合入调研与计划

> 快照时间：2026-11-13 17:5x（Asia/Shanghai）｜基线：`master` = `21ad0f9c`｜对照分支：`arch-refactor` = `0ba6c98d`
> 分支事实：`merge-base` = `4a49f82f`；master 在其后有 6 个提交，arch-refactor 只有 1 个（`0ba6c98d`，wip 快照）。
> 配套调研：[docs/merge/01-frontend-and-cli.md](merge/01-frontend-and-cli.md)（前端与 CLI，283 行）、[docs/merge/02-tools-and-gates.md](merge/02-tools-and-gates.md)（工具与门禁，601 行）。

---

## 0. 结论摘要（先看这六条）

1. **两条分支做的是同一次架构手术的两个不同解**：master 走「彻底删 Qt + 手写 C++ CLI + Go GUI」；arch 走「把 Qt 冻结到 `legacy/gui-qt/` + 自建 CLI + 自建 Go 前端 `ui-go/`」，另外**额外**做了 `src/AutoRemesher/* → src/core/<8 层>/` 的分层搬迁 + 生成式源清单 + 边界门 + 度量工具。真正值得合入的，只有 arch 的**目录分层 + 工具/门禁 + 计划文档**这三块。
2. **不要执行 `git merge arch-refactor`**。实测冲突 67 个路径，其中 58 个（87%）来自「master 删了 Qt，arch 把同一批文件改名到 legacy/gui-qt」的 rename/delete 冲突——直接合并会把这 58 个 Qt 文件重新"复活"回工作树，与 master 的路线正面冲突。剩 9 个是 `src/core/{double.h, positionkey.cpp, quadparameterizer.cpp, quadextractor.cpp, quadextractor.h, singularitysimplifier.cpp, autoremesher.cpp, autoremesher.h}` 的内容冲突 + `CMakeLists.txt` 的 add/add。
3. **方向定为「以 master 为基线，把 arch 的目录结构搬过来」，而不是「把 arch 的代码合过来」**。证据：忽略空白后，arch 侧 `src/core/*` 相对基线的真实改动极小（`quadextractor.cpp` 仅 +19/−1、`singularitysimplifier.cpp` +2/−2），而 master 侧同文件是 +2191/−352、+216/−58 的算法改动。arch 搬迁的本质是**改转发头 + 换目录**，代码语义未动，所以搬目录不会与 master 的算法改动打架。
4. **S1（核心目录搬迁）已完成并已验证**：在 `.agents/worktrees/merge-arch-core`（分支 `integrate/arch-core`）上 29 个文件 100% 相似度搬迁、构建通过、`check_boundaries` 0 违规、`gen_sources --check` 通过，且 `body_vox`/`body_clean` 两用例输出**逐字节相同**。
5. **arch 计划文档 §1.2 的"三份 sha 全等"验收门不可直接继承**：`head_in` 用例本身带非确定性（用 master 自己的二进制跑 8 次得到 3 个不同 sha，差异在 4 个顶点坐标的最后 1~2 位 ULP）。门要拆成「两例逐字节 + 一例容差/统计」。
6. **顺带查出 master 上 3 个既有缺陷**（与本次合并无关但挡路）：`gui/go.mod` 被根 `.gitignore` 的 `*.mod`（Fortran 规则）吞掉、`scripts/build_cli.bat` 调用的 `build/build_tbb.bat` 不在版本库、`release.yml` 仍在跑 `qmake`。第 1 条已在集成分支修好并验证。

---

## 1. 两条分支各自的资产表

| 资产 | master（21ad0f9c） | arch-refactor（0ba6c98d） | 合入结论 |
| --- | --- | --- | --- |
| Qt GUI | **删除**（78 个文件 + thirdparty/QtAwesome、QtWaitingSpinner） | **改名冻结**到 `legacy/gui-qt/`（58 个文件） | **不带**：master 的路线是彻底移除，冻结件只服务 arch 自己的可恢复性说明 |
| 核心源码布局 | 平铺 `src/AutoRemesher/*`（29 文件） | `src/core/{base,geom,numeric,field,cover,param,extract,pipeline}/` | **带（S1，已完成）** |
| 公共头 | `include/AutoRemesher/<名字>`（无扩展名转发头） | 同左，但转发到 `src/core/…`，并删掉死的 `QuadRemesher` | **带（S1）** |
| 构建 | `CMakeLists.txt` + `file(GLOB)` + `build.bat`/`scripts/build_cli.bat` + `bench/` | `CMakeLists.txt` + `cmake/core_sources.cmake`（生成）+ `tools/gen_sources.py` | **带生成式清单（S1）**，保留 master 的 TBB 与 bench |
| CLI | `src/cli/main.cpp`，目标 `autoremesher`，26 个选项，契约见 `docs/cli-protocol.md` | `src/cli/main_cli.cpp`，目标 `autoremesher_cli`，15 个选项 | **保留 master CLI，DROP arch 版**：arch 版缺 14 项选项、默认值仍是 Qt 那套、并调用 master 不存在的 `setKeepAllIslands` |
| Go 前端 | `gui/`（16 个测试函数、token 鉴权、进程树取消、SSE 契约、workspace 清理） | `ui-go/`（stdlib、7 文件、0 测试、无鉴权） | **保留 gui/，DROP ui-go**；只回灌 3 件小东西（见 §6） |
| 工具与门禁 | 无 | `tools/gen_sources.py`、`tools/check_boundaries.py`、`tools/check_plan_numbers.py` | **带（S1 已带两件，S2 带第三件）** |
| 度量工具 | 无 | `tools/metrics/`（`mesh_metrics.py` 1229 行、`emit_comparison.py` 1018 行、`test_metrics.py` 324 行、selfcheck 9 份） | **带（S2）**，注意 numpy 依赖 |
| 计划文档 | 5 份分析文档（DECOMPOSITION/GPU/OPTIMIZATION_LOG/RETOPOLOGY/bench README） | `docs/ARCHITECTURE-REFACTOR-PLAN.md`（1096 行）+ `docs/BUILD-CORE.md` + `GUI-LEGACY.md` + `docs/archive/v1` | **带前两份（改写后，S3）**，其余 DROP |
| 性能/算法工作 | `quadextractor` 增量邻接、`singularitysimplifier` 复用 scratch、`quadparameterizer` 重写、`bench/` | 无（arch 未含这些提交） | master 独占，是本次合并的**基线**，不可丢 |

---

## 2. 冲突面实测

```
$ git merge-tree --write-tree --name-only master arch-refactor
```

| 类别 | 路径数 | 说明 |
| --- | --- | --- |
| `legacy/gui-qt/**` rename/delete | 58 | master 删了这些路径，arch 把它们改名过去；git 判 rename/delete 冲突（其中 `autoremesher.pro`、`resources.qrc` 还是 modify/delete） |
| `src/core/**` content | 8 | `double.h`、`positionkey.cpp`、`quadparameterizer.cpp`、`quadextractor.cpp/.h`、`singularitysimplifier.cpp`、`autoremesher.cpp/.h` |
| `CMakeLists.txt` add/add | 1 | 两边都新增了内容完全不同的构建文件 |
| **合计** | **67** | |

**忽略空白后的真实改动量（同一批文件，两边对照）**——这张表就是"该搬目录还是该合代码"的判据：

| 文件 | master 相对基线 | arch 相对基线 |
| --- | --- | --- |
| `quadextractor.cpp` | +2191 / −352 | +19 / −1 |
| `singularitysimplifier.cpp` | +216 / −58 | +2 / −2 |
| `quadparameterizer.cpp` | +162 / −3 | +82 / −66 |
| `quadextractor.h` | +102 / −2 | +18 / 0 |
| `autoremesher.h` | +53 / 0 | +15 / 0 |
| `positionkey.h` | +36 / −4 | +14 / −4 |
| `positionkey.cpp` | +28 / −4 | +29 / −4 |
| `autoremesher.cpp` | +44 / 0 | +46 / −7 |
| `singularitysimplifier.h` | +25 / −1 | 0 |
| `parameterizer.h` | +7 / 0 | 0 |
| `parameterizer.cpp` | +1 / 0 | 0 |
| `quadparameterizer.h` | +4 / 0 | 0 |

结论：arch 侧是**结构改动**（转发头、目录、格式化），master 侧是**算法改动**。两者正交，因此「以 master 文件内容为准、只改它所在的目录」是唯一低风险路径。

---

## 3. 已完成的 S1：核心目录分层（`integrate/arch-core`）

**位置**：`E:\AI\autoremesher\.agents\worktrees\merge-arch-core`（`git worktree`，分支 `integrate/arch-core`，基于 `master 21ad0f9c`）。
**状态**：改动已 `git add`，**尚未提交**（按仓库约定不自动 commit）。

### 3.1 做了什么

| 动作 | 明细 |
| --- | --- |
| 搬迁 | 29 个文件 `git mv src/AutoRemesher/* → src/core/<层>/`，**相似度全部 100%**（内容逐字节未动） |
| 层归属 | base: double/positionkey/progress/radians/vector2/vector3；geom: meshseparator/surfacemesh；numeric: constrainedleastsquares/mixedintegerleastsquares；field: framefield/singularitysimplifier；cover: quadparameterizer；param: parameterizer；extract: quadextractor；pipeline: autoremesher/isotropicremesher |
| 转发头 | `include/AutoRemesher/*`（17 个）改为 `#include "../../src/core/<层>/<x>.h"`；删掉指向不存在文件的死头 `QuadRemesher` |
| 生成式清单 | 引入 `cmake/core_sources.cmake` + `tools/gen_sources.py`（禁止 GLOB，`--check` 可失败） |
| 构建接缝 | `CMakeLists.txt`：`file(GLOB AR_CORE_SOURCES …)` → `include(cmake/core_sources.cmake)` + `set(AR_CORE_SOURCES …)`；`bench/CMakeLists.txt` 同步改为读同一份清单并加 `../` 前缀 |
| 结构门 | 引入 `tools/check_boundaries.py`（层表 + 禁 Qt + 禁反向 include） |
| 附带修复 | 根 `.gitignore` 加 `!gui/go.mod`/`!gui/go.sum` 并把 `gui/go.mod` 纳入版本库（原被 Fortran `*.mod` 规则吞掉） |

**改动规模**：52 files changed, +233 / −41；其中 `R100` 29 个、`A` 3 个、`D` 1 个、`M` 19 个（17 转发头 + 两个 CMakeLists）。

### 3.2 验证结果（全部实跑）

| 门 | 命令 | 结果 |
| --- | --- | --- |
| 构建 | `cmake -S . -B build/cli -DCMAKE_BUILD_TYPE=Release -DAR_TBB_STATIC_LIB=<主仓>/build/tbb/tbb_static.lib` + `cmake --build build/cli --config Release --parallel` | **通过**，产出 `build/cli/dist/autoremesher.exe`（1,466,368 B） |
| bench 构建接缝 | `cmake -S bench -B build/bench` + build | **通过**（12 个 core 源文件正确进入编译），仅最后链接报 `LNK1181`——worktree 里没有 `build/tbb/tbb_static.lib`（未跟踪产物），属环境问题 |
| 源清单 | `python tools/gen_sources.py --check` | **通过**（12 sources, up to date） |
| 结构门 | `python tools/check_boundaries.py` | **通过**：`checked 29 files, 0 violation(s)` |
| Go GUI | `cd gui && go vet ./... && go test ./... && go build ./cmd/autoremesher-gui` | **通过**：`internal/job` 3.989s、`internal/server` 4.938s 全部 ok，产物 10,663,424 B（修 `go.mod` 之前该命令直接 `cannot find main module`） |

### 3.3 行为等价性（关键，含一个反例）

同一输入、同一组参数、两个二进制（master 的 `build/cli/dist/autoremesher.exe` vs 搬迁后的）逐字节比 sha256：

| 用例 | 参数 | 结果 |
| --- | --- | --- |
| `body_vox` | `--target-quads 15000 --sharp-edge 60 --adaptivity 0.3` | **IDENTICAL** `898e2881a6ac1a1ef07ee965d63ca526…`（master 连跑 5 次同一 sha） |
| `body_clean` | 同上 + `--anisotropy 1` | **IDENTICAL** `876092b1e07d76814683c9d688b18c8e…`（连跑 5 次同一 sha） |
| `head_in` | `--target-quads 10000 --sharp-edge 60 --adaptivity 0.5 --anisotropy 1` | **不稳定**：master 自己跑 8 次得到 3 个 sha（`bf0a2867…`×5、`138bfb39…`×2、`42c89ff6…`×1）；搬迁版跑 8 次得到同一族（`bf0a2867…`×7、`42c89ff6…`×1） |

**差异形态**：4 行顶点坐标的最后 1~2 位有效数字不同（如 `-0.14396631951559602` vs `-0.14396631951559596`），文件大小 455750 ↔ 455751 B，行数一致（10495 行）。**这是 TBB 并行归约顺序导致的既有非确定性，不是搬迁引入的**——判定依据是 master 的二进制单独跑就能复现两种值。
→ 影响：arch 计划里的「三份 sha 全等」门对 `head_in` **无效**，必须改写（见 §4）。这也顺带解释了为什么 arch 侧文档坚持"三份 sha 必须全等"却没发现这个问题：它只在 arch 的 core 上测过，而 arch 的 core 未含 master 后来的并行化改动。

---

## 4. 验收门设计（S1 之后每次合入都跑）

| 门 | 命令 | 通过判据 |
| --- | --- | --- |
| **G-A 构建** | `cmake -S . -B build/cli … && cmake --build build/cli --config Release --parallel`；`cmake -S bench -B build/bench && cmake --build build/bench`；`cd gui && go vet ./... && go test ./...` | 三处 rc=0；`build/cli/dist/autoremesher.exe` 生成 |
| **G-B 结构** | `python tools/gen_sources.py --check`；`python tools/check_boundaries.py` | 两个 rc=0、violation=0 |
| **G-C 行为（逐字节）** | `body_vox`/`body_clean` 跑一次比 sha256 | 各自等于本次基线记录的 sha（**这两个用例是确定性的**，实测 5/5 稳定） |
| **G-C′ 行为（容差）** | `head_in` 跑 k=5 次，取众数 sha；再与基线众数 sha 比 | 众数 sha 相等；若不等，用 `tools/metrics/mesh_metrics.py compare` 复核 quads/non-quads/components 与面比字段，全部在容差内 |
| **G-D 前端** | `gui/docs/e2e-smoke.sh`（需 Go + 已构建 CLI） | rc=0 |

> G-C 的基线 sha 必须在**每次合入后立即重建并记录**（`run.json` 里带参数、输入 sha、工具版本、取数时刻），不要照抄 arch 计划 §1.2 的 8101/9982/4720 那组数字——那是 arch 侧 core 的口径，master 已经改过算法。

---

## 5. 剩余阶段计划

### S2：工具、门禁与度量（预计 1.5 人日）

| 项 | 内容 | 验收 |
| --- | --- | --- |
| 2.1 | `tools/check_boundaries.py` 补规则：文档声称 R1~R8 但脚本只实现 R1/R2。需要补 R4（core 内禁 `std::cout/cerr/printf`，白名单 `base/Log.cpp`）、R6（`src/core/` 下每个目录必须在层表内，现在静默跳过）、R7（`geom/PositionKey.*` 禁 32 位 `long` 量化键）；把 R3 从"unexpected include"兜底改成显式判定 | 新增违规的负向测试样例能触发 rc=1 |
| 2.2 | 修 forwarder 链式解析（链式转发头现在会被误报 `unknown forwarder`） | 加一个两级转发头的临时用例不再误报 |
| 2.3 | R5 改判据：master 没有 `src/gui/**`（GUI 是 `gui/` 下的 Go），"禁止 GUI 反向依赖"只能改成构建图约束（core 目标不 link gui，或反向 grep `include/gui` 若将来出现） | 文档同步改写 |
| 2.4 | R8 重定阈值：`quadextractor.cpp` 基线 4553 行 → master 6392 行，1500 行告警从第一天就响（文档写的"4,263 行豁免"与实测也不符）。建议改为"文件行数 > 基线×1.2 或 > 3000 行"并在文档记录豁免名单 | 门在 master 树上 rc=0 |
| 2.5 | `tools/check_plan_numbers.py` + `docs/ARCHITECTURE-REFACTOR-PLAN.md` 同批引入（前者校验后者里的数字口径） | 脚本在改写后的文档上 rc=0 |
| 2.6 | `tools/metrics/` 整目录引入（`mesh_metrics.py`、`emit_comparison.py`、`test_metrics.py`、`selfcheck/*.json`、README）。**依赖 numpy**：系统 Python 3.14.8 无 numpy 会 `ModuleNotFoundError`；DSH 自带 Python 3.12 + numpy 2.3.5 下 `test_metrics.py` 60/60 PASS | `PYTHONUTF8=1 python tools/metrics/test_metrics.py` 全绿；README 写明 numpy 版本与 Windows 控制台 UTF-8 要求 |
| 2.7 | CI 接入：`.github/workflows/development.yml` 现在只跑 clang-format，三道门（G-A/G-B/G-C）都没有执行者。加 `gen_sources --check` + `check_boundaries` + `go vet/test` 作业 | PR 上能看到三个 job |

### S3：文档与前端修补（预计 1 人日）

| 项 | 内容 |
| --- | --- |
| 3.1 | 引入 `docs/ARCHITECTURE-REFACTOR-PLAN.md`（1096 行）并**改写头部与 §1.1**：目标名 `autoremesher_cli → autoremesher`、路径 `src/core` 与 master 对齐、删掉 `ui-go/`、`legacy/gui-qt` 两行；§1.2 锚点改成"待重建"（见 S4），不要在文档里保留会失效的具体数字（该文档自己的 C2/C6 口径也要求这一点） |
| 3.2 | `docs/BUILD-CORE.md` 与 master 的 `build.bat`/`scripts/build_cli.bat` 合并成一份构建说明（arch 版的 TBB/pwsh 段落仍有效，但源清单段落已过期，需按 `cmake/core_sources.cmake` 重写） |
| 3.3 | 全仓文档路径更新：`README.md:22`、`bench/README.md:3`、`src/cli/main.cpp:4` 注释、`OPTIMIZATION_LOG.md`、`RETOPOLOGY_ALGORITHMS.md`、`GPU_AND_QUALITY_ASSESSMENT.md`、`AUTOREMESHER_DECOMPOSITION.md` 里的 `src/AutoRemesher` 说法（共 8 处） |
| 3.4 | `gui/README.md` 两处修正：`:14` 的"可离线构建"（原本因 go.mod 缺失而假）、`:131` 的说明 |
| 3.5 | 回灌 ui-go 的 3 件小东西：①`!go.mod` 规则（S1 已落地）；②`-max-jobs` + `pruneLocked` 淘汰最旧已完成作业（gui 的 `m.jobs` 只写不删、每作业留 2000 条事件）；③`safeDir/withinRoots/isWithin` 作为工具函数备用。**不回灌**：`/api/fs` 路由、无鉴权、SSE 双方言、输出写输入目录 |
| 3.6 | 补 Go 门禁作业（gofmt/vet/test）；不支持 arch 的 `go 1.22`（master 的 `gui/go.mod` 是 `go 1.27`，二选一，建议保 1.27） |

### S4：重建回归锚点（预计 0.5 人日）

1. 用合入后的二进制对三个用例各跑 k=5 次（参数见 §3.3），记录每个 sha 的出现次数。
2. `body_vox`/`body_clean` 取唯一 sha 作为逐字节锚点；`head_in` 取众数 sha + `mesh_metrics measure` 的结构字段（quads/non-quads/components/largest/top-3 face ratio）作为容差锚点。
3. 产物落到 `artifacts/<case>/run.json` + `metrics.json`（**不提交** 140MB 的网格，只提交 `run.json`/`metrics.json` 与 `metrics_meta.json`）。
4. 把锚点写回 §3/§4 与本计划，并注明取数时刻（避免该文档 C6 那条"当前值没有失效机制"的老坑）。

### DROP 清单（明确不带，避免以后反复讨论）

| 资产 | 理由 |
| --- | --- |
| `legacy/gui-qt/`（58 文件） | master 已删除 Qt 并改走 Go GUI；保留冻结件会让仓库同时存在两套 GUI 遗产，且 CI/构建都不覆盖它 |
| `ui-go/`（7 文件） | 与 `gui/` 功能重叠且更弱（0 测试、无鉴权、只杀直接子进程、SSE 第二套帧契约）；只回灌 3 件小东西 |
| `src/cli/main_cli.cpp`（548 行） | 缺 14 项选项、默认值是 Qt 那套、调用 master 不存在的 `setKeepAllIslands`，整文件搬过来编译不过 |
| CLI 三选项 `--events-json` / `--report` / `--triangulate` | 分别与 master `--json` 重复、被 master README 明确移除过、修的是 arch 自己的错误默认值；**零未合入核心依赖，技术上可搬、判据上不值得** |
| `docs/GUI-LEGACY.md`、`legacy/README.md`、`docs/archive/ARCHITECTURE-REFACTOR-PLAN.v1-2026-11-10.md`（2357 行）、`ui-go/README.md` | 描述的是不带的资产 |

---

## 6. 提交切分建议（同意后我按这个切成 7 个提交）

| # | 提交信息 | 内容 | 门 |
| --- | --- | --- | --- |
| C0 | `fix(build): track gui/go.mod and stop the Fortran *.mod rule hiding it` | `.gitignore` + `gui/go.mod` | `cd gui && go build ./...` |
| C1 | `refactor(core): move the pipeline into src/core/<layer>, repoint the forwarders` | 29 个 R100 搬迁 + 17 个转发头 + 删死头 `QuadRemesher` | G-A + G-B |
| C2 | `build: generate the core source list instead of globbing` | `cmake/core_sources.cmake` + `tools/gen_sources.py` + 根/bench 两个 CMakeLists | `gen_sources --check` + 两个构建 |
| C3 | `tools: add the layering gate` | `tools/check_boundaries.py` + CI 作业 | `check_boundaries` rc=0 |
| C4 | `tools: import the metrics toolkit` | `tools/metrics/**` + README（numpy 要求） | `test_metrics.py` 全绿 |
| C5 | `docs: adopt the refactor plan and the layer/build docs` | 改写后的 `ARCHITECTURE-REFACTOR-PLAN.md`、`BUILD-CORE.md`、路径更新 | `check_plan_numbers.py` rc=0 |
| C6 | `gui: bound the job history and add Go gates` | `-max-jobs`/prune + gofmt/vet/test 作业 + README 修正 | `go test ./...` + e2e-smoke |

> C1/C2 可以合并成一个提交（搬迁后不加清单就构建不了）；但拆开能让 `git log` 保留"先分层、后换清单"的意图，评审也更容易。

---

## 7. 风险与已知坑

1. **CRLF**：仓库内 C++ 源码是 CRLF。搬运必须用 `git mv`（本次 29 个文件全是 `R100`，避免了整文件重写；注意 arch 侧同批文件在 `git diff` 里看起来是 +4571/−4553 的行数级改动，实际是行尾/格式噪声，别被它误导）。
2. **转发头的相对深度不一致**：master 的 `include/AutoRemesher/*` 里有的写 `../src/…`、有的写 `../../src/…`（历史遗留），机械替换后会得到 `../../../src/core/…`。本次已统一成 `../../src/core/…`，脚本用正则归一化；以后新加转发头请照抄这个形式。
3. **TBB 是环境产物**：`build/tbb/tbb_static.lib` 不在版本库（`build/` 被 ignore）。新 worktree/新 clone 必须显式 `-DAR_TBB_STATIC_LIB=<路径>` 或先跑 `build/build_tbb.bat`；而该脚本本身也不在库里（`bench/build_tbb.bat` 才是），这是 master 的既有断点，S3 一并修。
4. **`release.yml` 仍跑 qmake**（`:34`、`:264`、`:427`），而 master 已无 `.pro` 与 Qt——发布流水线目前是死的，与本合并无关但会干扰"合并后看起来 CI 挂了"的判断。
5. **`head_in` 的非确定性**：任何"必须逐字节一致"的断言都要限定到 `body_vox`/`body_clean`；重网格在 70 个连通分量的输入上会因并行归约顺序产生 ULP 级差异。
6. **`artifacts/` 140 MB** 的网格/结果不提交；只提交 `run.json`/`metrics.json`/`*_meta.json` 这类小件。
7. **不要为了"两边都保留"而合并 `ui-go` 与 `gui/`**：两套前端意味着两套进程契约与 SSE 帧格式，维护成本翻倍，且 master 的 `docs/cli-protocol.md` 已经把 `gui/` 钉在协议上。

---

## 8. 复现命令清单

```bash
# 冲突面
git merge-tree --write-tree --name-only master arch-refactor | tail -n +2 | sort -u

# 真实改动量（忽略空白，避免 CRLF/格式噪声）
git diff --shortstat -w 4a49f82f:src/AutoRemesher/quadextractor.cpp master:src/AutoRemesher/quadextractor.cpp

# 集成 worktree
git worktree add .agents/worktrees/merge-arch-core -b integrate/arch-core master
# 搬迁 + 转发头 + 清单（脚本：.agents/merge_stage1.sh）

# 门
python tools/gen_sources.py --check
python tools/check_boundaries.py
cmake -S . -B build/cli -DCMAKE_BUILD_TYPE=Release -DAR_TBB_STATIC_LIB=E:/AI/autoremesher/build/tbb/tbb_static.lib
cmake --build build/cli --config Release --parallel
cd gui && go vet ./... && go test ./...

# 行为等价（脚本：.agents/equiv_gate.sh、.agents/head_in_det.sh、.agents/body_det.sh）
```
