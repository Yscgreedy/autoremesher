
# AutoRemesher 架构改造计划（v1 归档，2026-11-10 清理前）

> **这是归档快照，不是当前计划。** 当前计划见 `docs/ARCHITECTURE-REFACTOR-PLAN.md`（只写未完成工作）。本文内部的 `§` 是旧编号：§11 与 §13~§18 的过程记录在现行版已删除或压成「附录 A」的一行索引，引用它们的代码注释已改指 `A.2-*` / `A.4`。

> 对象：`E:\AI\autoremesher` 分支 `arch-refactor`（工作树 `.agents/worktrees/arch-refactor`，基线 `4a49f82f`）
> 编写：arch-plan（task-3）｜日期：2026-11-10｜版本：v3（并入 M1/M2 已落地事实与 task-2 度量数据）
> 所有行号均由本次在**本工作树内**逐文件读取核对，行号是文件内绝对行号（含空行，从 1 起）。
> **计数口径**：文中"行数"= 非空行数（`Get-Content | Measure-Object -Line`）；括号内为含空行的全行数（`ReadAllLines`），与主检出根目录 `AUTOREMESHER_DECOMPOSITION.md` 的口径一致，两者不要混用。
> **数值口径**：`dev_p95(rel)` 在不同章节有 6 位小数（如 0.002192）与 4 位有效数字科学计数法（2.192e-3）两种写法，**它们是同一个数**（前者是后者的六位小数渲染）；精确值一律以 `artifacts/**/metrics.json` 的 `ref.deviation.p95_rel` 为准。同理 0.000818 与 8.181e-4 是同一测量，不是两次实验。
> 已实测的数字给出出处；未产出的明确标注「待产出」，不编造。

**导航**：§0 事实基线 ｜ §1 现状评估 ｜ §2 目标架构（目录/target 图/依赖规则/边界脚本/优化位置原则）｜ §3 契约层 ｜ §4 数据模型 CSR ｜ §5 里程碑 M0–M6 与验收门 ｜ §6 逐条改造清单（P0/P1/P2）｜ §7 并行化与可观测性 ｜ §8 兼容期与回滚 ｜ §9 工作量与顺序 ｜ §10 实测证据与待验证清单 ｜ §11 验证结果与 A/B 实测（含 COMPARISON.md 全量数据）｜ 附录 决策记录 D1~D5


---

## 0. 事实基线

### 0.1 已落地且有证据的事实

| # | 事实 | 实测值 | 证据路径 |
| --- | --- | --- | --- |
| F1 | 第一方代码规模 | 核心 9,875 非空行 / 30 文件；GUI 6,184 非空行 / 47 文件；CLI 380 非空行；内嵌 `tiny_obj_loader.h` 2,630 非空行 | 本次 `Measure-Object -Line` 实测 |
| F2 | Qt-free 构建 | 根 `CMakeLists.txt`（178 行）：`autoremesher_core`(STATIC) + `autoremesher_cli`(EXE)；`AUTOREMESHER_BUILD_GUI` 默认 OFF；GUI 目标未建（无 Qt） | `CMakeLists.txt:131、:155、:37`；`docs/BUILD-CORE.md` §2 |
| F3 | TBB 路径 | **路径 A**：vendored TBB 2017 单独 CMake 构建 `tbb.dll/tbb.lib` → IMPORTED 链接 + POST_BUILD 拷 DLL；路径 B（源码静态内嵌，含 `ASM_MASM`）已实现，`-DAUTOREMESHER_TBB_FROM_SOURCE=ON` 可切 | `CMakeLists.txt:59-116`；`docs/BUILD-CORE.md` §3 |
| F4 | 构建耗时（32 核，MSVC 19.51 / CMake 4.3.1 / Ninja 1.13.2） | TBB clean **2.8 s**；根工程 clean **10.1 s** | `docs/BUILD-CORE.md` §4 |
| F5 | CLI 产物 | `build/core/autoremesher_cli.exe` 958,976 B + `tbb.dll` 187,392 B（副本亦在 `artifacts/`） | 本次 `Get-ChildItem` 实测 |
| F6 | CLI 退出码 | 0 成功 / 1 参数错误 / 2 载入失败 / 3 重网格无结果。本次实测：无参 → **1**；不存在的输入 → **2**；cube 正常 → **0** | `src/cli/main_cli.cpp:56-59`；本次三次运行 |
| F7 | CLI 数值对齐 | body_vox.obj（`--target-quads 15000 --sharp-edge 60 --adaptivity 0.3`）：**8101** quads / 83 non-quads / 8244 verts / 7.1~8.2 s；历史 Qt 报告 `report_body_15k.txt` 为 8129 / 85 / 8272 / 7.502 s，差 **0.34%** | `docs/BUILD-CORE.md` §5 |
| F8 | 确定性 | 同一构建两次运行输出 OBJ 的 SHA256 完全一致（`9377D56A…C0427`） | `docs/BUILD-CORE.md` §5 |
| F9 | **M2 零回归** | after 版**不带任何开关**跑 body_vox，输出与基线二进制 **SHA256 完全相同** | M2 落地记录 |
| F10 | 度量工具已交付 | `tools/metrics/mesh_metrics.py`，自检 **60 条断言**全过；子命令 `measure` / `compare`；`compare` 退出码 **0 一致 / 1 有回归 / 2 参数或文件错误**，可直接做 CI 门禁；`--samples 50000` 使 40 万面 ref 的对比从 23.7 s 降到 7.1 s | `tools/metrics/mesh_metrics.py:1102-1145`；`tools/metrics/selfcheck/*.json` |
| F11 | 测试夹具 | `artifacts/inputs/head_scaled_1e-3.obj`（18.4 MB，坐标 ×1e-3）、`artifacts/inputs/head_scaled_1e5.obj`（27.8 MB，坐标 ×1e5）、`artifacts/inputs/multi_part.obj`（33.1 MB，约 80 万面，两部件） | 本次实测 |
| F12 | 单岛不并行（自报） | `Cores kept busy across the parallel phase: 1.00`，cube 与 body_vox 两次复现 | 本次 cube 运行；`docs/BUILD-CORE.md` §5 |
| F13 | **库内日志量** | 基线实测：head10k **3.56 MB**、multi12k **6.94 MB**、scaled10k **3.44 MB**、body15k **57 KB**（`quadextractor.cpp` 逐顶点/逐洞 `std::cerr`）。早前记录的 3.5/3.9 MB 属另一批次运行，**以 §11.1 的完整记录为准** | §11.1 |
| F14 | **after multi12k 输出质量** | 7,035 quads / quad ratio **91.57%** / **1,028** 条非流形边 / **186** 个边连通分量 / **45** 个退化面 | M2 运行记录 + `mesh_metrics` |
| F15 | **历史 Qt 产物质量（对照基准）** | 见 §0.3 表 | `tools/metrics/selfcheck/*.json`（task-2 实测） |
| F16 | **M2 行为开关已落地** | 默认全关＝历史行为；`--keep-all-islands`（对应 P0-1）、`--relative-tolerance`（对应 P0-2）、`--triangulate`（对应 §1.5-N4） | `src/cli/main_cli.cpp`、`quadextractor.h` 的 `setKeepAllIslands/setPositionTolerance`、`positionkey.h` 的 `setTolerance/quantize` |

### 0.2 已落地的源码变更（M2，作为 A/B 的最小可验证切片）

| 位置 | 变更 | 对应本文条目 |
| --- | --- | --- |
| `quadextractor.h` / `quadextractor.cpp` | 新增 `setKeepAllIslands(bool)`、`setPositionTolerance(double)`；`extract()` 改为 `return !m_remeshedPolygons.empty()`（诚实失败）；`removeIsolatedFaces()` 增加 keep-all 分支（原实现无条件只留最大 patch） | P0-1、P1-4 |
| `positionkey.h` / `positionkey.cpp` | `long` → `long long`（Windows 32 位 `long` 溢出）；`static long m_toIntFactor` → `thread_local double m_tolerance`（默认 1e-5，保持历史行为）；新增 `setTolerance/tolerance` 与饱和 `quantize()` | P0-2（部分） |
| `autoremesher.h` / `autoremesher.cpp` | 新增 `setKeepAllIslands` / `setRelativePositionTolerance`；岛内 tolerance = `voxelSize * 1e-3`（head 10k 用例下 ≈1e-5，故默认路径数值不变） | P0-1、P0-2 |
| `src/cli/main_cli.cpp` | `--keep-all-islands` / `--relative-tolerance` / `--triangulate`。`--triangulate` 用 `num_face_vertices` 做扇形三角化，修掉"n 边形按三元组硬切" | P0-1、P0-2、N4 |

**注意**：M2 给的是**机制 + 开关**，不是默认行为的改变。默认全关时 body_vox 输出 SHA256 与基线逐字节相同（F9）。所以 §6 里 P0-1/P0-2 的状态是"机制已落地，默认语义待 M4 切换"，不是"已完成"。

### 0.3 质量实测（这是"质量门"设计的依据）

同一套 `mesh_metrics` 对三个历史 Qt 产物与两个输入测得的数字：

| 网格 | quad 占比 | 非流形边 | 边连通分量 | 退化面 | 边界环 | 价4 占比 |
| --- | --- | --- | --- | --- | --- | --- |
| `head_quad_10k.obj` | 93.54% | 124 | **70**（主壳 4,899 面 + 69 个 2~15 面漂浮块） | 22 | 5 | 69.12% |
| `head_quad_20k.obj` | **97.10%** | **188** | 1 | 2 | — | 86.48% |
| `body_quad_15k.obj` | 98.97% | 12 | 1 | 0 | — | 94.96% |
| `head_in.obj`（输入） | — | — | **8,979** | — | **14,520** | — |
| `head_clean.obj`（清洗后输入） | — | — | 1 | — | 2 | — |

两条直接结论，写进所有验收门：

1. **只看 quad 占比会被骗。** head_quad_20k 的 quad 占比（97.10%）比 10k（93.54%）高，但非流形边从 124 涨到 188。二者不可互相替代，必须同时约束。
2. **输入侧的 8,979 个壳 / 14,520 个边界环**意味着"只保留最大 patch"（P0-1）在这类输入上是**灾难性**的：清洗前输入几乎必然被削成一个主壳。反过来，`head_clean.obj` 只有 1 壳 2 环，说明"清洗"这一步已经在上游做掉了类似的事——流水线里再做一次就是双重削减。

---

## 1. 现状评估

### 1.1 分层实况

行数口径见文首说明（非空行；括号内为全行数）。

| 层 | 实况文件 | 行数 | 对外依赖 | 被谁依赖 |
| --- | --- | --- | --- | --- |
| L4 GUI | `src/*.cpp|*.h`（47 文件） | 6,184 | Qt Widgets + OpenGL, core | 无（顶层） |
| L3 线程壳 | `quadmeshgenerator` 113(152)、`previewmeshgenerator` 391、`rendermeshgenerator` 207 | 863 | Qt, core | L4 |
| L2 门面/编排 | `autoremesher.cpp` 961(**1,059**)、`autoremesher.h` 230、`isotropicremesher` 78+83 | 1,352 | STL, TBB, Eigen, core | L3/L4、CLI |
| L1 算法内核 | `quadextractor` 4,263(**4,553**)、`quadparameterizer` 684(**716**)、`mixedintegerleastsquares` 500(**541**)、`singularitysimplifier` 451(**468**)、`parameterizer` 409(**436**)、`constrainedleastsquares` 384(**416**)、`framefield` 210(**225**)、`surfacemesh` 148(**159**)、`meshseparator` 140(**154**) | 7,189 | STL, TBB, Eigen | L2 |
| L0 基础类型 | `vector3.h` 273、`vector2.h` 179、`positionkey` 60+39、`double.h` 36、`radians.h` 34、`progress.h` 36 | 657 | 无 | 全部 |

L1+L2+L0 = 8,541 行是封闭集，根 `CMakeLists.txt:122-149` 已把这一整块打成 `autoremesher_core`（M1 已验证可编译可跑）。**问题不在"能否成库"，只在"库内部没有模块边界"**。

### 1.2 真实依赖图（由 include 实测）

~~~text
vector2 / vector3 ─→ double
surfacemesh ─→ vector2, vector3
constrainedleastsquares ─→ (Eigen)
mixedintegerleastsquares ─→ constrainedleastsquares
framefield ─→ constrainedleastsquares, surfacemesh, vector3
singularitysimplifier ─→ surfacemesh
quadparameterizer ─→ constrainedleastsquares, mixedintegerleastsquares, surfacemesh
parameterizer ─→ constrainedleastsquares, framefield, quadparameterizer, singularitysimplifier, surfacemesh
quadextractor ─→ double, meshseparator, positionkey
autoremesher ─→ isotropicremesher, meshseparator, parameterizer, quadextractor
~~~

已是有向无环，但三条越层边决定目录不能照抄"按层切"：

- **`framefield（场）→ constrainedleastsquares（数值求解器）`**：`framefield.cpp:24` 直接 include CLS。所以 CLS/MILS 不能放 `cover/`（否则 `field→cover` 成环），必须独立为最底层 `numeric/`。
- **`parameterizer（编排）→ quadparameterizer（cover）`**：目录里 `cover/` 必须排在 `param/` 之前。
- **`quadextractor → meshseparator` 且 `autoremesher → meshseparator`**：岛拆分是共享拓扑操作，必须下沉 `geom/`，否则 `extract` 被 pipeline 反向依赖。

### 1.3 耦合点清单

| # | 耦合 | 位置 | 症状 |
| --- | --- | --- | --- |
| C1 | 库 → stderr | `autoremesher.cpp:1046-1047`；`quadextractor.cpp` 41 处、`autoremesher.cpp` 11 处、`parameterizer.cpp` 5 处、`singularitysimplifier.cpp` 1 处（合计 58 处）；`searchBoundaries` 逐顶点打印（`quadextractor.cpp:1415、1422`） | **单次运行 3.5~3.9 MB stderr**（F13）；库无法被嵌入 |
| C2 | 结果靠裸指针存活 | `mainwindow.h:132-133`（`std::vector*` 结果）、`:175-179`（5 个 `ModelShaderMesh*` 手工 delete/new） | 所有权靠人眼；"有没有源网格"用 `nullptr` 判断（`mainwindow.cpp:932`） |
| C3 | 失败语义 | `remesh()` 返回 bool；`extract()` 曾恒 true（`quadextractor.cpp:288`，**M2 已修**）；岛内异常 catch 后静默跳过（`autoremesher.cpp:818-831`） | 无法区分失败原因；岛级部分失败无通道 |
| C4 | 进度两套风格 | `std::function`（`progress.h:36`）与 C 指针 + void* tag（`autoremesher.h:45、66-74`） | 门面 C 风格、内核 std::function，中间靠 `makeStageProgress`（`autoremesher.cpp:516-532`）翻译 |
| C5 | 中间产物外泄 | `autoremesher.h:101-159` 共 15 个 getter，全为 GUI 预览服务 | 公开面绑定到某个 UI 的具体需求 |
| C6 | `vector<vector<size_t>>` 贯穿全栈 | `mainwindow.h:131` → `AutoRemesher` 8 个成员 → `quadextractor.h:99` | 每面一次堆分配 + 指针追逐；无法按 flat index 分块并行 |
| C7 | 无头模式寄生 GUI | `main.cpp:83` → `:220-226`（MainWindow + `app.exec()`） | 无 Qt 不能跑；退出码恒 0（CLI 已绕开，Qt 路径未修） |
| C8 | 失败路径不发信号 | `mainwindow.cpp:1058-1062`、`:1065-1068`（**挂死**）、`:1199-1203` | 三种失败对上层完全同形 |

### 1.4 债务分级与状态

| 级别 | 含义 | 条数 | 状态（截至 M2） |
| --- | --- | --- | --- |
| P0 | 静默产出错误结果 | 4 | P0-1、P0-2 "机制已落地、默认语义未切"；P0-4 的 CLI 侧已由 M1 解决、Qt 路径未修；P0-3 未动（GUI 不可构建） |
| P1 | 算法质量/稳定性 | 8 | 仅 P1-4（`extract()` 恒 true）M2 已修；其余 7 条未动 |
| P2 | 性能/可维护性/卫生 | 10 | 全未动；F13 的 3.5 MB stderr 最紧迫 |

### 1.5 对 `AUTOREMESHER_DECOMPOSITION.md` 的独立核验

主检出根目录那份拆解报告（209 行）作为输入参考，本次逐条回源码核对：

**10 条 P0/P1 全部确认**（行号与描述吻合）：`quadextractor.cpp:1439-1449`、`positionkey.cpp:26,39-41`、`mainwindow.cpp:932`、`mixedintegerleastsquares.cpp:456-469`、`constrainedleastsquares.cpp:159,214-217`、`framefield.cpp:101-110`、`quadparameterizer.cpp:680-682`、`surfacemesh.cpp:51-52`、`quadextractor.cpp:288`、`quadextractor.cpp:1300-1303`。

**口径说明**：原文的行数是含空行全行数（`quadextractor.cpp` 4,553 等），本文 §1.1 用的是非空行数（4,263），差值即空行。两者都对，只是口径不同；本文所有行号引用不受影响。

**需修正 1 处**：

| 原文 | 问题 | 修正 |
| --- | --- | --- |
| §6 "OBJ 6 位有效数字" | 描述对，影响被低估 | 对 `head_clean.obj`（包围盒量级 10²）是 1e-4 级量化，**比 PositionKey 的 1e-5 粗一个数量级**；CLI 用 `formatNumber`（`main_cli.cpp:78-83`）刻意复刻。新增 P2-10。 |

**新增 4 条（原文未列，N4 已被 M2 修掉）**：

- **N1**：`searchBoundaries` 逐顶点写 stderr（`quadextractor.cpp:1415、1422`）。原文只说"可能刷屏"，未指出是**性能**问题：`fixHoles()` 在 `extract()` 内被调用两次（`:164、:204`）。F13 的 3.5 MB 就是它。
- **N2**：`mainwindow.cpp:1065-1068` 的 headless 死路——`m_quadMeshGenerator` 非空时直接 return 且不发信号，无头模式永久挂起。
- **N3**：`constrainedleastsquares.cpp:335-339` 走拉格朗日回退后**不清 `m_matrixDirty`** → 每轮重做 SparseQR + SparseLU；`framefield.cpp:202-214` 的 5 轮迭代付 5 次。
- **N4**（M2 已修）：OBJ 载入把任意 n 边形按"每 3 个索引一组"硬切（`main_cli.cpp:236-244` 的 `i += 3`；Qt 侧 `MainWindow::loadObj` 同病），四边形输入被切成错误的两个三角形。M2 的 `--triangulate` 按 `num_face_vertices` 做扇形修正，**默认仍关**以保字节一致。

---

## 2. 目标架构

### 2.1 目录布局

五层目录 + 三个补齐目录；另有两个**非 core** 目录（`legacy/` 与 `ui-go/`，见 §12）。补齐项都有硬理由，不是偏好：

~~~text
src/
  core/
    base/       Progress.h  Result.h  Log.h  Span.h  Csr.h      ← 契约（§3），最低层
    geom/       Vector2  Vector3  Double  Radians
                PositionKey  MeshSeparator                      ← 通用几何/拓扑
    numeric/    ConstrainedLeastSquares  MixedIntegerLeastSquares
                                                                ← 被 field 依赖，必须低于 field
    field/      SurfaceMesh  FrameField  SingularitySimplifier
    cover/      QuadParameterizer
    param/      Parameterizer
    extract/    QuadExtractor
    pipeline/   AutoRemesher  IsotropicRemesher                 ← 门面，唯一对外入口
  cli/          main_cli.cpp                                    ← 唯一稳定接口；--events-json 出口
legacy/
  gui-qt/       mainwindow  modelshader*  theme  previewmeshgenerator …
                resources.qrc  resources/  shaders/  autoremesher.pro
                thirdparty/{QtAwesome, QtWaitingSpinner}       ← 冻结快照，不进默认 target/CI/发布（§12.2）
ui-go/          Go 前端（可选）：进程边界 + NDJSON 事件（§12.3），不链接 core
include/AutoRemesher/   18 个转发头，仅修相对路径（P2-6）并改指新位置
~~~

三个补齐目录的理由：

- **`base/`**：Result/Progress/Log/Span/Csr 无处安放；塞进 `geom/` 会让 geom 反向依赖契约。
- **`numeric/`**：`framefield.cpp:24` 直接 include `ConstrainedLeastSquares`。若 CLS/MILS 放 `cover/`，立刻得到 `field→cover` 反向边。这是 include 实测的直接结论。
- **`pipeline/`**：`AutoRemesher` 依赖 param 与 extract 全部；放在 core 根目录会让"目录即层级"的检查脚本失效。

**不保留源码双份**。迁移用 `git mv` + 逐文件改 include，M2 一次搬完。双份源码意味着同一个 bug 修两次，是并行开发期的定时炸弹。

### 2.2 CMake target 图

~~~text
                    ┌──────────────────────┐
                    │ thirdparty (vendored)│
                    │ eigen tbb meshopt    │
                    │ isotropicremesher    │
                    └──────────┬───────────┘
                               │ INTERFACE
                 ┌─────────────┴──────────────┐
                 │                            │
    ┌────────────▼───────────┐    ┌───────────▼────────┐
    │ autoremesher_tbb       │◄───┤ autoremesher_core  │
    │ (IMPORTED 或 STATIC)   │    │ STATIC             │
    └────────────────────────┘    │ core/base..pipeline│
                                  └───────────┬────────┘
                                              │ PUBLIC: include/ eigen tbb
                    ┌─────────────────────────┼──────────────────────┐
    ┌───────────────▼────────┐   ┌────────────▼──────────┐  ┌────────▼──────────┐
    │ autoremesher_cli       │   │ autoremesher_core_    │  │ autoremesher_     │
    │ EXE (Qt-free)          │   │ objlib (W4/WX 门禁)   │  │ tbb_static(路径B) │
    │ src/cli/*.cpp          │   └───────────────────────┘  └───────────────────┘
    │ --events-json          │
    └───────────┬────────────┘
                │ 进程边界 + NDJSON（非链接，§12.3）
    ┌───────────▼────────────┐   ┌──────────────────────────────────────┐
    │ ui-go (可选 EXE)       │   │ legacy/gui-qt（冻结，不 configure）  │
    │ 本地 Web UI + SSE      │   │ Qt5/6 GUI + qrc + shaders + .pro     │
    └────────────────────────┘   └──────────────────────────────────────┘

    进阶（非链接，本轮不实施，§12.4）：
    autoremesher_core ⇢ autoremesher_capi（纯 C 头）⇢ cgo ⇢ ui-go
~~~

唯一允许的连边：

| 从 | 到 | 可见性 | 说明 |
| --- | --- | --- | --- |
| `autoremesher_core` | `autoremesher_tbb` | PUBLIC | TBB 是 core 的公开依赖 |
| `autoremesher_core` | Eigen | PRIVATE（M3 起） | 头文件里不出现 `Eigen::`，Eigen 只在 .cpp 内（`framefield.cpp`、`constrainedleastsquares.cpp`） |
| `autoremesher_cli` | `autoremesher_core` | PRIVATE | CLI 不导出符号 |
| `ui-go` | —（不链接任何 C++ target） | —（进程边界） | 只依赖 `autoremesher_cli` 的 NDJSON 契约（§12.3） |
| `legacy/gui-qt` | 不接入构建 | — | 冻结；`AUTOREMESHER_BUILD_GUI=OFF` 时不 configure（§12.2） |
| `autoremesher_core_objlib` | — | — | OBJECT 库，仅用于给 core 跑一次 `/W4 /WX` 门禁 |

`autoremesher_gui` 本次不构建（无 Qt，`docs/BUILD-CORE.md` §6.1）。当前 `CMakeLists.txt:176-178` 的 `AUTOREMESHER_BUILD_GUI` 分支只发 warning——M6 前必须改成真构建，否则"并存"是空话。

**两个必须改掉的 CMake 细节**：

1. `CMakeLists.txt:122` 的 `file(GLOB ...)` 没有 `CONFIGURE_DEPENDS`：新增 .cpp 不重跑 cmake 就静默不编译。目录重构本来就要动这行，直接改成显式源列表。
2. `CMakeLists.txt:146` 的 `/GL-` / `/fp:fast` 是快迭代取舍。M4 起加 `AUTOREMESHER_FP_PRECISE` 开关，让"数值对拍"与"性能对比"分别选定。

### 2.3 依赖规则（可机器判定）

| 层 | 模块 | 允许的 AutoRemesher 头 | 允许的外部头 |
| --- | --- | --- | --- |
| 0 | base | 无 | 标准库 |
| 1 | geom | base | 标准库 |
| 2 | numeric | base, geom | 标准库, `Eigen/*` |
| 3 | field | base, geom, numeric | 标准库, `Eigen/*`, `tbb/*` |
| 4 | cover | base..field | 标准库, `Eigen/*` |
| 5 | param | base..cover | 标准库, `Eigen/*`, `tbb/*` |
| 6 | extract | base, geom, field | 标准库, `tbb/*` |
| 7 | pipeline | 全部 | 标准库, `tbb/*`, `meshoptimizer.h` |

（`extract` 不得 include `param` / `cover`：抽取器只吃 UV 与三角网格，不吃参数化器。写成脚本后能阻止未来"顺手把 Parameterizer 拉进抽取器"的滑坡。）

规则编号即脚本 rule id：

- **R1 禁止 Qt**：`src/core/**` 不得出现 `#include <Q...>` / `<Qt...>`。
- **R2 禁止反向 include**：M include N 的头要求 `rank(N) <= rank(M)`。
- **R3 禁止相对路径 include**：只允许 `<AutoRemesher/Xxx>` 或 `<core/模块/Xxx.h>`。相对路径会绕过 R2 的模块名解析。
- **R4 禁止库内输出**：`src/core/**` 不得出现 `std::cerr` / `std::cout` / `printf` / `fprintf` / `fputs`（白名单 `base/Log.cpp` 自身）。
- **R5 禁止 GUI 反向依赖**：`src/gui/**` 不得被 core / cli include。
- **R6 禁止未归属目录**：`src/core/` 下每个目录必须在层表内。
- **R7 禁止 32 位 long 量化键**：`geom/PositionKey.*` 不得出现 `long `（M2 已改 `long long`，此规则防回归）。
- **R8 文件体积告警**：单文件 > 1,500 行告警（`quadextractor.cpp` 4,263 行为已知豁免，见 M5）。

### 2.4 自动化边界检查（已实现，见 G15）

**状态：已落地并通过实测**（§17.4）。`tools/check_boundaries.py` — 按 §2.3 的层序判定三条：向上 include、Qt 头、非白名单 include。

| 实测项 | 结果 |
| --- | --- |
| 正例 | repo 根执行 `python tools/check_boundaries.py`（**不带参数**）→ `checked 29 files, 0 violation(s)`，**exit 0** |
| 负例 | 在 `base/double.h` 注入 `#include <AutoRemesher/QuadExtractor>` → 报 `base includes upward into extract`，**1 违规 / exit 1**；还原后回 exit 0 |
| 调用形式 | 接受**任意层级路径**（缺省当前目录）：`resolve_root()` 依次尝试自身/父/祖父找到含 `src/core` 的层；`repo 根`、`src`、`src/core` 三种调用均 exit 0。找不到 `src/core` 才 exit 2（结构问题，非参数问题） |

接成 CMake 自定义目标 + CI 门禁（**G15**，退出码直接使用）。下面是实现骨架，与落盘版本同构：

~~~python
# tools/check_boundaries.py —— 骨架
import re, sys, pathlib

RANK = {"base":0,"geom":1,"numeric":2,"field":3,"cover":4,"param":5,"extract":6,"pipeline":7}
HEADER = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')

MODULE_OF = {
    "Progress":("base",0), "Vector2":("geom",1), "Vector3":("geom",1),
    "Double":("geom",1), "Radians":("geom",1), "PositionKey":("geom",1),
    "MeshSeparator":("geom",1), "SurfaceMesh":("field",3),
    "FrameField":("field",3), "SingularitySimplifier":("field",3),
    "ConstrainedLeastSquares":("numeric",2), "MixedIntegerLeastSquares":("numeric",2),
    "QuadParameterizer":("cover",4), "Parameterizer":("param",5),
    "QuadExtractor":("extract",6), "AutoRemesher":("pipeline",7),
    "IsotropicRemesher":("pipeline",7),
}
ERRORS = []

def check(root):
    for path in sorted(root.glob("**/*")):
        if path.suffix not in (".cpp", ".h") or not path.is_file():
            continue
        rel = path.relative_to(root)
        owner = rel.parts[0]
        for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            m = HEADER.match(line)
            if not m:
                continue
            inc = m.group(1)
            if inc.startswith(("Q", "Qt")):
                ERRORS.append("R1 %s:%d core 引入 Qt 头 %s" % (rel, lineno, inc))
            if inc.startswith(".."):
                ERRORS.append("R3 %s:%d 相对路径 include %s" % (rel, lineno, inc))
                continue
            name = inc.split("/")[-1].split(".")[0]
            if name in MODULE_OF:
                target, rank = MODULE_OF[name]
                if rank > RANK[owner]:
                    ERRORS.append("R2 %s:%d %s(L%d) -> %s(L%d) 反向依赖"
                                  % (rel, lineno, owner, RANK[owner], target, rank))
            if re.search(r'std::(cerr|cout)|\bprintf\b|\bfprintf\b|\bfputs\b', line):
                ERRORS.append("R4 %s:%d 库内直接输出" % (rel, lineno))
    for path in root.iterdir():
        if path.is_dir() and path.name not in RANK:
            ERRORS.append("R6 未归属目录 src/core/%s" % path.name)

if __name__ == "__main__":
    check(pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "src/core"))
    for e in ERRORS:
        print(e)
    sys.exit(1 if ERRORS else 0)
~~~

CMake 接线：

~~~cmake
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_custom_target(autoremesher_boundaries
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_boundaries.py
            ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    COMMENT "Checking core module boundaries (R1-R8)")
add_dependencies(autoremesher_core autoremesher_boundaries)
~~~

**验收门**：M3 结束时脚本对 `src/core` 必须 0 错误；并且**故意植入**三条违规（core 里 include `<QString>`、`field` include `cover/QuadParameterizer`、`geom` 里 `std::cerr`）确认分别报 R1 / R2 / R4 并返回 1。检查脚本本身不验证等于没检查。

---

### 2.5 改动准则：优化位置原则（D17）

> **改进必须落在「自由度所在的那个系统」里。**
>
> - 凡是**「给下游求解器喂更好的初值」**的改动 —— 都会被下游的**重新优化**吃掉。实例：M4 取整（§13）、M4-1b 取整调度（§14）、M4-5b 折痕权重（§16），**三次全部如此**。
> - 能被保留的只有**「把新的自由度/约束加进那个重新求解的系统」** —— 实例：M4-1c 的每面 log 尺度（进 `solveQuadCover`）、M4-5c 的折痕约束（进 `applyCurlCorrection` 的 3F 系统）。

**评审任何优化项时先问一句：「它改的是初值，还是自由度？」**

| 改的是 | 结果 | 判据 |
| --- | --- | --- |
| **初值 / 调度 / 权重**（喂给下游求解器的输入） | 被下游重新优化吃掉；指标随操作点漂移（§14.4 已实证） | 出现在「改参数就能让指标好看，但换个操作点就反转」的形态 |
| **自由度 / 约束**（进联合优化的系统） | 保留；代价是求解规模上升（未知数 +3F 或 +每面尺度） | 出现在「指标改善稳定、但时间与规模同步上涨」的形态 |

**为什么把这条写成原则而不是教训**：三个负结果本身不产生行动，它们只产生「别再试」；但这条原则产生**可执行的判断**——它把「再试一次」和「换作用位置」区分开了，也让 M4-1c / M4-5c 的价值可以被独立评估（它们不是第四次重试，是第一次换位置）。

**反例提醒**：`--field-brush` 与 `band/batch` 这类旋钮**属于「初值/调度」侧**，所以它们的收益必然是操作点相关的（V9 与 §14.3 都印证）。任何人再用它们做「调参改善质量」的论证，先看这条。


## 3. 契约层

### 3.1 错误模型：Result / StatusCode 取代 bool + cerr

现状：`remesh()` 返回 bool，原因写 stderr（`autoremesher.cpp:560、583`）；`parameterize()` 失败打 `"Quad cover solve failed"`（`parameterizer.cpp:411`）。上层唯一能感知的是"结果为空"。

目标 `src/core/base/Result.h`：

~~~cpp
namespace AutoRemesher {

enum class StatusCode : int {
    Ok = 0,
    EmptyInput,        // 无顶点或无三角形
    InvalidParameter,  // targetTriangleCount == 0 等
    TopologyInvalid,   // SurfaceMesh 构造后 faceCount 不符
    SolverFailed,      // CLS/MILS 无法分解或不收敛
    FrameFieldFailed,
    CoverFailed,
    ExtractionEmpty,   // extract 未产出任何多边形
    IslandsDropped,    // 有岛被丢弃
    IoError,
    Cancelled,
};

struct Status {
    StatusCode code = StatusCode::Ok;
    std::string detail;        // 给人看的一句话，不做路径拼接
    size_t failedIslands = 0;  // 部分失败时使用
    size_t totalIslands = 0;
    explicit operator bool() const { return code == StatusCode::Ok; }
};

template <typename T>
struct Result {
    Status status;
    T value {};
    explicit operator bool() const { return static_cast<bool>(status); }
};

inline Status ok() { return {}; }
inline Status fail(StatusCode code, std::string detail)
{
    return Status { code, std::move(detail), 0, 0 };
}
}
~~~

迁移规则（硬约束）：

| 旧形态 | 新形态 | 备注 |
| --- | --- | --- |
| `bool remesh()` | `Result<RemeshOutput> remesh()` | 保留 `bool` 兼容包装到 M6，内部转调新接口 |
| `bool extract()`（M2 已变 `!empty()`） | `Status extract()`，空多边形 → `ExtractionEmpty` | P1-4 收尾 |
| 岛内 catch 后 stderr + 跳过 | `failedIslands++`，返回 `Status{Ok, detail, failed, total}`，detail **非空** | 部分成功必须可上报，不能算全成功 |
| 15 个裸 getter | `struct RemeshOutput` 一次性返回 | GUI 按需取字段 |

**退出码映射**（在现有 `main_cli.cpp:56-59` 的 0/1/2/3 基础上扩展）：

| StatusCode | exit | 依据 |
| --- | --- | --- |
| Ok | 0 | |
| EmptyInput / InvalidParameter | 1 | 与 usage 错误同码：都是调用方输入问题 |
| IoError | 2 | 载入失败 |
| SolverFailed / FrameFieldFailed / CoverFailed / ExtractionEmpty | 3 | 与现状一致（重网格无结果） |
| Ok 但 `failedIslands > 0` | 0，但 stderr 一行汇总 + report 增一行 | 部分成功不该让 CI 红，但必须显式可见 |

### 3.2 进度：ProgressSink 接口 + C 回调适配

~~~cpp
// src/core/base/Progress.h
namespace AutoRemesher {

struct ProgressEvent {
    float overall = 0.0f;   // 全流水线 0..1
    float stage = 0.0f;     // 当前阶段内 0..1
    const char* stageName = nullptr;  // 生命周期由调用栈保证，不跨线程持有
    size_t islandIndex = 0;
    size_t islandCount = 1;
};

class ProgressSink {
public:
    virtual ~ProgressSink() = default;
    // 必须能从 TBB 工作线程调用，实现自行加锁
    virtual void onProgress(const ProgressEvent& event) = 0;
};

// C 回调适配，GUI 与旧调用方用
class CallbackProgressSink final : public ProgressSink {
public:
    using Handler = void (*)(void* tag, const ProgressEvent& event);
    CallbackProgressSink(Handler handler, void* tag) : m_handler(handler), m_tag(tag) {}
    void onProgress(const ProgressEvent& event) override
    {
        if (m_handler) m_handler(m_tag, event);
    }
private:
    Handler m_handler;
    void* m_tag;
};
}
~~~

关键点：

- **`stageName` 保持 `const char*`**：现有实现全部传字面量（`quadextractor.cpp:58、64、117…`）。跨线程构造 `std::string` 就是每步一次堆分配 + 一次跨线程析构。代价是契约要写"不得传临时串"。
- **permille 去抖与"最慢岛"选取（`autoremesher.cpp:496-513`）留在 pipeline 层**，不下移到 sink——那是策略不是契约。
- 现有 `makeStageProgress`（`autoremesher.cpp:516-532`）本就是有状态机，改造成 `StageProgress : ProgressSink` 时保留其 lastTime/lastName 计时职责。
- `setProgressHandler(AutoRemesherProgressHandler)`（`autoremesher.h:66-69`）M3 变薄封装，M6 删除。

### 3.3 所有权：unique_ptr 移交 + 只读 span 输入

现状所有权是"看注释"：`Parameterizer` / `QuadExtractor` 构造接 `const vector*`（`parameterizer.h:101-103`、`quadextractor.h:37-44`），生命周期靠调用方；`takeTriangleUvs()` 移交 `unique_ptr`（`parameterizer.h:43-46`），而 `originalTriangleUvs()` 返回同内容的拷贝引用（`:48-51`）——**同一对象两种所有权语义并存**。

| 位置 | 规则 |
| --- | --- |
| 输入网格 | `MeshView { Span<const Vector3> positions; CsrView<const uint32_t> faces; }`，纯借用，调用期间必须有效 |
| 中间产物 | 一律 `std::unique_ptr<T>` 移交，移交后成员置空，再取即返回错误 |
| 结果 | `RemeshOutput` 按值 move 返回，不再有 15 个 getter |
| GUI 显示副本 | GUI 自己 `make_unique<ModelShaderMesh>(...)`，库不参与 |

**C++14 的现实处理**：`CMakeLists.txt:27` 是 `CMAKE_CXX_STANDARD 14`，没有 `std::span`。自建最小 `Span<T>`（约 40 行：data/size/begin/end/operator[]）放 `base/Span.h`，**不升 C++17**——升标准会牵动 Qt 6 与 TBB 2017 的既有 CI 组合。**已裁决为 D5**：M3 期间不升，保留自研 Span；若 M3 后要升，三条前置缺一不升（见附录决策记录 D5）。

### 3.4 日志：可注入，库内零输出

~~~cpp
// src/core/base/Log.h
namespace AutoRemesher {
enum class LogLevel { Debug, Info, Warn, Error };
class Logger {
public:
    virtual ~Logger() = default;
    virtual void log(LogLevel level, const char* message) = 0;
};
Logger* setLogger(Logger* logger);   // 返回旧指针，便于作用域还原
Logger* logger();
void logMessage(LogLevel level, const char* format, ...);  // 无 logger 时静默
}
~~~

落地清单（本次实测的 `std::cerr` 分布，改造后必须全为 0）：

| 文件 | 现有处数 | 处置 |
| --- | --- | --- |
| `src/AutoRemesher/quadextractor.cpp` | 41 | 调试类降 `LogLevel::Debug` 且默认不输出；`searchBoundaries` 逐顶点打印**删除**（P2-3），只留一条汇总 |
| `src/AutoRemesher/autoremesher.cpp` | 11 | `phaseReport` 打印（`:1046-1047`）移到前端；失败信息转 `Status.detail` |
| `src/AutoRemesher/parameterizer.cpp` | 5 | 失败转 `StatusCode`；统计转结构化事件（§7.2） |
| `src/AutoRemesher/singularitysimplifier.cpp` | 1 | 同上 |
| `src/cli/main_cli.cpp` | 16 | **保留**：终端程序，输出是职责 |
| `src/mainwindow.cpp` | 2 | 保留（GUI 层） |

**`phaseReport` 的去向**：现在是 `std::vector<std::string>`（`autoremesher.h:181-184`），已格式化，程序无法消费。M3 改为 `std::vector<PhaseEvent>`（name/order/microseconds/islandIndex），CLI 负责渲染成现行文本（保证 report 格式零变化），测试负责断言（§7.2）。

**~3.9 MB 的实测目标**：F13 的 head10k 3.5 MB / multi12k 3.9 MB 是 M3 的可量化收益——改造后同一输入的 stderr 应 ≤ 200 行（进度 + phase report + 汇总）。

---

## 4. 数据模型迁移：`vector<vector<size_t>>` → CSR

### 4.1 现状字节账（可复算）

~~~text
vector<vector<size_t>> faces(F)
  外层          : F * 24 B        （三个指针）
  每面堆块      : 3*8 = 24 B payload
                  分配器取整 32 B + 头部 8~16 B → 实测按 44 B 计
  合计          : F * 68 B
~~~

按真实产物计（§0.2 的实测计数）：

| 网格 | V | F | 当前 AoS | CSR(uint32) | 节省 |
| --- | --- | --- | --- | --- | --- |
| `head_quad_20k.obj`（以四边为主） | 14,788 | 14,730 | 14,730 × 92 B = **1.35 MB** | 59 KB + 236 KB = **295 KB** | **4.6×** |
| 等距三角网格（`targetQuads*2` = 40,000 面，`main.cpp:1078`） | ~20,000 | 40,000 | 40,000 × 68 B = **2.72 MB** | 160 KB + 480 KB = **640 KB** | **4.25×** |
| `SurfaceMesh::m_cornersAroundVertex` | 20,000 | C = 120,000 | 480 KB + **5.28 MB** | 80 KB + **480 KB** | **10.3×** |
| UV `vector<vector<Vector2>>` | — | 40,000 | **3.7 MB / 份** | `Vector2[C]` = **1.92 MB** | **1.9×** |

**量级**：完整流水线里 AoS 面表 + 角点邻接 + UV 四份 + 抽取图，粗算 40~60 MB 全是"小的、散的、带分配器头部的"块。CSR 化的直接收益不是省几十 MB，而是**去掉 F 次 malloc/free 与不可预测的指针追逐**——这是岛内分块并行的前提。

UV 那份 3.7 MB × 4 = 14.7 MB 是纯浪费（P2-1），与 CSR 是两件事，可分别做。

### 4.2 目标容器形态

~~~cpp
// src/core/base/Csr.h
namespace AutoRemesher {

template <typename Index>
class CsrView {
public:
    CsrView() = default;
    CsrView(const Index* offsets, const Index* indices, size_t itemCount)
        : m_offsets(offsets), m_indices(indices), m_itemCount(itemCount) {}
    size_t size() const { return m_itemCount; }
    Span<const Index> operator[](size_t i) const
    {
        return { m_indices + m_offsets[i], size_t(m_offsets[i + 1] - m_offsets[i]) };
    }
private:
    const Index* m_offsets = nullptr;
    const Index* m_indices = nullptr;
    size_t m_itemCount = 0;
};

template <typename Index>
class Csr {
public:
    void buildFromCounts(const std::vector<Index>& counts);   // 两遍法第一遍
    void setItem(size_t item, Span<const Index> values);      // 第二遍
    CsrView<Index> view() const
    {
        return { m_offsets.data(), m_indices.data(), m_offsets.size() - 1 };
    }
private:
    std::vector<Index> m_offsets;  // items + 1
    std::vector<Index> m_indices;
};
}
~~~

**索引宽度统一 `uint32_t`**。现有代码已经假设索引可打包 32 位（`meshseparator.cpp:45,64` 的 `maximumPackableVertexIndex` 及超限回退；`autoremesher.cpp:117` 的边打包）。既然已有这个上限，容器层就不该再用 8 字节索引把内存翻倍。>4G 顶点在桌面重拓扑里不可能；`meshseparator` 的 map 回退路径（`:98-101`）可在 M4 删除，改为明确返回 `TopologyInvalid`。

### 4.3 SurfaceMesh 邻接 CSR

现状 `surfacemesh.h:65` 的 `std::vector<std::vector<size_t>> m_cornersAroundVertex`，构造期逐角点 push_back（`surfacemesh.cpp:53`）。该访问器被 `framefield.cpp:129`、`quadparameterizer.cpp:527、621`、`singularitysimplifier.cpp:101` 高频调用。

~~~cpp
class SurfaceMesh {
public:
    SurfaceMesh(Span<const Vector3> positions, CsrView<const uint32_t> triangles);
    Span<const uint32_t> cornersAroundVertex(size_t vertex) const
    {
        return m_cornersAroundVertex[vertex];
    }
private:
    std::vector<Vector3> m_positions;              // 仍持有副本，见下
    std::vector<std::array<uint32_t, 3>> m_triangles;
    std::vector<uint32_t> m_oppositeCorners;
    Csr<uint32_t> m_cornersAroundVertex;
};
~~~

**关于 `m_positions` 是否继续持有副本**：现状 `surfacemesh.cpp:34` 整体拷贝一次。改借用可省 V×24 B，但会让 `SurfaceMesh` 生命周期绑定调用方，而它被跨函数传递（`quadparameterizer.cpp:660` 栈上构造后一路传引用）。**决策：M4 保持持有副本**，只在 pipeline 层减少一次上层拷贝。这不是省内存的战役，是邻接访问模式的战役。

构造仍是两遍法：先数每顶点角点数，再 `buildFromCounts`，再一次填充。第二遍填充当前是 `surfacemesh.cpp:58-76` 的"沿 `nextAroundVertex` 单链表找对面"，改成直接在 CSR 上做同样的事——**算法语义必须逐位保持**，靠 §4.5 的拓扑对拍验证。

### 4.4 QuadExtractor 的图结构数组化

三份并行的小图都是指针追逐：

| 结构 | 位置 | 类型 | 问题 |
| --- | --- | --- | --- |
| 交点去重表 | `quadextractor.cpp:763`、`:950` | `std::map<PositionKey, size_t>` | 红黑树，每节点约 100 B；**两次全量构建**（`:950-952` 在 `holdSingularLines` 里重建） |
| 边邻接 | `quadextractor.h:115` | `unordered_map<size_t, unordered_set<size_t>>` | 每节点一个哈希桶 + 一个哈希集，`simplifyGraph`（`:302-331`）反复增删 |
| 连接属性 | `quadextractor.h:105-106` | `map<pair, ConnectionInfo>` + `set<pair>` | 同一条边记录两处，擦除要同步两处 |

改造顺序（M4/M5，风险从低到高）：

1. `m_halfEdges`（`quadextractor.h:107`，`set<pair>`）→ 打包 `uint64_t` 的 `vector` + 排序二分。`rebuildHalfEdges`（`:1284-1293`）与 `searchBoundaries`（`:1398-1437`）只读它，改动面小、收益直接。
2. `crossPointMap` → **按量化整数三元组做开放寻址哈希**（自建约 80 行），键从 32 B 的 `PositionKey` 降到 `uint64_t`（P0-2 归一化后 |q| < 2²¹，三个 21 位装进 64 位）。M2 已经把 `m_toIntFactor` 换成 `thread_local double m_tolerance` 且换成 `long long`，这一步的前置条件已经满足。
3. `edgeConnectMap` → 度数已知，直接建 CSR；`simplifyGraph` 的增删改为"重建成新 CSR"。**最危险**，因为 `simplifyGraph` 的 `delayPairs`（`:305-320`）依赖迭代顺序。做法：保留原函数，新增 `simplifyGraphToCsr`，两版边集合逐条对拍后再删旧版。
4. `m_connectionInfos` / `m_addedConnections` 合并成一张"边属性" CSR，消除两处同步维护。

**不做的事**：`m_remeshedPolygons` 保持 AoS 到 M5 之后。它被 12 个修缮 pass 直接改写（`quadextractor.cpp:1670-4318` 之间的大半文件），中途换表示会把每个 pass 都变成待验证改动。四边形输出只有 ~1.5 万面，AoS 的常数在那里不是瓶颈。

### 4.5 迁移的兼容与回滚

- `using PolygonList = std::vector<std::vector<uint32_t>>;` 与 `Csr` 并存到 M6，由 `AUTOREMESHER_LEGACY_POLYGONS` 切换，默认走新路径。
- 每步 CSR 替换必须过**拓扑对拍**：同一输入下比较 (a) 顶点数/面数、(b) 排序后的无向边集合 `(min,max)`、(c) 每顶点邻接集合，三者完全相等才算通过。浮点坐标允许末位抖动（`/fp:fast` 已存在），**拓扑必须完全相等**。
- 对拍工具复用 `mesh_metrics compare`：`components` / `edges_non_manifold` / `boundary_loops` / `valence_histogram` 必须逐项相等；这四项相等即等价于拓扑相等（对多边形网格是充分条件）。

---

## 5. 里程碑

### 5.1 验收门总表（唯一权威，阈值集中在这里）

基线 = M0 采集的 `artifacts/baseline/*.json`。**"当前状态"列只写实测，未测就写"待产出"。**

| 门 | 指标（`mesh_metrics` 字段） | 阈值 | 数据来源 | 当前状态 |
| --- | --- | --- | --- | --- |
| G1 | `quad_ratio_pp` 变化 | 下降 ≤ **1.0 pp** | `compare --quad-tol 1.0` | 基线（§11.1）：head10k 92.91% / body15k 99.37% / multi12k 92.18% / scaled10k 92.57% |
| G2 | `valence4_ratio` 不降、`irregular_vertices` 不增（原 G7 的内容） | — | `measure` | multi12k 基线 65.61% → after 62.73%（**回归项**，keep-all 的接缝代价）；head10k after 未单独采（本轮未采） |
| G3 | `boundary_loops` | 不增；补洞类里程碑要求**减少** | `measure` | 基线：head10k **6** / body15k **1** / multi12k **4** / scaled10k **2**；输入 head_clean **2**（§0.3） |
| **G4-0** | 用例标注：`scale_class`（unit/small/large）+ `parts_class`（single/multi） | **未标注的用例不得进入 G4 判定**（D8-3）；G4-b 按 `parts_class` 选 largest（single）或 top-k（multi） | 用例表 / run.json | 现有四用例：head10k unit/single、body15k unit/single、multi12k unit/**multi**、scaled10k **small**/single |
| G4-a | `components_small_face_count`（≤3 面壳） | **≤ 基线**；豁免/容忍上限写作 **≤ 基线 × 1.10（含等号，D8-2）**。**D8-1 前置失效**：基线处于退化态时不适用 | `measure` | head10k 48 / body15k 0 / multi12k 100 / scaled10k 2（§11.7） |
| G4-b | `components_top3_face_ratio`（主结构；`parts_class=single` 时用 `components_largest_face_ratio`） | **≥ baseline − 2 pp**。**多部件禁用 largest**（multi12k largest 0.4779 天然失义）。**D8-1 前置失效**：退化态基线不适用 | `measure` | 基线：head10k 0.9573 / body15k 1.0000 / multi12k **0.9355** / scaled10k 0.9973（§11.7） |
| G4-c | 尺度不变性（两尺度 `largest` 之差） | **≤ 0.02**。**独立用例**（scaled10k 对 head10k），且是 **D8-1 退化态下的主判据** | 两次 `measure` 相减 | 基线 **4.01 pp（不通过）** / after **0.11 pp（通过）** —— 本轮最灵敏的一条 |
| G5 | `edges_non_manifold` | **不增** | `measure` | 基线：head10k **124** / body15k **8** / multi12k **160** / scaled10k **52**（§11.1） |
| G6 | `covered_ratio` | 不降 | 同上 | 基线：head10k **61.11%** / body15k **95.52%** / multi12k **47.03%** / scaled10k **28.19%** |
| G7 | `ref_dev_p95` | ≤ 基线 × **1.02**（rel，**容差 2%**） | `measure --ref INPUT` | 基线（rel）：head10k **2.192e-3** / body15k **8.181e-4** / multi12k **1.843e-3** / scaled10k **2.985e-3** |
| G8 | 单模型 wall | 重构类 ≤ 基线 × **1.10**；提速类（M5）≤ × **0.85**。**必须按 §5.2 的计时协议测**（串行 / 交替 A/B / 3 次中位数），否则不作为判定依据 | 专用基准脚本（§5.2） | 基线：head10k 36.34 s、body15k 12.586 s、multi12k 87.329 s、scaled10k 33.49 s；**本文所有 wall 仅作参考量**（噪声可达 82%，见 §5.2） |
| G9 | 输出 OBJ SHA256 | 同输入连续两次相同 | `Get-FileHash` | **已通过**：head10k `f35b9377…eede`、body15k `c5325ad4…0f53`（after 无开关与基线逐字节相同）、默认路径 body_vox `9377D56A…C0427` |
| G10 | 构建 + 边界检查 | clean ≤ 60 s；`check_boundaries` 0 错误 | CMake/Ninja | 构建 **10.1 s**（通过）；边界脚本待写（M3） |
| G11 | 库内 stderr 体积 | ≤ 200 行 / 次 | 重定向计数 | 基线实测：head10k **3.56 MB** / multi12k **6.94 MB** / scaled10k 3.44 MB / body15k 57 KB（§11.1）；M3-3 的量化目标 |
| G12 | `degenerate_faces` | 不增 | `measure` | multi12k 基线 60 → after 45；scaled10k 0 → 19（随修复恢复真实分片，见 §11.3 豁免规则） |
| G13 | 输出保真：`--obj-precision`（D4） | 默认 **6 位有效数字**（与 Qt 版逐字节对齐）；改默认必须同时重建 baseline 与 after 两套 JSON。**单列，不混进 G1/G5/G7** | `Get-FileHash` + 顶点坐标抽查 | 当前默认 6；`--obj-precision {6,9,17}` 已提供 |
| G14 | CLI 事件契约（§12.5 原文五条） | 抽样 ≥20 事件按 v1 解析全过；未知字段注入不崩；`done` 含 quads/non_quads/vertices/exit；`phase` 行数与 `phaseReport()` ±0 | `--events-json` + ui-go | 第 **1/3/4** 条已验（§13.6.1：62 progress + 1 phase(55 行) + 1 done）；第 2 条待 ui-go 冒烟 |
| **G15** | 模块边界（`tools/check_boundaries.py`） | **退出码 0**：29 文件 0 违规（向上 include / Qt 头 / 非白名单 include）。调用形式 `python tools/check_boundaries.py [任意层级路径，缺省当前目录]`（`resolve_root()` 支持自身/父/祖父三级）；**只有找不到 `src/core` 才 exit 2**，那是目录结构问题不是参数问题 | `python tools/check_boundaries.py` | §17.4 已验：repo 根 / `src` / `src/core` 三种调用均 exit 0；`base/double.h` 注入 QuadExtractor 后 1 违规 exit 1，还原后回 0 |
| **G16** | 结构等价（容器 / 遍历顺序 / 分配策略类改动） | 三步判定（**不许跳步**）：① 三份 sha 全等 → **逐字节等价**，不用重跑基线；② sha 变但拓扑四项（`components`/`edges_non_manifold`/`boundary_loops`/`valence_histogram`）全等 → **浮点累加顺序变化**，可接受，但**必须重跑基线并更新 §11.1**；③ 拓扑四项也变 → **行为改变**，按 D19 定位到具体动作 | `Get-FileHash` + `mesh_metrics measure` | **第一个正例**：M3-5 第一切片（CSR/SurfaceMesh），三份 sha 全等（§18.3） |

**门编号的 canonical 映射（全文以此为准）**：G1 quad_ratio ｜ G2 valence4/irregular ｜ G3 boundary_loops ｜ G4-0~c components ｜ **G5 edges_non_manifold** ｜ **G6 covered_ratio** ｜ **G7 ref_dev_p95（容差 2%）** ｜ G8 wall ｜ G9 sha ｜ G10 build ｜ G11 stderr ｜ G12 degenerate ｜ G13 输出保真（obj-precision）｜ G14 事件契约 ｜ **G15 模块边界** ｜ **G16 结构等价**。**历史文本里出现的 G2=nm / G5=dev / G7=valence4 属旧编号，已在 §5.1 统一纠正**（D11 之后）。

**G4 豁免规则（唯一一条，合并版）**：若 G4 未过，但**同时**满足 `coverage` 上升 **且** `dev_p95` 下降，则判为「形态变化」→ 豁免并记录；`small_face_count` 的容忍上限为 **≤ 基线 × 1.10（含等号，D8-2）**。**前置失效（D8-1，已裁决）**：基线处于退化态（bbox 尺度误差 >5% 或 coverage <35%）时，G4-a/G4-b 计数判据**不适用**，由 G4-c 与 G1/G5/G7 承担，且**禁止以「碎片增加」作为回归证据**（scaled10k 是首个适用者，见 §11.7）。

**阈值总原则（写死在表下，防漂移）**：**阈值一律由基线实测推导，文档只记录推导式（如 `max(0.85, baseline − 2pp)`、`|Δlargest| ≤ 0.02`、`≤ baseline`），不记录某次的具体数值。** 本文出现的所有数字都是「某次基线的快照」；基线换了（新模型、新工具版本、task-4 重建目录）就必须重算，禁止把任何一次实测值当常数抄进文档、脚本或代码。

**阈值防漂移条款**：G4 的 `baseline_ratio − 0.02` 与 G1/G7 的相对阈值都**随基线更新而重算**，禁止把任何一次实测值当常数抄进文档、脚本或代码。基线换了（新模型、新工具版本、task-4 重建目录）就必须重算一遍；本文出现的所有具体数字都是"某次基线的快照"，不是常量。

**G1~G8 的判定集合固定为 6 个模型**：`head_in.obj`、`head_clean.obj`、`head_vox.obj`、`body_vox.obj`、`artifacts/inputs/head_scaled_1e-3.obj`、`artifacts/inputs/multi_part.obj`。前四个验正常路径；**第五个（scaled10k）是 G4-c 专用用例**（同时服务 P0-2 验收，因为 P0-2 的 bug 正是「跨尺度结构不一致」）；第六个（multi12k）验 P0-1 与 G4-b 的多部件口径。

### 5.2 验收门的执行形式（写死，避免各人跑不同口径）

~~~powershell
# 1) 单网格指标（含与输入的偏差/覆盖率）
$py tools\metrics\mesh_metrics.py measure OUT.obj --ref INPUT.obj --json X.json --samples 20000

# 2) 基线 vs 候选对比（CI 门禁：退出码 0 一致 / 1 有回归 / 2 参数或文件错误）
$py tools\metrics\mesh_metrics.py compare BASE.json CAND.json --json CMP.json
~~~

三条指标约定：

1. **baseline 与 candidate 必须同 `--ref`、同 `--samples`**，否则 `covered_ratio` / `ref_dev_*` 不可比。本项目统一用 `--samples 20000`（COMPARISON.md 的实际口径；50000 是更快的档位，改口径要两套一起改）。
2. `compare` 的退出码直接当 CI 门禁（0/1/2），不要自己解析 JSON 再判断——两套判据必然漂移。
3. **指标计算工具本身有版本（D7）**：每次 `measure` 都会把 `mesh_metrics_sha256` 写进同目录的 `metrics_meta.json`。**任何引用指标 JSON 的地方（文档、COMPARISON、CI、本文 §11）都必须同时记录这个 sha**，否则指标不可复现、也无法判断两份 JSON 是否可比。换工具版本 → 必须重建 baseline 与 after 两套 JSON 并更新 COMPARISON.md 头部的 sha；旧 sha 的 JSON 只能作历史参考。

#### 5.2.1 计时协议（G8 专用，不可省略）

**这篇文档里所有 wall 都是参考量，不是判定依据。** 原因是实测过：同一份 after scaled10k 二进制，两次测量得到 **19.1 s 与 34.8 s（差 82%）**，而基线自身两次重复只有 33.49 / 33.62 s（差 0.4%）。差值来源是本机并行跑其他任务时的负载竞争，不是代码差异。同理 COMPARISON.md 里 body15k 的"−17.2%"也只是噪声（两次基线测量自身就差 9.5%）。

因此 G8 的强制协议：

| # | 条款 | 理由 |
| --- | --- | --- |
| T1 | **串行执行**：计时期间不跑编译、不跑其他用例、不留后台任务 | 82% 的抖动全部来自并发负载 |
| T2 | **同机同负载**：baseline 与 candidate 在同一会话内完成，中途不改电源计划/不插拔电源 | 笔记本降频会系统性偏移 |
| T3 | **3 次取中位数** | 单次不可用；中位数抗单次尖峰 |
| T4 | **交替跑 A/B/A/B/A/B**，配对后取中位数之比 | 消除"先跑的那个吃亏/占便宜"的时间漂移 |
| T5 | 每个用例先跑 1 次**丢弃**（文件缓存 + TBB 线程池冷启动） | M1 实测 8.216 s → 7.145 s 就是这个效应 |
| T6 | 报告必须写：机器核数、是否插电、并行任务数、6 次原始值 | 没有原始值的墙钟数字不可复核 |
| **T7 门禁前置条件（复合条款）** | **(a) 记录实际配置**：A/B 一律显式 `-D` 传开关或用全新 build 目录；`run.json` 记录 `CMakeCache.txt` 的实际取值 + `AUTOREMESHER_*` env 旋钮的取值，而不是命令行意图。**(b) 全新构建**：门禁用例必须在**全新 build 目录或 clean 之后**跑——增量目录在编译中断后会留下不一致产物，会把「构建产物问题」误判成「算法回归」（§18.4 的 Release 必崩 / ASan 正常就是这一例） | 二者是同一类问题：**环境与产物状态没被记录，结论就不可复现**。`option()` 的默认值低于 `CMakeCache.txt` 优先级（§13.6 的 7,954 vs 8,101）；中断的增量目录同理（§18.4 的 `build/m3` vs `build/m3b`） |

**用例划分**（依据 COMPARISON.md 的观测）：

- **岛级并行**（M5 之前的收益来源）：用 head10k（8,979 islands）与 multi12k（17,958 islands）——岛多，外层并行才吃满。
- **岛内并行**（M5 的目标）：用 body15k（1 个 island、10,045 面）——它在岛级并行里 `cores=1.00`，这个"劣势"正是岛内并行的测试条件。
- 所以 G8 不写"单模型 wall"，写**两个用例各一条**：`head10k`"岛级" + `body15k`"岛内"。


### 5.3 里程碑总表

| Ms | 名称 | 核心动作 | 状态 |
| --- | --- | --- | --- |
| M0 | 基线定格 | Qt-free CLI + 度量工具 + 6 模型基线 | **工具与 CLI 已交付**；基线 JSON 采集 = task-4（进行中） |
| M1 | Qt-free 构建 | CMake + core 静态库 + TBB + CLI | **已落地（有证据）** |
| M2 | 行为开关切片 | keep-all-islands / relative-tolerance / triangulate + 诚实失败 | **已落地（有证据）** |
| M3 | 目录与契约层 | `git mv` 到 `core/*` + 多 target + 边界脚本 + Result/Progress/Log | 待做 |
| M4 | 数据模型 + P1 | CSR 化 + P1-1~P1-3、P1-5~P1-8 | 待做 |
| M5 | 抽取器拆分 + 并行 | QuadExtractor 拆 TU + 图数组化 + 岛内并行 | 待做 |
| M6 | 兼容收尾 | GUI 目标接回、qmake 决策、旧接口删除、A/B 总报告 | 待做 |

### 5.4 M0 基线定格（工具已交付，采集进行中）

**DoD**：
- [x] 4 用例各跑 **2 次**（参数对齐 `remesh/report_*.txt`），OBJ + report + stderr + run.json 存 `artifacts/baseline/<case>/`（SHA256 双跑一致）
- [x] 每个产物跑 `measure --ref <输入> --json --samples 20000`，JSON 存同目录
- [x] 度量工具可用且自检通过（60 条断言，F10）
- [x] CLI 退出码与报告格式对齐（F6/F7）
- [x] 重复跑第二次确认 G9（4 用例全部一致）
- [x] `artifacts/baseline/SUMMARY.md` 列出四用例全字段（本文 §11.1 转载）

**验收门**：G9 全模型通过；SUMMARY.md 落盘。**当前进度：M0 已收口**（D1）——工具 ✓、CLI ✓、指标采集 ✓（4 用例 × 双跑）、A/B 对照 ✓（§11）。

### 5.5 M1 Qt-free 构建（已落地）

| DoD 项 | 状态 | 证据 |
| --- | --- | --- |
| `autoremesher_core`(STATIC) + `autoremesher_cli`(EXE) 两个目标 | ☑ | `CMakeLists.txt:131、:155` |
| 无 Qt 依赖可全量编译 | ☑ | clean **10.1 s**；`docs/BUILD-CORE.md` §4 |
| TBB 可用（路径 A，路径 B 可切） | ☑ | `CMakeLists.txt:59-116`；`docs/BUILD-CORE.md` §3 |
| CLI 参数/OBJ/报告格式与 Qt headless 对齐 | ☑ | `docs/BUILD-CORE.md` §4 |
| 真实退出码 0/1/2/3 | ☑ | 本次实测 1/2/0；`main_cli.cpp:56-59` |
| 对 body_vox 跑通并产出 obj+report | ☑ | 8,101 quads / 83 non-quads / 8,244 verts |
| 数值与历史 Qt 报告同量级（差异可解释） | ☑ | −0.34%，归因于编译器/浮点路径，core 源码零修改 |
| 同构建结果可复现 | ☑ | SHA256 一致 |
| ③ GUI 目标 | ☐ | 无 Qt，M6 处理 |
| ③ `file(GLOB)` 改显式列表 | ☐ | M3 |

### 5.6 M2 行为开关切片（已落地）

M2 的定位是**给 P0 缺陷装上可开关的机制，并把 `extract()` 的失败语义改诚实**，而不是改默认行为。这样 A/B 能在一个二进制里完成，也能随时回退。

| 开关 | 对应缺陷 | 语义 | 默认 | DoD |
| --- | --- | --- | --- | --- |
| `--keep-all-islands` | P0-1 | 保留全部岛，而不是只留最大 patch | **关**（历史行为） | ☑ 实现；☑ A/B 数字（**转默认被阻断**，需 P0-1b） |
| `--relative-tolerance` | P0-2 | PositionKey 量化改为相对包围盒（岛内 = `voxelSize*1e-3`） | **关**（固定 1e-5） | ☑ 实现；☑ A/B 数字（**修复有效**，待三尺度用例） |
| `--triangulate` | N4 | n 边形按 `num_face_vertices` 扇形三角化 | **关**（历史"每 3 个切"） | ☑ 实现；☐ A/B 数字（V4，未采证） |

| M2 DoD 项 | 状态 | 证据 |
| --- | --- | --- |
| `long` → `long long`，`m_toIntFactor` → `thread_local double m_tolerance` | ☑ | `positionkey.h` / `positionkey.cpp` |
| `extract()` 返回 `!m_remeshedPolygons.empty()` | ☑ | `quadextractor.cpp` |
| `removeIsolatedFaces()` 增加 keep-all 分支 | ☑ | `quadextractor.cpp` |
| **零回归**：不带开关时 body_vox 输出与基线 SHA256 相同 | ☑ | F9（`9377D56A…C0427`） |
| multi12k × `--keep-all-islands` 的差异 | ☑ **已完成**：quads +5.63%、nm ×6.42、comp +44 → **转默认被阻断**（§11.2.2） | verify-runner |
| head_scaled_1e-3 × `--relative-tolerance` 的差异 | ☐ **待产出** | verify-runner |
| `artifacts/COMPARISON.md` | ☑ **已完成**（80 行，含 5 条回归门） | verify-runner |

**M2 与本节规划的关系**：M2 是并行插入的"最小验证切片"，它把 P0-1/P0-2/P1-4 的实现风险提前消化了。因此 §6 里这三条的剩余工作被改写为"切换默认值 + 补 A/B 数字"，而不是从零实现。

### 5.7 M3 目录与契约层（待做）

**目标**：把"事实上存在的层"变成"构建系统强制存在的层"，并落地 §3 的四个契约。

**依赖**：M2 已合入（否则 `git mv` 会和 M2 的改动打架）；M0 基线 JSON（用于"零数值变化"判定，可后补但必须在 M4 之前）。

**涉及**：全部 `src/AutoRemesher/*` → `src/core/{base,geom,numeric,field,cover,param,extract,pipeline}/`；`src/*.cpp|h` → `src/gui/`；18 个转发头改指新路径；`CMakeLists.txt`；新增 `tools/check_boundaries.py`、`core/base/{Result.h,Progress.h,Log.h,Span.h,Csr.h}`；改 `autoremesher.{h,cpp}`、`parameterizer.{h,cpp}`、`quadparameterizer.{h,cpp}`、`quadextractor.{h,cpp}`、`framefield.cpp`、`singularitysimplifier.cpp`、`meshseparator.cpp`、`main_cli.cpp`

**DoD**：
- [ ] `src/core` 下 8 个目录、无游离文件；`check_boundaries.py` 返回 0；三处故意植入的违规分别报 R1/R2/R4
- [ ] 58 处库内 `std::cerr` 全部迁移（G11：stderr ≤ 200 行/次）
- [ ] `remesh()` 返回 `Result<RemeshOutput>`，旧 `bool` 包装保留
- [ ] `extract()` 返回 `Status`（在 M2 的 `!empty()` 基础上升级）
- [ ] `phaseReport()` 返回 `std::vector<PhaseEvent>`；CLI 渲染文本与改造前逐字节相同
- [ ] CLI 退出码扩展 + 部分成功汇总行
- [ ] `file(GLOB)` 改显式列表；`/W4 /WX` 门禁目标跑通

**验收门**：M3 **不改算法**，所以 G1~G9 必须与基线**逐字节/逐项完全一致**（不是容差内）。唯一允许的差异是 phaseReport 不再被打印两遍——这一处差异要单独记录、单独 diff。

### 5.8 M4 数据模型 CSR + P1 缺陷（待做）

**目标**：面表/邻接/抽取图 CSR 化，同时修 P1-1、P1-2、P1-3、P1-5、P1-6、P1-7、P1-8。**第一个允许数值变化的里程碑**。

**依赖**：M3 的 `Csr`/`Span` 容器；M0 基线。

**DoD**：
- [ ] 拓扑对拍工具通过（6 模型 × (V,F,边集合,邻接集合) 完全相等，在"只做 CSR 化"的中间提交上验）
- [ ] P1-1~P1-8 各自独立提交，可单独回滚
- [ ] **P0-1b / M4-10**：patch 边界焊接达成门槛（nm ≤160、comp ≤142、quads ≥7035、coverage ≥47.03%），使 `--keep-all-islands` 具备转默认资格
- [ ] 峰值 RSS 改造前后记录（CSR 部分下降 ≥50%）
- [ ] `irregular_vertices` / `valence4_ratio` 改善；`components` 达到输入连通分量数

**验收门**：G1 ≤1.0 pp、**G5 不增**、G3 减少、**G7 ≤1.02×**、**G2 不退化**、G8 ≤1.10×、G12 不增。
**专项门**：P1-1 修好后 `"Rounding cover to integers"` 必须出现 **≥3 轮**迭代且单调收敛（需要在 `solveIteration` 加计数器事件才能断言）。

### 5.9 M5 抽取器拆分与岛内并行（待做）

**目标**：`quadextractor.cpp`（4,263 非空行）按阶段拆 TU；抽取图数组化；岛内并行拿到 ≥15%。

**涉及**：`core/extract/quadextractor_connections.cpp`（`:759-1257`）、`quadextractor_mesh.cpp`（`:486-757`）、`quadextractor_topology.cpp`（`:1284-1670`）、`quadextractor_passes.cpp`（`:1670-4318`）、`quadextractor.cpp` 只留 `extract()` 编排（`:48-289`）；`pipeline/autoremesher.cpp`（并行调度）

**DoD**：
- [ ] 每个 TU ≤ 1,500 行；`extract()` 变纯编排（≤ 300 行）
- [ ] `searchBoundaries` 逐顶点 cerr 删除（P2-3）；`fixHoles` 两次调用合并
- [ ] 岛内并行：`extractMesh` 环搜索按区域分块；`holdSingularLines` 按独立锥两阶段（每锥产出候选 → 串行合并）

**验收门**：G1~G12 不退化；**G8 单岛模型 wall ≤ 基线 × 0.85**（用 `head_clean.obj`）；`Cores kept busy` 从 **1.00** 升到 **≥1.5** 才算真并行。

### 5.10 M6 兼容收尾（待做）

**DoD**：
- [ ] `AUTOREMESHER_LEGACY_POLYGONS=OFF` 且旧 AoS 路径删除
- [ ] 旧 `bool` 包装与 `setProgressHandler` 删除；R9 禁止旧符号回归
- [ ] `autoremesher.pro` 有明确结论（§8.1），不留"两个构建系统都半死"
- [ ] `artifacts/COMPARISON.md` 给出 M0 → M6 的 6 模型全表

**验收门**：G1~G12 全部通过。

### 5.11 M3 起的依赖关系

~~~text
M0(基线 JSON) ──┬──→ M4 ──→ M5 ──→ M6
                │      ↑        ↑
M3(目录+契约) ──┴──────┘        │
                       M2(开关) ─┘（A/B 数字补齐后可并行）
~~~

- **M3 可以立刻开工**：它不改数值行为，不依赖基线 JSON。
- **M4 必须等 M0 基线**：没有基线就没有 G6/G7 的判定。
- **M5 依赖 M4 的 CSR 容器**，不依赖 M4 的 P1 修复完成度。
- **M2 的 A/B 数字**（multi_part / head_scaled_1e-3）可与其他里程碑并行，它只影响 §6 里三条的"验证"列。

---

## 6. 逐条改造清单

格式：**ID｜优先级｜位置** → 现状 / 改法 / 影响面 / 验证 / **状态**。

### 6.1 P0：静默产出错误结果

#### P0-1 removeIsolatedFaces 只留最大岛

- **位置**：`quadextractor.cpp:1439-1449`（函数）、调用点 `:195` 与 `:199`
- **现状**：`splitToIslands` 后取 `std::max_element` 按面数，**把结果整体赋给 `m_remeshedPolygons`，其余岛无条件丢弃**。调用点 `:195` 无条件先调一次，所以多部件模型输出只剩最大部件。函数无条件 `return true`，即使只有一个岛也会白跑 `rebuildHalfEdges()+fixHoles()`。**实测危害**：`head_quad_10k.obj` 输出含 **70 个边连通分量**（主壳 4,899 面 + 69 个 2~15 面漂浮块，§0.3），而输入 `head_in.obj` 有 **8,979 个壳**——这条规则在清洗前输入上会把几何削掉绝大部分。
- **改法**：
  1. 保留面数 ≥ `max(4, largest * minRatio)` 的所有岛（`minRatio` 默认 0.02），或保留全部岛只删"面积 < ε×中位面积"的碎片。
  2. 返回值改为"是否真的删了东西"（比较删除前后岛数/面数）。
  3. 被删岛数/面数进 `RemeshOutput.stats` + `Status.detail`。
  4. CLI 暴露 `--island-policy {largest,all,none}` + `--island-min-ratio`。
- **影响面**：多部件模型输出变大（正确方向）；单部件模型行为不变但少一次 rebuild；`fixHoles` / `smoothAndProject` 输入面数上升 → 看 G8。
- **验证**（§11.2 已实测）：multi12k 开 keep-all → quads 6,660→**7,035（+5.63%）**、components 142→**186**、non_manifold 160→**1,028（×6.42）**、valence4 65.61%→62.73%，而 dev_p95 仅 −0.31%、coverage 仅 −0.23 pp。**结论：机制生效，但接缝代价大于收益，不能转默认。**
- **状态**：**M2 已落地开关；默认切换被阻断——前置门槛（non_manifold ≤160、components ≤142）未达成，需先做 P0-1b / M4-10 的 patch 焊接。**

#### P0-1b（M4-10）patch 边界焊接与重复顶点归并

- **位置**：`quadextractor.cpp:193-205`（`extract()` 里 keep-all 之后的重建段）、`quadextractor.cpp:1439-1449`（`removeIsolatedFaces`）、结果合并 `autoremesher.cpp:922-942`
- **背景**：P0-1 的开关（`--keep-all-islands`）把多 patch 保留下来了，**但 patch 之间没有焊接**。§11.2 的实测代价：non_manifold **160 → 1,028（×6.42）**、components **142 → 186（+44）**、valence4 **65.61% → 62.73%**，而 dev_p95 仅 −0.31%、coverage −0.23 pp——**+375 quads 没有换来任何覆盖率净收益**。所以现在不是"要不要保留多 patch"的问题，而是"保留之后必须把这些 patch 缝回一张连续曲面"。
- **改法**（三步，必须按序）：
  1. **重复顶点归并**：按量化位置把跨 patch 的同名顶点合并。容差语义与 P0-2 的"交点判重"不同——这里是"焊接容差"（建议 `voxelSize * 1e-2`，比交点判重粗两个数量级），必须用独立参数，**不能复用 P0-2 的 tolerance**，否则会把该保留的细节焊掉。
  2. **T-junction 消除**：相邻 patch 的边界环上，一侧顶点落在另一侧边的中间 → 插入该顶点并重连，再跑一次 `removeNonManifoldFaces()`。此时它才是有意义的（面集已连续，判据才有分辨力）。
  3. **焊接后重算** `rebuildHalfEdges()` + `fixHoles()` 各一次（不要像现在这样在 while 里反复跑，见 §7.1）。
- **前置门槛（写死，未达不得转默认）**：`non_manifold ≤ 160`（基线值）、`components ≤ 142`（基线值），同时 `quads ≥ 7035`、`coverage ≥ 47.03%`。
- **影响面**：输出拓扑结构变化；可能与 P1-5 的补洞交互（焊接会产生新的环）。需要单独一轮 A/B。
- **验证**：multi12k 上 nm/components/valence4 三项回到基线量级且 quads 不降；G4 三项判据同时通过。
- **状态**：**未动，排入 M4（编号 M4-10，人日 3.0，属"接缝"类工作，不并行——它要改 extract() 的核心重建段）**。
- **一条被推翻的推断**：本文 v4 曾推测"keep-all 让 `removeIsolatedFaces` 变近空操作、跳过 while 里的 `splitToIslands`+`rebuildHalfEdges`+`fixHoles`，所以快 33%"。**COMPARISON.md 的 83.391 s（−4.5%）推翻了这个推断**——33% 是 58.7 s 那次负载噪声造出来的假象。**"旧 bug 同时是性能黑洞"的说法撤回**：keep-all 的真实提速只有 4.5%，不足以构成"修 bug 顺便提速"的论据。

#### P0-2 PositionKey 量化：绝对精度 / 向零截断 / 32 位 long

- **位置**：`positionkey.cpp:26`（原 `m_toIntFactor = 100000`）、`:39-41`（原 `(long)(x * m_toIntFactor)`）；使用方 `quadextractor.cpp:763、950、979、1026`
- **现状**：三个缺陷叠加。
  1. **绝对量化**：分辨率恒 1e-5 世界单位。模型量级 10² 时是 1e-7 相对（过细），量级 1e-3 时是 1e-2 相对（不同交点被合并，连接图塌陷）。
  2. **向零截断**：负数截断向 0、正数取整向 0，bin 在 0 附近宽度翻倍，跨界模型缝合处出现非均匀合并。
  3. **32 位 `long`**：Windows LLP64 下 `long` 是 32 位，|x| > 21474.8 溢出（有符号溢出是 UB，实际表现为远点判等）。
- **改法**（M2 已完成前两步）：~~换 `long long`~~ ☑；~~`m_toIntFactor` → `thread_local double m_tolerance`~~ ☑；下一步：**相对包围盒归一化**——进入 `extractConnections` 前算包围盒对角 `d`，步长 `delta = d * 1e-7`，`q = llround((x - origin) / delta)`，断言 |q| < 2²¹（三个 21 位可装进 `uint64_t` 键，供 §4.4 的开放寻址表用）。
- **影响面**：**全流水线数值最敏感处**。判重变细 → 交点变多 → 抽取图变细，可能改变四边形数量与拓扑。两个方向都要验：极小尺度看「塌陷」，极大尺度看「溢出」——§11.6 的 1e5 用例证明基线的长坐标是靠 32 位 `long` wrap **侥幸可用**，M2 改 `long long` + 饱和后行为才确定。**大尺度的错比小尺度更难发现**（基线输出看起来完全正常）。
- **验证**（§11.2 已实测，证据充分）：scaled10k 开 `--relative-tolerance` 后 quads 2,030→**4,830（×2.38）**、coverage 28.19%→**60.80%（+32.62 pp）**、dev_p95 2.985e-3→**2.360e-3（−20.95%）**，而**最硬的一条是 bbox**：基线输出包围盒对角 0.00117208，比理想值 0.00133052 **小 11.99%**，修复后偏差 **0.09%**。基线 `exit=0`、输出非空——教科书级的"静默退化"。
- **状态**：**M2 机制与证据均已就位**；剩余 = 归一化 + 64 位打包键（M4）+ 三尺度交叉用例（1e-3 / 1 / 1e3，见 §11.4 缺口）。**副作用非缺陷**：nm 52→136、components 6→70、degenerate 0→19、boundary_loops 2→4 是"从塌陷态恢复真实分片"，按 G4 豁免规则判通过。

#### P0-3 MainWindow 用空指针判"源/结果"

- **位置**：`mainwindow.cpp:932`（`if (nullptr == m_remeshedVertices && nullptr == m_remeshedQuads)`）；配套 `mainwindow.h:132-133`、`:175-179`；初始化 `mainwindow.cpp:511-512、748、934-935`；使用 `:1028、:1170`
- **现状**：用"结果向量是否为空"推断"当前渲染的是源还是结果"。第一次重网格成功后结果长期非空，之后任何入队的源网格都被当结果处理并触发 `generatePreviewMeshes`（拿到的是上一轮数据）。判断用 `&&`，而 `saveMeshToFile`（`:1028`）用"任一为空即返回"，两处语义不一致。
- **改法**：
  1. `enum class MeshSlot { Source, Decimated, Isotropic, Param, Remesh };`，`m_renderQueue` 改为 `queue<RenderRequest { MeshSlot slot; ResultMesh mesh; }>`。
  2. `renderMeshReady` 按 `slot` 分派，不看指针。
  3. 结果向量改 `std::unique_ptr`，`nullptr` 只表示"该槽位无数据"，不再兼任推理依据。
  4. `saveMeshToFile` / `saveMesh` 共用 `bool hasResult() const`。
- **影响面**：纯 GUI 层。GUI 本次不可构建，**只能在有 Qt 的环境验证**。
- **验证**：脚本序列——载入 A → 重网格 → 载入 B → 确认预览槽位正确。无 Qt 环境只能做代码评审 + 编译（`autoremesher_gui` 目标 M6 才有）。
- **状态**：**未动**。

#### P0-4 无头模式退出码恒 0（三条失败路径同形）

- **位置**：`main.cpp:226`（`return app.exec()`）；`mainwindow.cpp:1058-1062`（载入失败只 quit 不 emit）；`:1065-1068`（陈旧 generator 直接 return，**无人 quit → 挂死**）；`:1199-1203`（无结果 emit 0,0,0）；`saveMeshToFile` 静默返回（`:1028-1029`）
- **现状**：Qt headless 的失败一律 exit 0，其中一条还是永久挂起。
- **改法**：
  1. `runHeadless` 每条失败路径都 `emit headlessFinished(...)` 并带 `StatusCode`（信号加参数）。
  2. `main.cpp:182-218` 的 lambda 把 `StatusCode` 映射成退出码（同 §3.1 表），用 `QCoreApplication::exit(code)` 而不是 `quit()`。
  3. `m_quadMeshGenerator != nullptr` 的死路改成"先收掉旧任务"或明确报错，绝不静默 return。
  4. 根治：GUI 的 headless 分支直接复用 `src/cli/main_cli.cpp` 的逻辑（已是无 Qt 实现），GUI 只做参数转发。
- **影响面**：修好后 `.github/workflows/release.yml` 的冒烟测试（现在只断言"输出文件存在"）才能升级为退出码断言。
- **验证**：三条失败路径各一条命令，断言 1/2/3。**CLI 侧本次已实测 1/2/0 正确**；Qt 路径待有 Qt 环境。
- **状态**：**CLI 侧已由 M1 解决；Qt 路径未修**。


### 6.2 P1：算法质量与稳定性

#### P1-1 MILS 取整阈值退化成"一次全取整"

- **位置**：`mixedintegerleastsquares.cpp:456-470`
- **现状**：阈值循环只取**第一个**周期变量的距离（`break`），而且写成了 `max(d + .001, 1.0)`。因为 `d` 是到最近整数的距离，`d ∈ [0, 0.5]`，所以阈值恒 `>= 1.0`；随后第二个循环的条件是 `d < threshold`，**恒真** → 第一轮就把**所有**整数变量固定。设计意图"逐次固定最接近整数的少数变量再重解"完全失效，混合整数退化为"一次四舍五入 + 一次线性解"。原文抄录（只改注释）：

~~~cpp
double threshold = 1e20;
for (size_t i = 0; i < m_kernelSize; ++i)
    if (!m_fixed[i] && m_period[i] > 0) {
        double d = std::fabs(m_values[i] / m_period[i] - std::round(m_values[i] / m_period[i]));
        threshold = std::max(d + .001, 1.0);   // 应为 min
        break;                                 // 只看第一个周期变量
    }
for (size_t i = 0; i < m_kernelSize; ++i)
    if (!m_fixed[i] && m_period[i] > 0) {
        double d = std::fabs(m_values[i] / m_period[i] - std::round(m_values[i] / m_period[i]));
        if (d < threshold) m_fixed[i] = true;  // d < 1.0 恒真
    }
~~~

- **改法**：① 阈值改成"遍历全部未固定周期变量取最小值 + 安全带 0.05"；② 去掉 `break`；③ 加"每轮至少固定 1 个"防死循环；④ 不收敛时返回 `SolverFailed` 而不是等到上限。
- **影响面**：四边形质量变（更少扭曲/奇异点）；cover 求解时间上升（多轮重解），由 G8 ≤1.10× 覆盖。**陷阱**：`solveQuadCover` 返回 `s.converged()`（`quadparameterizer.cpp:607`），而 `parameterize` 失败会让整岛丢结果（`autoremesher.cpp:855-861`）。修好 MILS 反而可能让某些岛整块消失——必须同时把"不收敛"降级为"用当前最优解 + 记状态"。
- **验证**：`irregular_vertices` / `valence4_ratio` 改善；G1 ≥ −1.0 pp；断言 `Rounding cover to integers` 事件 ≥3 轮。
- **状态**：**未动**。

#### P1-2 CLS 仿射替换链无环检测

- **位置**：`constrainedleastsquares.cpp:155-170`（`resolve` 的 `while`）、`:211-220`（单根/双根替换建立）；同模式在 `mixedintegerleastsquares.cpp:310-313`（`finalizeM1`）、`:376-379`
- **现状**：`resolve` 沿 `m_substitutions[cursor].root` 走，条件 `!fixed && root != cursor`，**没有访问计数、没有深度上限**。当前构造方式理论上不产生环，但只要有人放宽 `:214-217` 的双根合并（例如允许已固定节点重新变成根），立刻无限循环——而这类改动正是重构期最常见的。
- **改法**：① 深度上限 `m_variableCount` + "回到起点"判定，命中返回 `SolverFailed` 并走拉格朗日回退；② 用 **union-find + 按秩合并** 取代父指针全表 resolve（当前最坏 O(n²)，改后 O(α(n))）；③ `finalizeM1` 两处 `while` 同样加计数。
- **影响面**：正常输入数值不变（根相同），只在退化输入下把"挂死"变"返回错误"。
- **验证**：单测构造 A→B、B→A，断言 n 步内返回错误；6 模型 G1~G12 逐位不变。
- **状态**：**未动**。

#### P1-3 FrameField 多锐边后写覆盖

- **位置**：`framefield.cpp:101-110`
- **现状**：对每面遍历 3 条边，凡"锐边或边界边"就写 `periodic[2*f]` / `periodic[2*f+1]` 为该边方向的场角并 `locked[f]=1`。**一面有 2~3 条锐边时只有最后遍历到的那条生效**，且作为**硬约束**进 CLS（`:174-178`），选错就精确地错。注意 `oppositeCornerIndex == npos`（边界边）也走这条路径，边界面上的多条边界边同样互相覆盖。
- **改法**：① 收集该面全部锐边方向，按 4-对称性折叠到 [0, π/2) 后取圆均值（`atan2(Σsin4θ, Σcos4θ)/4`）；② 多锐边面可选降级为**软约束**（energy 行 + 权重），因为互相矛盾的锐边无唯一正确解；③ `locked` 保留"至少一条锐边"语义，角度用聚合值。
- **影响面**：硬表面模型（`--sharp-edge 60` 那批）方向场改变，进而改变 cover 与抽取。必须按 `sharp-edge 30/60/90` 三档分别 A/B。
- **验证**：硬表面模型 G1 不降、`irregular_vertices` 不增；单测：两面成 90° 且每面 2 条锐边，断言场方向是圆均值而非任一条。
- **状态**：**未动**。

#### P1-4 extract() 恒返回 true

- **位置**：`quadextractor.cpp:288`（原 `return true`）；调用方 `autoremesher.cpp:855-860`
- **现状**：函数体 242 行无失败分支，`:855` 的 `if (!thread.remesher->extract())` 不可达。
- **改法**：M2 已改为 `return !m_remeshedPolygons.empty()`；M3/M4 升级为 `Status`：空多边形 → `ExtractionEmpty`；`smoothAndProject` 找不到投影目标 → `TopologyInvalid`；调用方累计 `failedIslands`。
- **影响面**：会在"静默成功但输出更少"的岛上产生可见失败；多岛模型 `failedIslands` 可能非 0 → CLI 汇总行必须打印。
- **验证**：单三角形退化输入断言 exit 3 而非 exit 0 + 空文件。
- **状态**：**M2 已修（诚实布尔）；Status 化待 M3**。


#### P1-5 边界环 >65 直接放弃补洞

- **位置**：`quadextractor.cpp:1295-1309`（`if (loop.size() > 65)`）
- **现状**：魔数 65；超限环打一行 cerr 就跳过，**不计数、不上报**；`fixHoles` 在 `extract()` 内被调两次（`:164、:204`），每次重扫所有环。
- **改法**：① 阈值参数化（`--max-hole-edges`，默认 65），report 输出"放弃补洞的环数 / 最大被放弃长度"；② 超大环改**条带填充**（沿环长切成 ≤65 段分别 `fixHoleWithQuads`）；③ `fixHoles` 只调一次。
- **影响面**：大洞被补上 → 面数上升，`boundary_loops` 应下降（G3 的"减少"要求）。
- **验证**：G3 边界环数与 `boundary_vertices` 下降；**G7（dev_p95）不劣化**（补的面要投影回原表面）。
- **状态**：**未动**。

#### P1-6 两套场算法叠加语义不明

- **位置**：`quadparameterizer.cpp:680-682`
- **现状**：`if (!(guidance && guidance->size() == mesh.faceCount())) smoothCrossField(...)` 然后**无条件** `brushFieldAlongSpanningTree(...)`。`Parameterizer` 总是传非空且尺寸正确的 `field`（`parameterizer.cpp:407-409`），所以 `smoothCrossField` 在本流水线里**永不执行**，而生成树刷场无条件覆盖 FrameField 的解。等于"花 2.9 ms 解全局场，然后用生成树刷一遍抹掉"。
- **改法**：`--field-brush {always,never,auto}`，默认改 `never`（尊重 guidance），保留 `always` 供 A/B。
- **影响面**：**可能显著改变方向场**，进而改变全部下游。M4 里最需要独立 A/B 的一条，单独提交、单独一轮指标。
- **验证**：`always`（现行为）vs `never` 在 4 个正常模型的 G1/G5/G7 对比表；若 `never` 更差则默认保持 `always` 并把 `smoothCrossField` 删掉（不留死路径）。
- **状态**：**已定性（V9 / §15，D14）**：一套在用 + 一套死代码——`smoothCrossField` 在本流水线里**从未执行**（`guidance` 恒非空，`quadparameterizer.cpp:680` 条件恒假），强制打开（`always`）在 A/B 两点都是负收益；`brushFieldAlongSpanningTree` 是**承重墙**（关掉后 CPU 134 s / 216 s 无有效输出）。**处置**：`smoothCrossField` 标 dead code 并在 M3 删除；brush 保留、`off` 仅诊断。原「二选一调参」议题关闭。

#### P1-7 SurfaceMesh 放过越界顶点索引

- **位置**：`surfacemesh.cpp:51-52`
- **现状**：构造时 `if (vertex >= vertexCount()) continue;` 只跳过建邻接，**三角形照旧 push 进 `m_triangles`**（`:42`）。之后 `cornerVertex(c)` 对这条三角形返回越界索引，`position(v)` / `cornersAroundVertex(v)` 就是越界读。
- **改法**：构造期过滤掉含越界索引的三角形 + 记录丢弃面数；`faceCount() != triangles.size()` 现在已是失败条件（`quadparameterizer.cpp:661-662`），过滤后语义由"整体失败"变"丢了几面"。
- **影响面**：只对畸形输入有影响；正常输入 0 丢弃。
- **验证**：单测（含索引 999 的三角形 → 被丢弃、`faceCount()` 减 1、ASAN 无越界）。
- **状态**：**未动**。

#### P1-8 OBJ 载入把 n 边形按三元组硬切

- **位置**：`src/cli/main_cli.cpp:236-244`（`for (size_t i = 0; i + 2 < shape.mesh.indices.size(); i += 3)`）；Qt 侧 `MainWindow::loadObj` 同病
- **现状**：任意 n 边形的索引流被无条件按每 3 个一组切开并丢弃尾部索引。四边形输入的索引会被错位解释，**这是"输入是四边形网格"时的静默几何错误**。
- **改法**：按 `shape.mesh.num_face_vertices[i]` 做扇形三角化（v0, vi, vi+1）。
- **影响面**：只影响 n 边形输入；三角输入字节不变。
- **验证**：构造 1 个四边面 + 1 个五边面的 OBJ，断言三角化后的面集合正确（对比 `mesh_metrics measure` 的 faces 与 `edges_unique`）。
- **状态**：**M2 已提供 `--triangulate`，默认仍关**（保持与历史输出字节一致）；默认切换随 M4。

### 6.3 P2：性能 / 可维护性 / 卫生

| ID | 位置 | 现状 | 改法 | 验证 |
| --- | --- | --- | --- | --- |
| P2-1 | `parameterizer.cpp:415-416`；`autoremesher.cpp:842、886-895`；`quadmeshgenerator.cpp:108-109`；`mainwindow.cpp:1161-1162` | **UV 四份深拷贝**：`m_originalTriangleUvs = cover.triangleUvs` 一份 + `make_unique(...cover.triangleUvs)` 又一份 + `capturedUvs` / `capturedOriginalUvs` 各一份 + `m_isotropic*` merge 一份。每份 ~3.7 MB（§4.1） | ① `cover.triangleUvs` 改 move 移交；② "原始 UV"只服务 [Param] 预览（`previewmeshgenerator.cpp:200-203` 是唯一消费者），改成存"每面是否被修复"的 `vector<uint8_t>`（1 B/面） | 输出 OBJ 逐位不变；峰值 RSS 下降；[Param] 预览仍能区分修复面 |
| P2-2 | `parameterizer.cpp:42-130`（函数）、`:388-389`（被注释的调用） | `computeConformalScaling` 约 89 行，**零调用者**（全仓 grep 只有定义 + 注释里的调用） | 删除，需要时从 git 历史取回 | 编译通过；6 模型 G1~G12 不变（证明确实没被调用） |
| P2-3 | `quadextractor.cpp:1401、1415、1422、1425、1436` | `searchBoundaries` **逐顶点**打 cerr，`fixHoles` 调两次 → 万级行、**3.5~3.9 MB stderr**（F13） | 全删，只留一条"N 个边界环，最大 X"的 Debug 汇总 | stderr ≤200 行/次（G11）；wall 改善 |
| P2-4 | 全局（§1.3 C1） | 58 处库内 `std::cerr` | 走 §3.4 `Logger`，调试类降 Debug 且默认关 | `check_boundaries` R4 = 0 |
| P2-5 | `radians.h`；`include/AutoRemesher/QuadRemesher` | `namespace Radians` 零引用（`radians.h:27-32`）；`QuadRemesher` 转发向不存在的 `quadremesher.h`（全仓无此文件） | 两文件删除 | 构建通过 |
| P2-6 | `include/AutoRemesher/*` 18 个 | 11 个用 `../src/...`（相对自身是错的，靠 `-Iinclude` 回退搜索才成立），6 个用 `../../src/...`，两种深度并存 | 统一 `../../src/core/模块/X.h`，或改 `configure_file` 生成 | 去掉 `-I.` 仍能编译；R3 = 0 |
| P2-7 | `version.h:25-32` | 8 个 `#define APP_* PROJECT_DEFINED_*`，**无 `#ifndef` 默认值**；脱离 qmake/CMake 直接编译失败 | 每个加 `#ifndef` 默认值，CMake 侧继续覆盖 | 单独编译 `version.h` 通过 |
| P2-8 | `mainwindow.cpp:615-645` | `saveMesh` 成功后不复位 `m_saved`，标题一直挂修改标记 | 成功后置位 + `updateTitle()` | 手工（需 Qt） |
| P2-9 | `mainwindow.cpp:1167`、`:928` | 对象已 `moveToThread`（`:914` 同类），却在 GUI 线程 `delete` | 改 `deleteLater()`，或 `thread::finished` 之后 delete | 需 Qt；待验证 |
| P2-10 | `mainwindow.cpp:1033-1045`；`main_cli.cpp:78-83` | OBJ 写出 6 位有效数字（`QTextStream` 默认），对 10² 量级模型是 1e-4 绝对量化 | 默认提 `%.9g`，加 `--obj-precision` 开关；**默认变更必须与 G9 一起验** | 顶点坐标相对误差 < 1e-7；`ref_dev_p95` 改善 |

---

## 7. 并行化与可观测性

### 7.1 并行化路线：岛级 → 岛内求解器分块

**现状实测**：`Cores kept busy across the parallel phase: 1.00`（F12）。两个主用例（head/body）都是单岛，外层 `tbb::parallel_for`（`autoremesher.cpp:738、876`）退化为单线程。串行段不并行，并行度天花板就是 1。

| 级别 | 对象 | 现有结构 | 改法 | 预期 |
| --- | --- | --- | --- | --- |
| L1（已有） | 岛 | `parallel_for` over `islandContexes`（`:738`）、over `parameterizationThreads`（`:876`） | 保留；单岛时无收益 | 多岛模型已经吃满 |
| L2（M4） | 岛内数据并行 | `framefield.cpp` 已对 facet / vertex 做 `parallel_for`（`:93、:113、:125、:161、:216`） | 把同类模式补到 `singularitysimplifier` 的法向/帧构建、`quadparameterizer` 的 per-face 场构建 | 中等规模模型 5~15% |
| L3（M5） | 岛内**求解器**分块 | 全部串行：Eigen 单线程分解、`computeSeam` 的 BFS、`holdSingularLines` 的锥遍历、`extractMesh` 的环搜索 | 见下四条 | 单岛 15~30% |

**L3 的四条具体切法**（风险从低到高）：

1. **`holdSingularLines`（`quadextractor.cpp:934-1257`）**：每个饥饿锥独立遍历（`:1019-1045` 起），彼此只通过 `branchesOfPoint` 交互。改成"每锥产出候选连接列表 → 串行合并"，合并阶段用已排序边集合去重。**天然可并行**。
2. **`extractMesh` 的环搜索（`:486-757`）**：以面为单位分配，彼此只在共享顶点处竞争。按"顶点环连通块"分区，块内并行、块边界串行。
3. **`computeSeam`（`quadparameterizer.cpp:375-445`）**：当前 `while(changed)` × 全角点扫描是最坏 O(C²)。改成**队列式一轮 O(C)**（只重扫被改角点的邻居）——这是算法改进不是并行，但消除同一热点。
4. **cover 求解（`solveQuadCover`，`:447-608`）**：MILS 的 4-pass 消元本质串行。可选方向是按 kernel 变量分块做 Jacobi 式迭代，但会改变收敛路径与结果——**建议不做**，除非前三条吃完仍不够。

**不并行的理由要写进代码注释**：CLS 的正规方程分解、`simplifyGraph` 的 `delayPairs` 顺序敏感逻辑，一旦并行就改变结果。这类"明确不并行"的判断必须留在注释里，否则下一个维护者会重新试一遍。

### 7.2 可观测性：phaseReport → 结构化事件

**现状**：`std::vector<std::string>`（`autoremesher.h:181-184`），构造在 `autoremesher.cpp:963-1044`，然后**库自己打印**（`:1046-1047`），CLI 又打印一次（`main_cli.cpp:350-351`）。已被格式化的文本无法断言、无法入库、无法比较两次运行。

**目标**：

~~~cpp
struct PhaseEvent {
    const char* name;          // "Solving frame field"
    float order;               // 流水线位置，用于稳定排序
    long long microseconds;    // 累加值（多岛求和）
    size_t islandIndex;        // 汇总行填 npos
    bool accumulated;          // true = 多岛累加，false = 单次
};
std::vector<PhaseEvent> phaseReport() const;
~~~

三个消费方：

| 消费方 | 用法 |
| --- | --- |
| CLI | 渲染成现行文本（**逐字节兼容**），新增 `--phase-json OUT.json` |
| 测试 | 断言 `Rounding cover to integers` 出现 ≥3 次（P1-1 专项门）、Total ≤ 阈值 |
| CI | 与基线 JSON 对比各阶段耗时，回归 >20% 报警 |

**必须新增的计数事件**（否则验收门无法自动判定）：

- **`Field brush mode: <legacy|off|always>` 这一行**（`quadparameterizer.cpp:740`，每进程打印一次）：它决定场算法走哪条链路，必须与 `run.json` 一起归档。**并入 §5.2.1 的 T7**——T7 覆盖「一切影响数值行为的输入」，本条是它的第 3 个实例（前两个是 CMake 开关与 MILS env 旋钮）。
- **MILS 迭代轮数与调度参数**（P1-1 / §14）：`plannedRoundCount()` 的解析值、实际轮数、`band` / `batch` 的取值（含 env 覆盖）——M4-1b 证明「轮数用尽 = 静默丢岛」，所以这三个数必须进事件流，否则下一次同样的悬崖还是看不见
- 被放弃补洞的边界环数（P1-5）
- 被丢弃的岛数与面数（P0-1）
- `failedIslands` / `totalIslands`（§3.1）
- 被过滤的越界三角形数（P1-7）

**日志分级的落地**：`LogLevel::Debug` 默认关，`--verbose` 打开。这样 F13 的 3.5 MB 在默认路径下变成 0，需要时仍可拿到。

---

## 8. 兼容期与回滚

### 8.1 qmake 与 CMake 并存策略

| 阶段 | qmake（`autoremesher.pro`） | CMake | 说明 |
| --- | --- | --- | --- |
| M1/M2（现在） | 仍是"官方"GUI 构建 | 只构建 core + CLI | 两者源清单必须同步；目录重构会打破这一点 |
| **M3（D9 之后）** | **已移入 `legacy/gui-qt/`，主链不再引用** | core + CLI + 边界检查 | 原 D2 的 `gen_sources.py` 双输出方案**作废**（core 源清单本就只有 CMake 一份；`legacy/gui-qt/autoremesher.pro` 保留其历史 SOURCES 列表，不再要求同步） |
| M6 | 不适用（已冻结） | core/cli/测试的唯一构建系统 | 如需恢复 GUI，走 §12.6 的四步，**不是**恢复 qmake 并存 |

**已裁决（D9，取代 D2）：GUI 移出主链，qmake 随之进入 `legacy/gui-qt/`。** D2 的「保留薄壳」方案**作废**——薄壳的前提是「GUI 仍要发布」，而 D9 的前提是「GUI 冻结、不发布」；既然不再发布，qmake 留在主链只会让源清单同步问题继续存在。

**D2 中仍然有效的部分**：`tools/gen_sources.py` 的「单一来源」原则保留，但**输出只剩一份**：

~~~text
tools/gen_sources.py           单一事实来源：模块 -> 文件清单（表驱动）
   └── 生成 cmake/core_sources.cmake   （set(AUTOREMESHER_CORE_SOURCES ...)）

CMakeLists.txt                 include(cmake/core_sources.cmake)
legacy/gui-qt/autoremesher.pro 保留其历史 SOURCES 列表，不再要求与主链同步（冻结快照）
~~~

三条约束（从四条缩减，因为 qmake 那一份不再需要生成）：

1. **单一来源在脚本里**，产物进版本库（生成物 diff 可读，且避免「忘了跑脚本」导致的构建差异）；CI 增加一步 `python tools/gen_sources.py --check` 校验产物与脚本一致。
2. 目录重构（M3-1）与清单生成（M3-2）必须**同一次提交**落地，否则中间态编译不过。
3. 恢复条件：Qt 侧 CI 恢复绿色并跑通 GUI 构建后，按 §12.6 的四步恢复；恢复时 qmake 随 `legacy/gui-qt/` 一起搬回，**不恢复「两套构建系统并存」的状态**。

### 8.2 行为开关（feature flag）

| 开关 | 默认 | 生效里程碑 | 删除/回滚期限 |
| --- | --- | --- | --- |
| `--keep-all-islands` / `--island-min-ratio` | 关 | M2 提供，M4 切默认 | M6 删除开关（成为唯一行为） |
| `--relative-tolerance` | 关 | M2 提供，M4 切默认 | M6 删除 |
| `--triangulate` | 关 | M2 提供，M4 切默认 | M6 删除 |
| `--field-brush legacy|always` | **legacy（D3）** | M4（P1-6） | M6 删除（保留选定值；翻默认需新建 baseline） |
| `--max-hole-edges` | 65 | M4 | 长期保留（用户可调） |
| `AUTOREMESHER_LEGACY_POLYGONS` | ON | M4~M6 | M6 删除 |
| `AUTOREMESHER_FP_PRECISE` | OFF | M4 | 长期保留（对拍用） |
| `AUTOREMESHER_TBB_FROM_SOURCE` | OFF | M1 已有 | 长期保留（无预编译库的机器需要） |
| `--obj-precision {6,9,17}` | **6（D4）** | M4（P2-10） | 长期保留（对拍用） |
| `--verbose`（Debug 日志） | 关 | M3 | 长期保留 |

**开关纪律**：每个开关必须有一个"何时删除"的里程碑。没有删除期限的开关就是永久分叉——上表每行都已填期限。

### 8.3 二进制对比与 A/B 方法

| 层 | 方法 | 判据 | 工具 |
| --- | --- | --- | --- |
| L1 字节 | 输出 OBJ 的 SHA256 | 相同 = 零回归（M2 用它证明了 F9） | `Get-FileHash` |
| L2 拓扑 | `components` / `edges_non_manifold` / `boundary_loops` / `valence_histogram` | 逐项相等 = 拓扑等价 | `mesh_metrics compare` |
| L3 几何 | `ref_dev_mean/p95/max`、`covered_ratio` | G7 ≤1.02×、G6 不降 | `measure --ref` |
| L4 时间 | 各阶段 `PhaseEvent.microseconds` + 总 wall | G8 | CLI 计时 + report |

**A/B 执行规范**：

1. **同机、同构建配置**跑 baseline 与 candidate；`--ref` 与 `--samples` 必须一致（否则 L3 不可比）。
2. 每个模型**至少 2 次**，取第二次——首次有文件缓存与 TBB 线程池冷启动差异（M1 的 8.216 s → 7.145 s 就是这个效应的实例）。
3. 判定用 `compare` 的退出码，不自己解析 JSON。
4. 回归定位顺序：先 L2（拓扑变 → 逻辑改动），再 L3（几何漂移 → 数值改动），最后 L4。

### 8.4 回滚触发条件

| 触发 | 动作 |
| --- | --- |
| G1 下降 > 1.0 pp，或 **G5 增加** | 该提交回滚（不是「再调一版」） |
| **G7 > 1.02×** | 回滚；若确认是预期（例如补洞），在 `COMPARISON.md` 记录并调整基线 |
| G9 失败（不确定性） | 立即回滚——不确定性意味着存在未受控的并行/哈希顺序依赖 |
| G8 超 1.10× 且无正当理由 | 回滚，或降级为默认关的开关 |
| G11 stderr 反弹 | 回滚（说明有人又加回了 cerr） |

---

## 9. 工作量与执行顺序

### 9.1 人日估算

按"熟悉本代码库的工程师"计，含自测、不含评审等待。

| 任务 | 人日 | 依赖 | 可并行 |
| --- | --- | --- | --- |
| M0 基线采集（6 模型 × measure/compare + INDEX） | 1.0 | CLI ✓、metrics ✓ | 是 |
| M2 A/B 数字补齐（multi_part、head_scaled-1e-3、三个开关） | 1.5 | M0 | 是 |
| M3-1 git mv + include 改写 + 转发头修复 + version.h 默认值 | **0.5（与 M3-2 合计，已花）** | ✅ 已完成（§17.1/17.2） | — |
| M3-2 边界检查脚本 + CMake 多 target + 生成式源清单 | （含上行 0.5） | ✅ 已完成（§17.3/17.4，G15） | — |
| M3-3 Result/Status + StatusCode 贯穿（含 58 处 cerr 清理） | 3.0 | M3-1 | 与 M3-2 并行 |
| M3-4 ProgressSink + PhaseEvent + CLI 渲染逐字节兼容 | 2.0 | M3-1 | 与 M3-3 部分并行 |
| M3-5a CSR 容器（CsrView/Csr）+ SurfaceMesh 邻接 | **0.5（已花，原估 2.0）** | ✅ 已完成（§18，G16 首例） | — |
| M3-5b 面表 `vector<vector<size_t>>` → CSR | **1.0（未动）** | M3-5a | 否 |
| M3-5c 抽取图数组化（halfEdges / crossPointMap / edgeConnectMap） | **1.5（未动）** | M3-5a；风险最高，见 §4.4 | 否 |
| M4-1 P1-1 MILS 阈值 + 迭代计数事件 | 1.5 | M0（**D1：与 M3 并行**） | 是 |
| M4-2 P1-2 CLS union-find + 环检测 | 1.5 | — | 是 |
| M4-3 P1-3 FrameField 多锐边圆均值 | 1.5 | — | 是 |
| M4-4 P1-5 条带补洞 + 参数化 | 2.0 | — | 是 |
| M4-5 P1-6 刷场开关 + A/B 定默认（= V9） | **1.0（已花）** | D1：与 M3 并行 | ✅ **已完成**（§15 / D14）：smoothCrossField 死代码、brush 保留 |
| M4-6 P1-7 越界索引过滤 | 0.5 | — | 是 |
| M4-7 P0-1 / P0-2 / P1-8 默认值切换 + 数字确认 | 1.0 | M2 A/B | 是 |
| M4-8 抽取图数组化（halfEdges → crossPoint 哈希 → edgeConnect CSR） | 4.0 | M3-5 | 否 |
| M4-9 P2-1 UV 四份拷贝消除 | 1.0 | — | 是 |
| M4-10 P0-1b patch 边界焊接与重复顶点归并（不并行：改 extract() 核心重建段） | 3.0 | P0-1 的 A/B 数字（§11.2） | 否 |
| M4-1b 取整调度参数化 + 自适应迭代上限 | **0.5（已花）** | — | **已否决**（§14.6 D12）：调度扫描到边界，nm 收益随操作点反转 |
| M4-1c 尺度耦合（每面 log 尺度自由度 + t 软约束 + 重解 UV） | **4~6（未排期）** | M5 已交付（否则 G8 必挂） | 否 |
| M4-5b 折痕权重四模式扫描（length/invlen2/cotan/cotanlen） | **0.5（已花）** | — | **已否决**（§16 D16）：四模式无一同时过 A/B，方向相反 |
| M4-5c 折痕方向作为约束进 curl correction 的 3F 系统 | **1.5~2（未排期）** | M5 已交付（3F 未知数放大求解规模） | 否 |
| M5-1 QuadExtractor 拆 5 个 TU + extract() 编排化 | 2.5 | M4-8 | 否 |
| M5-2 holdSingularLines 锥级并行 | 2.0 | M5-1 | 否 |
| M5-3 extractMesh 分区并行 + computeSeam 队列化 | 3.0 | M5-1 | 否 |
| M5-4 P2-3 逐顶点日志删除 + 性能复测 | 0.5 | M3-3 | 是 |
| M6-1 GUI 目标接回 + P0-3 / P0-4 / P2-8 / P2-9 | 4.0 | 有 Qt 环境 | 是 |
| M6-2 旧接口/旧路径删除 + R9 规则 | 1.5 | M5 | 否 |
| M6-3 qmake 决策落地 + 源清单单点化 | 1.0 | M6-2 | 否 |
| M6-4 COMPARISON.md + 全量回测 | 1.5 | 全部 | 否 |
| **G-1 legacy 迁移**（GUI 源 + qrc + shaders + resources + .pro 搬入 `legacy/gui-qt/`，主链断引用） | **0.5** | D9 | 与 M3-1 可并行（不同目录） |
| **G-2 ui-go 前端**（进程边界 + NDJSON 消费者 + 本地 Web UI + SSE） | **3.0** | G14 的 CLI 侧先落地 | 是 |
| **G-3 autoremesher_capi + cgo**（未排期，仅估算） | **2.0** | §12.4 的触发条件达成后才排 | — |

**合计算式（唯一权威，调整时只改这里）**：

~~~text
base 47.0      = M0~M6 原始计划（含 M3-1 2.0 + M3-2 1.5 的原估）
M3 第一刀结算   = 原估 3.5 → 实花 0.5（省 3.0，D19）
已排期增量 4.5  = legacy 0.5 + ui-go 3.0 + M4-1b 0.5 + M4-5b 0.5
──────────────────────────────────────────────────────────────
计入合计 48.5  = 47.0 − 3.0 + 4.5
~~~

| 类别 | 明细 | 人日 | 是否计入合计 |
| --- | --- | --- | --- |
| 基线（已结算 M3 第一刀） | M0~M6 原始计划 47.0 − M3 省下的 3.0 | 44.0 | ✅ 计入 |
| 已排期增量 | legacy 0.5 + ui-go 3.0 + M4-1b 0.5 + M4-5b 0.5 | 4.5 | ✅ 计入 |
| **小计** | | **48.5** | — |
| **已花** | M4-1b 0.5 + M4-5b 0.5 + M3 第一刀 0.5 + **M3-5a 0.5**（M4-5 的 1.0 已含在基线内） | **2.0** | ✅ 计入 |
| **未排期 backlog** | capi 2.0 / M4-1c 4~6 / M4-5c 1.5~2 | **7.5~10** | ❌ **不计入** |

**口径规则（D18）**：① 「已花」与「未排期」**必须分栏统计**——本轮对不上的直接原因就是两类混在一列里；② 以后任何调整**只改上面的算式脚注**，禁止只改总数不改明细；③ 未排期项永远不进合计，哪怕已经估了人日。

**风险溢价**：P1-1 与 P1-6 都可能触发"结果显著变化 → 重新校准基线"的连锁（各 +1 人日）；M4-8 的 `simplifyGraph` 顺序敏感性是本计划最大的单点风险（+2 人日）。

### 9.2 执行顺序（关键路径）

~~~text
M0 收口(基线 JSON) ─┬─ 链 A：M3-1(2) → M3-5(3) → M4-8(4) → M5-1(2.5) → M5-3(3) → M6-2(1.5) → M6-4(1.5)
                    ├─ 链 B：M3-2(1.5) → M3-3(3) → M3-4(2)          （与链 A 交错，改边界脚本与契约）
                    └─ 链 C：M4-1(1.5) + M4-5(1) 立刻开工 ─→ M4-2/3/4/6(5.5) ─→ M4-7/9(2) ─→ M5-2/4(2.5)
                                                                    （D1：与链 A/B 并行，不依赖 CSR 容器）
~~~

**关键路径 ≈ 20 人日**（M0 → M2-A/B → M3-1 → M3-5 → M4-8 → M5-1 → M5-3 → M6-2 → M6-4）。

**已裁决（D1）：基线 JSON（V1）完成即视为 M0 收口，此后 M3 与 M4-1/M4-5 并行开工。** 依据：两者技术上不互相依赖（MILS 阈值只在 `mixedintegerleastsquares.cpp` 内部；FrameField 多锐边只在 `framefield.cpp:101-110`；CSR 容器在 `base/Csr.h` + 各处的邻接替换），且 M4 的收益需要尽快拿到以验证**验收门本身是否可用**。

并行期的四条硬约束：

| 约束 | 内容 |
| --- | --- |
| P1 | 并行期内**每条 PR 必须跑 G1/G5/G7**(`quad_ratio_pp` / `edges_non_manifold` / `ref_dev_p95`)，并在 artifacts/COMPARISON.md 里留一行（模型、开关、三指标、结论） |
| P2 | M4 若使**任一 P0/P1 指标回归**：先回滚**开关**（--field-brush / 相关默认值），确认不是开关组合问题后再看 M3 |
| P3 | **M5（并行化）严格排在 M3 之后**——它要改的邻接容器正是 M3 交付的，提前并行会让 diff 不可读 |
| P4 | M4-8（抽取图数组化）与 M4-1/M4-5 不是同一批次：M4-8 依赖 M3-5 的 CSR，仍走串行链 |

调整后的并发编排（3 人）：

- A：M3-1 → M3-5 → M4-8 → M5-1 → M5-3（串行链，不可拆）
- B：M3-2 → M3-3 → M3-4（边界脚本与契约，可交错）
- C：**M4-1 与 M4-5 立刻开工** → M4-2/M4-3/M4-4/M4-6 → M4-7/M4-9 → M5-2/M5-4

**瓶颈未变**：A 的链（M3-1 → M3-5 → M4-8 → M5-1 → M5-3）仍是关键路径 ≈20 人日。D1 的收益不是缩短关键路径，而是**把 M4 的质量收益提前 2~3 周拿到**，从而用真实数据校准 G1/G5/G7 的阈值——现在这三个阈值是"按经验定的"，M4-1/M4-5 落地后可以变成"按实测定的"。

**风险**：并行期两条链都会改 artifacts/ 下的基线对照物，容易互相污染。约束：baseline 目录只由 task-4 写，M4 侧只写 artifacts/after/m4-* 子目录。

---

## 10. 附：本次实测证据与待验证清单

### 10.1 本次亲手跑出来的证据

| 证据 | 命令/方法 | 结果 |
| --- | --- | --- |
| CLI 退出码 1 | `autoremesher_cli.exe`（无参数） | exit=1 |
| CLI 退出码 2 | `-i <不存在的文件> -o x.obj` | exit=2 |
| CLI 退出码 0 + 全流程 | 8 顶点立方体，`--target-quads 200` | exit=0；136 quads / 2 non-quads / 141 verts / 0.036 s |
| **phaseReport 被打印两次** | 同上，看 stderr | 同一份约 50 行的阶段报告出现两遍（库一次 + CLI 一次），证实 C1 / §3.4 |
| **Cores kept busy = 1.00** | 同上 | 单岛输入并行度为 1，证实 F12 |
| 日志噪音 | 同上 | stderr 混有 `Extract connections...`、`Searching boundaries...`、`Cleanup triangle faces:2` 等库内输出 |
| 核心零 Qt 依赖 | 对 `src/AutoRemesher/*` 匹配 `#include <Q` | 0 命中（3 条是 `<queue>` 误匹配） |
| 依赖图 | 匹配 `#include <AutoRemesher/` | 见 §1.2，与实测边一致 |
| `Radians` 零引用 | 匹配 `Radians` | 只有 `radians.h` 自身 4 处 |
| `quadremesher.h` 不存在 | `Get-ChildItem src/AutoRemesher/quadremesher.h` | 无此文件，但 `include/AutoRemesher/QuadRemesher` 指向它 |
| 转发头两种深度 | 读 18 个转发头 | 11 个 `../src/`、6 个 `../../src/`、1 个 `../src/` |
| 参考产物计数 | 逐行数 `v `/`f ` | head_quad_20k 14,788/14,730；head_quad_10k 5,357/5,142；body_quad_15k 8,272/8,214 |
| 构建产物 | `Get-ChildItem build/core` | `autoremesher_cli.exe` 958,976 B、`tbb.dll` 187,392 B |
| M1 构建耗时与数值对齐 | `docs/BUILD-CORE.md` §4/§5 | 10.1 s；8,101 quads（−0.34%）；SHA256 一致 |
| `std::cerr` 分布 | 逐文件计数 | quadextractor 41、main_cli 16、autoremesher 11、parameterizer 5、mainwindow 2、main 1 |

### 10.2 引用但非本次直接跑出的数字（出处已标明）

| 数字 | 出处 |
| --- | --- |
| stderr：head10k **3.56 MB**、multi12k **6.94 MB**、scaled10k **3.44 MB**、body15k **57 KB** | `artifacts/*/*/stderr.txt` 实测体积（本次用 Get-Item 复核） |
| 全部 A/B 指标（quads / ratio / nm / bl / comp / dev_p95 / coverage / degenerate / valence4 / wall） | **`artifacts/COMPARISON.md` 主表（§11.1 / §11.2 已全量落表）** |
| head_quad_10k：93.54% / 124 / 70 壳 / 22 退化 / 5 环 / 价4 69.12% | task-2 实测，`tools/metrics/selfcheck/*.json`（F15） |
| head_quad_20k：97.10% / 188 / 1 壳 / 2 退化 / 价4 86.48% | 同上 |
| body_quad_15k：98.97% / 12 / 1 壳 / 0 退化 / 价4 94.96% | 同上 |
| head_in：8,979 壳 / 14,520 环 / 54,014 边界边；head_clean：1 壳 / 4 边界边 / 2 环 | 同上 |
| mesh_metrics 60 条断言、`compare` 退出码 0/1/2、`--samples 50000` 使对比 23.7 s → 7.1 s | task-2 交付说明（F10） |

### 10.3 待验证清单（本文所有"待产出"汇总）

| # | 待验证项 | 阻塞谁 | 归属 |
| --- | --- | --- | --- |
| V1 | ~~baseline 指标 JSON~~ **已完成**（4 用例 × 双跑 + `SUMMARY.md`，见 §11.1） | — | ✅ task-4 |
| V2 | ~~multi12k × `--keep-all-islands`~~ **已完成**：quads +5.63% 但 nm ×6.42、comp +44 → 转默认被阻断（§11.2.2） | — | ✅ verify-runner |
| V3 | ~~scaled10k × `--relative-tolerance`~~ **已完成**：quads ×2.38、coverage +32.62 pp、bbox 误差 11.99%→0.09%（§11.2.3） | — | ✅ verify-runner |
| V4 | n 边形输入 × `--triangulate` 的面集合正确性 | P1-8 | verify-runner |
| V5 | ~~`artifacts/after/` + `COMPARISON.md`~~ **已完成**（80 行主表 + 5 条回归门，§11 全量引用） | — | ✅ verify-runner |
| V6 | CSR 化前后峰值 RSS（head_vox、multi_part） | M4 的 DoD | 待测 |
| V7 | 岛内并行后的 `Cores kept busy` 与 wall | M5 的 G8 | 待测 |
| V8 | P0-3 / P0-4(Qt 路径) / P2-8 / P2-9 的行为验证 | M6 GUI | **需 Qt，本机无法验证** |
| V9 | ~~`--field-brush legacy` vs `always` 的 G1/G5/G7 对比~~ **已回答（§15 / D14）**：`smoothCrossField` 是死代码（恒假分支），强制打开在 A/B 两点都是负收益；`brush` 是承重墙，不可删 | — | ✅ 已完成 |
| V10 | ~~P1-1 修好后 `Rounding cover to integers` 的轮数~~ **已回答**：`plannedRoundCount()` 给出解析解（batch=0.10 → 68 轮，下限 4），§14.3 的调度扫描给出实测；P1-1 线已随 D11/D12 关闭 | — | ✅ 由 §14 回答 |
| V11 | `autoremesher.pro` 与 CMake 源清单的单点化方案 | M6 | 待做 |
| V12 | ~~G4 主壳占比不可计算~~ **已关闭**：`components_largest_face_ratio` 与 `components_small_face_count` 已进 8 份 JSON；G4 三项按 D6 定稿并实算（§11.7） | — | ✅ task-5 |
| V15 | ~~`components_top3_face_ratio` 未实现~~ **已关闭**：metrics 已加 `components_top2/top3_face_ratio`（`mesh_metrics.py:475-476`）并重算 8 份 JSON；G4-b 字段名与 §11.7 表均已切到 top-3 | — | ✅ task-5 |
| V13 | ~~`--relative-tolerance` 缺 1 / 1e3 尺度~~ **已关闭**：1 / 1e-3 / 1e5 三尺度完成（§11.6），并额外抓出 1e5 的 long 溢出 | — | ✅ 已补测 |
| V14 | G8 的 T1~T6 协议尚未完整跑过一次 A/B/A/B/A/B | 性能门目前只有参考值 | 待执行 |
| V16 | ui-go 在**非 Windows** 平台（Linux/macOS）的构建与冒烟 | 跨平台承诺 | task-7 |
| V17 | **schema 版本兼容性**：v1 消费者遇到 v2 事件 / v2 消费者遇到 v1 事件 | 事件契约（§12.3）的长期可用性 | task-7 + CI |
| V18 | 折痕权重在 A/B 两点的一致性 | **已回答（§16 / D16）**：四种权重在 A/B 上收益方向相反，无一能同时通过 | — | ✅ 已完成 |

### 10.4 收尾清单（仅剩这些）

| # | 事项 | 状态 | 归属 |
| --- | --- | --- | --- |
| V4 | `--triangulate` 的 n 边形面集合正确性 | 未做 | verify-runner |
| V6 | CSR 化前后峰值 RSS | 未做 | M4 |
| V7 | 岛内并行后的 `Cores kept busy` 与 wall | 未做 | M5 |
| V8 | Qt 侧行为（P0-3 / P0-4 / P2-8 / P2-9） | **本机不可验** | M6（需 Qt） |
| V9 | ~~`--field-brush legacy` vs `always` 对比~~ **已回答（§15）** | 已完成 | ✅ |
| V10 | P1-1 修好后「Rounding cover to integers」的轮数 | M4 期间 | M4 |
| V11 | `.pro` 与 CMake 源清单单点化（`gen_sources.py`） | 未做 | M3-2 |
| V14 | G8 的 T1~T6 协议完整跑一轮 A/B/A/B/A/B | 未做 | 待执行 |
| V16 | ui-go 非 Windows 构建 + 冒烟 | 未做 | task-7（§12.5） |
| V17 | 事件 schema v1/v2 双向兼容用例 | 未做 | task-7 + CI（§12.3） |

**已关闭**：V1 / V2 / V3 / V5 / V12 / V13 / V15（证据分别在 §11.1、§11.2、§11.6、§11.7）。

**指标可复现性（D7）**：本文 §11 引用的每一份 `metrics.json` 都由 `mesh_metrics.py`（sha256 `5defe0ed…`）在 `--samples 20000` 下产出；`metrics_meta.json` 逐份记录了该 sha、`ref`、`samples` 与 `measured_at`。复核 §11 的数字时先比对 sha——不一致就说明你量的不是这篇文章里的那个量。

**本机环境限制**：无 Qt、无 qmake、无 MinGW/MSYS2。因此 V8 全部条目与 §8.1 的 qmake 侧改动**在当前环境内无法验证**——本文给出改法与判据，不声称已验证。

---

## 11. 验证结果（含 A/B 实测）

> **证据链**（全部为落地产物，不是估算）：`artifacts/COMPARISON.md`（主表 + 结论 + 回归门）｜`artifacts/baseline/SUMMARY.md`｜`artifacts/after/SUMMARY.md`｜`artifacts/baseline/<case>/metrics.json`｜`artifacts/after/<case>/metrics.json`｜原始 `stderr.txt` / `report.txt` / `run.json`
> **工具**：`tools/metrics/mesh_metrics.py`（sha256 `5defe0ed…`）+ `--samples 20000`；基线与 after 两个二进制的 sha256 记录在 COMPARISON.md 头部。
> **口径**：`deviation_p95(rel)` = 输出顶点到输入表面最近距离的 p95，按目标包围盒对角归一化；`coverage` = 参考表面被输出覆盖的比例；`nm` = `edges_non_manifold`；`bl` = `boundary_loops`；`comp` = `components`。

### 11.1 基线（`artifacts/baseline/`，每用例跑 2 次、SHA256 全部一致）

| case | quads | quad_ratio | nm | bl | comp | **largest** | dev_p95(rel) | coverage | wall | exit |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| head10k | 4,720 | 92.91% | **124** | 6 | **70** | 95.22% | 2.192e-3 | 61.11% | 36.34 s | 0 |
| body15k | 9,982 | 99.37% | 8 | 1 | 1 | 100.00% | 8.181e-4 | 95.52% | 12.586 s | 0 |
| multi12k | 6,660 | 92.18% | 160 | 4 | 142 | **47.79%** | 1.843e-3 | 47.03% | 87.329 s | 0 |
| scaled10k | **2,030** | 92.57% | 52 | 2 | 6 | 99.22% | **2.985e-3** | **28.19%** | 33.49 s | 0 |

**scaled10k 那一行就是 P0-2 的犯罪现场**：输出包围盒对角 **0.00117208**，而理想值（head10k 对角 1.33175 的 1/1000）是 **0.00133175**——**小 11.99%**，coverage 只有 28.19%，而 `exit=0`、输出非空。固定 1e-5 绝对焊接把顶点成片塌掉，用户看不出任何异常。

### 11.2 A/B 结果（`artifacts/after/`）

#### 11.2.1 默认路径（无开关）——零回归成立

| case | 与基线关系 | 依据 |
| --- | --- | --- |
| head10k | **逐字节相同** | sha256 `f35b937797976be4394fc669f030ab74d9ddc709e7f9d1cddace2ce5a105eede`；全部指标完全相等（quads 4,720、ratio 92.91%、nm 124、comp 70、dev 2.192e-3） |
| body15k | **逐字节相同** | sha256 `c5325ad47432a8de38d789b5f6f8dccfabd82d4e2c94fd45956e22586a6a0f53`；全部指标完全相等（9,982 / 99.37% / 8 / 1 / 8.181e-4） |

**这不是"指标接近"，是同一字节流**：M1 的抽取 + M2 的开关改造在默认分支上没有引入浮点差异或遍历顺序变化。这条是 G9 的最高形态，也是后面所有 A/B 可信的前提——**差异只能来自开关，不可能来自"顺手改坏"**。

#### 11.2.2 multi12k + `--keep-all-islands`（P0-1）

| 指标 | 基线 | after | 差 |
| --- | --- | --- | --- |
| quads | 6,660 | **7,035** | **+375（+5.63%）** |
| quad_ratio | 92.18% | 91.57% | −0.61 pp |
| nm | 160 | **1,028** | **+868（×6.42）** |
| comp | 142 | **186** | **+44** |
| bl | 4 | 4 | 0 |
| dev_p95(rel) | 1.843e-3 | 1.838e-3 | −0.31% |
| coverage | 47.03% | 46.80% | −0.23 pp |
| valence4 | 65.61% | 62.73% | −2.88 pp |
| degenerate | 60 | 45 | −15 |
| wall | 87.329 s | 83.391 s | −4.51% |

**判定：机制验证通过，但不能转默认。** quads 与 comp 同步上升，符合"保留每个 island 的全部 patch"的语义；但多出来的 patch 以**独立连通块 + 多条 >2 面共享边**的形式出现，**未被焊接**——nm 涨 6.42 倍、valence4 掉 2.88 pp，而 coverage 反而 −0.23 pp：**+375 quads 没有换来覆盖率净收益**。转默认前置门槛见 §6.1 P0-1b（nm ≤160、comp ≤142）。

**一条被推翻的推断**：v4 曾推测"keep-all 让 `removeIsolatedFaces` 变近空操作、跳过 while 里的 `splitToIslands` + `rebuildHalfEdges` + `fixHoles`，所以快 33%"。**83.391 s（−4.51%）推翻了它**——33% 是 58.7 s 那次负载噪声。**"旧 bug 同时是性能黑洞"的说法撤回。**

#### 11.2.3 scaled10k + `--relative-tolerance`（P0-2）

| 指标 | 基线 | after | 差 |
| --- | --- | --- | --- |
| quads | 2,030 | **4,830** | **+2,800（×2.38 / +137.93%）** |
| coverage | 28.19% | **60.80%** | **+32.62 pp** |
| dev_p95(rel) | 2.985e-3 | **2.360e-3** | **−20.95%** |
| bbox 对角误差 | **11.99%**（偏小） | **0.09%** | −11.90 pp |
| quad_ratio | 92.57% | 92.81% | +0.25 pp |
| nm | 52 | 136 | +84（×2.62） |
| bl | 2 | 4 | +2 |
| comp | 6 | 70 | +64 |
| degenerate | 0 | 19 | +19 |
| wall | 33.49 s | 34.825 s | +3.99% |

**判定：P0-2 修复有效，证据充分**——quads ×2.38、coverage +32.62 pp、bbox 尺度误差 11.99% → 0.09%，且 quad_ratio 持平（+0.25 pp）说明不是"用更多面换覆盖率"，而是"把塌掉的几何恢复回真实尺度"。

**为什么 nm / comp / degenerate 上升却是好事**：基线把真实分片塌成 6 个 component / 52 条非流形边——这是"塌陷出来的假整洁"。恢复后是 70 component / 136 条非流形边，即**真实的碎片结构**。按 §5.1 G4 的豁免规则（nm/comp 上升 **但** coverage 上升 **且** dev_p95 下降）判为通过。

### 11.3 计时噪声：本节所有 wall 只作参考

同一份 after scaled10k 二进制在两次测量中给出 **19.1 s** 与 **34.8 s**（差 **82%**），而基线自身两次重复只差 **0.4%**（33.49 / 33.62 s）。COMPARISON.md 里 body15k 的"−17.21%"同理只是噪声（两次基线测量自身就差 9.5%）。

因此：

- **本节表里的 wall 一律不作收益/回归结论**，只作"同一量级"的参考。
- 任何性能判定必须走 §5.2.1 的 T1~T6 协议（串行 / 同负载 / 3 次中位数 / 交替 A/B/A/B / 丢弃首跑 / 报原始值）。
- 这条同时解释了为什么 v4 里"scaled10k −43%"是错的：那是**负载噪声叠加"错误更快"**（塌陷后图更小，后段更快）。真实值是 **+3.99%**。
- **三尺度交叉验证（§11.6）又给了一个反例**：同一 head10k 用例本轮测得 **18.5 ~ 20.6 s**，而 `artifacts/` 里的基线是 **36.3 / 39.8 s**——**差约 2 倍**，全部来自机器并行负载。三个数放在一起就说明：**同一份代码在不同负载下可以差 2 倍**，任何不按 T1~T6 跑出来的 wall 都不构成证据。

### 11.4 回填状态

**已全部由 `artifacts/COMPARISON.md` 回填**（v4 的 §11.4 待填清单至此作废）：after 侧的 `nm` / `components` / `boundary_loops` / `degenerate` / `deviation_p95` / `covered_ratio` 全字段、四个 case 的基线与 after 双份 `metrics.json`、默认路径的两条 sha256，均已在 §11.1 / §11.2 落表。

**仍缺三项（已定位，不影响本文结论）**：

| # | 缺口 | 影响 | 归属 |
| --- | --- | --- | --- |
| V12 | ~~G4 主壳占比不可计算~~ **已关闭**：8 份 JSON 已含 `components_largest_face_ratio` + `components_small_face_count`，G4 三项按 D6 定稿并实算（§11.7） | — | ✅ task-5 |
| V15 | ~~`components_top3_face_ratio` 未实现~~ **已关闭**：metrics 已加 `components_top2/top3_face_ratio`（`mesh_metrics.py:475-476`）并重算 8 份 JSON；G4-b 字段名与 §11.7 表均已切到 top-3 | — | ✅ task-5 |
| V13 | ~~`--relative-tolerance` 只有 1 尺度采证~~ **已关闭**：1 / 1e-3 / 1e5 三尺度完成（§11.6），并额外抓出 1e5 的 long 溢出问题 | — | ✅ 已补测 |
| V14 | G8 的 T1~T6 协议尚未按规程跑过一次完整 `A/B/A/B/A/B` | 性能门目前只有参考值，没有可判定值 | 待执行 |

### 11.5 对验收门的最终校准结果

§11 的数据把三个门从"按经验定"变成"按实测定"：

| 门 | 校准结果 |
| --- | --- |
| **G1**（quad_ratio ≤ −1.0 pp） | 两条真实 A/B 分别 **−0.61 pp** 与 **+0.25 pp**，都在门内 → 阈值合理，不需调整 |
| **G4**（comp / nm / 主壳占比） | 实测暴露了原判据"等于输入连通分量数"的荒谬：multi12k 输入 **17,958 islands**（head10k 是 8,979），输出 comp 142——绝对相等不可能。**最终判据**：三项同时满足 + 两条豁免规则（见 §5.1） |
| **G8**（wall） | **必须配 §5.2.1 协议才有意义**。原始 wall 的噪声 82% 已大于所有待测收益，不配协议的时间门等于没有门 |

**一条更根本的收获**：§11 证明了"指标上升不等于变差"（scaled10k 的 nm/comp 上升是修复），也证明了"指标改善不等于变好"（multi12k 的 quads +5.63% 而 coverage −0.23 pp）。**任何单指标判定都会被骗**——这就是 G1/G4/G7/G8 必须成组使用、且带豁免规则的原因。

---


### 11.6 尺度交叉验证：`--relative-tolerance` 的三尺度结果（关闭 V13）

统一参数：`--target-quads 10000 --sharp-edge 60 --adaptivity 0.5 --anisotropy 1`。输入：`head_in.obj`（scale 1）、`artifacts/inputs/head_scaled_1e-3.obj`（坐标 ×1e-3）、`artifacts/inputs/head_scaled_1e5.obj`（坐标 ×1e5，27.8 MB）。

| 尺度 | 路径 | quads | non-quads | verts | 相对基线 |
| --- | --- | --- | --- | --- | --- |
| 1 | 基线（无开关） | 4,720 | 360 | 5,376 | — |
| 1 | after + `--relative-tolerance` | 4,744 | 357 | 5,349 | **+0.51% quads**，其余指标同量级 → **单位尺度近似中性** |
| 1e-3 | 基线 | 2,030 | 163 | 2,319 | —（已塌陷，bbox 误差 **11.99%**） |
| 1e-3 | after + `--relative-tolerance` | **4,830** | 374 | 5,457 | **×2.38 quads**、coverage **+32.62 pp**、bbox 误差 → **0.09%** |
| 1e5 | 基线 | 4,511 | 324 | 5,190 | —（未崩溃，见结论 2） |
| 1e5 | after + `--relative-tolerance` | 4,527 | 336 | 5,244 | **+0.35% quads** → **大尺度近似中性** |

**结论 1（可转默认的形状，但不是"零影响"）**：开关在单位尺度（+0.51%）与大尺度（+0.35%）都近似中性，只在极小尺度产生质变（×2.38 quads / +32.62 pp coverage / bbox 误差 11.99% → 0.09%）。因此它具备"可安全转默认"的形状——**但要特别注意：单位尺度下仍有 24 个 quads 的差异（4,720 → 4,744），这不是噪声**，是判重粒度相对化后的真实变化。所以"转默认"**不可能**靠 SHA 相同来证明零回归，必须按 D3/D4 的规矩：翻默认前由 task-4 **新建 baseline 目录**并重跑 G1~G13。

**结论 2（1e5 暴露的是 long 溢出，不是量化精度）**：基线在该尺度"侥幸可用"，原因是 32 位 `long` 在 4e9 量级已经溢出，wrap 之后在局部仍可分辨——**这是巧合，不是正确性**。M2 已把 `long` 改为 `long long` 且 `quantize()` 饱和，after 在该尺度只差 +0.35%，即"行为确定"。这条的推论写进 §6.1 P0-2 的影响面：**P0-2 的修复不只针对小尺度，大尺度上它把一个 UB 行为变成了确定行为**——后者更难被用户察觉（基线的输出看起来完全正常）。

**结论 3（计时一律不可比）**：同一 head10k 用例本轮测得 **18.5 ~ 20.6 s**，而 `artifacts/` 里基线是 **36.3 / 39.8 s**——差 2 倍，全部来自机器并行负载，不是代码。这三个数一起放进 §11.3 作为 T1~T6 协议的反例。

**与 V13 的关系**：COMPARISON.md 的回归门第 3 条要求"`--relative-tolerance` 转默认前需补 1e-3 / 1 / 1e3 三尺度交叉用例"。本节覆盖 1 / 1e-3 / 1e5 三个尺度，其中 **1e5 是加做的（原要求是 1e3）**——跨度更大，且正是它抓出了 long 溢出这条。**原清单的 1e3 未单独跑**：1e5 的跨度已覆盖它，若需要 1e3 的精确数字属加做项，不影响闭合结论。**V13 关闭。**

---

### 11.7 G4 三项定稿（D6 / D8）：用真实基线算出的判据（关闭 V12、V15）

8 份 `metrics.json` 现已包含 `components_largest_face_ratio` / `components_small_face_count` / `components_top2_face_ratio` / `components_top3_face_ratio`，工具 sha 逐份记在 `metrics_meta.json`（D7）。按 D6 + D8 实算：

| 用例 | scale_class / parts_class（D8-3） | G4-a small(≤3 面) | G4-b top-3 占比 | G4-c Δlargest | G4 判定 |
| --- | --- | --- | --- | --- | --- |
| head10k | unit / single | 48 → 48（×1.00） | 0.9573 → 0.9573（0 pp） | — | **通过**（三项全过） |
| body15k | unit / single | 0 → 0 | 1.0000 → 1.0000 | — | **通过**（单岛，占比恒 1.0，该项无分辨力） |
| multi12k | unit / **multi** | 100 → **110（×1.10，恰好压线，判通过）** | 0.9355 → **0.8999（−3.56 pp）** | — | **不通过**：top-3 跌破阈值 0.9155 → keep-all 转默认被阻断 |
| scaled10k | **small** / single | 2 → 48（×24）→ **前置失效（D8-1）** | 0.9973 → 0.9583（−3.90 pp）→ **前置失效（D8-1）** | **4.01 pp → 0.11 pp** | **通过**：G4-a/b 不适用，由 G4-c + G1/G5/G7 承担 |

#### D8-1 前置失效条款（scaled10k 是首个适用者）

当**基线本身处于退化态**（判据：bbox 尺度误差 > 5% **或** coverage < 35%；scaled10k 实测 **11.99% / 28.19%**）时：

- **G4-a（small_face_count）与 G4-b（top-k 占比）的计数判据不适用**，改由 **G4-c（两尺度 largest 差 ≤ 0.02）** 与 **G1 / G5 / G7**（quads / non_manifold / deviation_p95）承担判定；
- 该用例上**禁止用「碎片增加」作为回归证据**。

原因：塌陷态的碎片数是被焊掉的**假数字**——scaled10k 基线把 46 个真实小碎片焊没了，所以它的 small=2 不代表干净，只代表看不清。拿这个分母去要求「不超过基线 × 1.10」，等于要求修复之后继续塌着。这不是放宽标准，而是**换一把能测量的尺子**：塌陷态下计数指标的分母没有意义，而 G4-c 不看计数、只看「同一形状在两个尺度下是否被切成不同结构」。

#### D8-2 阈值写法（multi12k 恰好压线）

`small_face_count` 的写法固定为 **≤ 基线 × 1.10（含等号）**。multi12k 是 100 → **110**，比值**恰好 ×1.10**，**判通过**。

这样写是为了消除「< 还是 ≤」的实现歧义：边界值出现一次（multi12k），就必须有唯一解释，否则同一个结果会得出两个结论。

#### D8-3 用例标注（G4-0）

**每个进入 G4 判定的用例必须先标注两列**：

| 列 | 取值 | 影响哪条判据 |
| --- | --- | --- |
| `scale_class` | unit / small / large | 决定 D8-1 的前置失效是否触发（small / large 需看 bbox 误差与 coverage） |
| `parts_class` | single / multi | 决定 G4-b 用 largest（single）还是 top-k（multi） |

**未标注的用例不得进入 G4 判定。** 这条是防下一轮有人拿 multi-part 的 largest 去套单部件阈值——multi12k 的 largest 0.4779 就是现成的反例，套上去会得出「主结构塌了 47%」这种完全错误的结论。

#### 与 P0-1b 的联动（更新到 top-3 口径）

multi12k 的四个信号指向同一件事：**多出来的 patch 是独立碎片、没有焊接**——small ×1.10、top-3 −3.56 pp、nm ×6.42、comp +44。所以 P0-1b / M4-10 的验收直接盯这三个数：

| 指标 | 门槛 | 基线 → after |
| --- | --- | --- |
| `components_small_face_count` | ≤ 100（= 基线） | 100 → 110 |
| `components_top3_face_ratio` | ≥ 0.9155（= 0.9355 − 2 pp） | 0.9355 → 0.8999 |
| `edges_non_manifold` | ≤ 160（= 基线） | 160 → 1,028 |

三项同时达标，才允许 `--keep-all-islands` 转默认。

**阈值的性质**：本节所有数字都是**当前这份基线的快照**。D6/D8 要求的是推导式（≤ 基线、≤ 基线 × 1.10、≥ baseline − 2 pp、两尺度差 ≤ 0.02），基线一换就重算（§5.1 防漂移条款）。

---

## 12. GUI 战略：Qt 退场与 Go 可选前端

> 本章是**增量修订**（D9 / D10），不影响 §1–§11 的编号与结论；§11 的实测数字不因本章改动。
> 一句话：**主链只留 core + CLI 两个可验证的产物，GUI 从"不可构建的债务"降级为"冻结的历史快照"，新前端通过进程边界接入而不是链接进库。**

### 12.1 现状与问题

三条来自 §1.3 的耦合在这里合流——它们本来各自是代码味道，一旦要"换前端"，就变成硬约束：

| 耦合 | 位置 | 对"换前端"的直接影响 |
| --- | --- | --- |
| C2 结果靠裸指针存活 | `mainwindow.h:132-133`、`:175-179` | 任何新前端都必须重写一遍"谁 new 谁 delete"，无法复用 |
| C5 中间产物外泄 | `autoremesher.h:101-159`（15 个 getter） | 库的公开面被某个 UI 的预览需求绑死，换前端就要重新定义对外接口 |
| C7 无头寄生 GUI | `main.cpp:83`、`:220-226` | CLI 路径必须拖上一个 GUI 框架：进程启动 = 初始化 `QApplication` + 请求 OpenGL 3.3 core |

再加两条**硬事实**（不是判断，是实测/文件事实）：

- **H1 本机无 Qt，GUI 完全不可构建。** `docs/BUILD-CORE.md` §6.1 已确认；`CMakeLists.txt:176-178` 的 `AUTOREMESHER_BUILD_GUI=ON` 目前只发一条 warning。
- **H2 发布链路被 Qt 版本与 homebrew 路径绑死。** `autoremesher.pro:1-3`（`QT += core widgets opengl`，Qt6 再加 `openglwidgets`）、`:271-274`（macx 下写死 `/opt/homebrew/opt/tbb/include` 与 `-L/opt/homebrew/opt/tbb/lib`）。

**后果**：P0-3（空指针判源/结果）、P0-4（无头退出码）、P2-8、P2-9 这四条**无法验证**（§10.4 的 V8）——GUI 既不能打包、不能进 CI，也不能被评审确认，成了"不可测的债务仓库"。这是本章的直接动因。

### 12.2 目标形态

| 层 | 产物 | 地位 | 验证方式 |
| --- | --- | --- | --- |
| 主链内核 | `autoremesher_core`（STATIC） | 唯一算法实现 | G1~G13（全部在 core 上判定） |
| 主链接口 | `autoremesher_cli`（EXE，Qt-free） | **唯一稳定对外接口** | G1~G14 |
| 可选前端 | `ui-go` | 进程边界之外，可缺席 | G14 + 冒烟门（V16） |
| 冻结 | `legacy/gui-qt/` | 不参与任何默认流程 | 不验（详见 §12.5） |

~~~text
        ┌──────────────────────────────┐
        │     autoremesher_core        │  STATIC，无 Qt
        │  base geom numeric field     │
        │  cover param extract pipeline│
        └──────────────┬───────────────┘
                       │ 静态链接（实线）
        ┌──────────────▼───────────────┐
        │     autoremesher_cli         │  EXE，Qt-free
        │  --events-json → NDJSON      │  ← 唯一稳定接口
        └──────┬───────────────────────┘
               │ 进程边界 + stdio（虚线，非链接）
        ┌──────▼───────────────────────┐
        │        ui-go (可选)          │  独立二进制，可缺席
        │     本地 Web UI + SSE        │
        └──────────────────────────────┘

  进阶（虚线，本轮不实施）：
        core ⇢ autoremesher_capi（纯 C 头 + opaque handle）⇢ cgo ⇢ ui-go

  legacy（隔离）：
        legacy/gui-qt/  Qt5/6 GUI 冻结；不进默认 target / CI / 发布
~~~

**冻结的确切含义**（六条，缺一条就不算冻结）：

1. 不参与默认 `cmake --build`（`AUTOREMESHER_BUILD_GUI` 保持 OFF，且 OFF 时**不配置**该目录）。
2. 不进 CI（删除/绕过 `.github/workflows/release.yml` 里对 GUI 的三平台打包；该文件的处置见 §8.1）。
3. 不进发布物。
4. 不接 P0~P2 修复——**除非该修复是恢复 GUI 所必需的**（见 §12.6 的恢复步骤）。
5. 不新增功能。
6. 只保留"能编译的历史快照"：源码 + qrc + shaders + 资源 + .pro 原样搬入，**不做重构**。

### 12.3 进程契约：`--events-json` 的 NDJSON schema v1

CLI 新增 `--events-json`：向 stdout（或 `--events-file PATH`）输出**每行一个 JSON 对象**（NDJSON），与人类可读输出并存（后者仍走 stderr，格式不变）。

**公共字段**（四类事件都有）：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `schema` | string | 固定 `"v1"`；升版才变 |
| `type` | string | `progress` / `phase` / `done` / `error` |
| `seq` | int | 单调递增，从 0 起；用于丢帧检测与排序 |
| `ts` | int | 进程启动以来的毫秒数（不是墙钟，避免时区/时钟回拨） |

**四类事件的专属字段**：

| type | 字段 | 含义 |
| --- | --- | --- |
| `progress` | `percent` (float 0..1)、`stage` (string)、`island` (int)、`islands` (int) | 对应现 `ProgressSink`（§3.2）。`stage` 为空串表示"沿用上一个" |
| `phase` | `name`、`order` (float)、`us` (int)、`island` (int，汇总行 -1)、`accumulated` (bool) | 对应 `PhaseEvent`（§7.2）。**行数必须与 `phaseReport()` 一致** |
| `done` | `quads`、`non_quads`、`vertices`、`wall_s` (float)、`exit` (int) | 成功终止。`exit` 与进程退出码同源（§3.1） |
| `error` | `code` (string，取 `StatusCode` 枚举名)、`detail` (string)、`failed_islands`、`total_islands` | 终止但失败。可出现在任意位置，出现即终止 |

**向后兼容规则**（四条，写死）：

1. **只加不减**：新增字段允许，消费者必须忽略未知字段。
2. **删除字段或改变语义必须升 `schema` 到 v2**，且 v1 至少再保留一个大版本。
3. **字段顺序无关**，解析器不得依赖顺序。
4. **未知 `type` 必须容忍**：记日志后丢弃，不得崩溃、不得中断解析（为未来 `warning` / `metric` 类事件留位）。

**降级策略**（开关不存在时）：老二进制没有 `--events-json`（未知参数会以 exit 1 退出，见 §10.1 的实测），ui-go 必须能探测到这一点并退化为**文本进度解析**——从 stderr 解析 `N% done. <status>` 与 phaseReport 行，并在界面标明 `degraded: text`。降级模式不参与 G14 判定。

**三条样例**（真实字段，非示意）：

~~~json
{"schema":"v1","type":"progress","seq":41,"ts":1873,"percent":0.42,"stage":"Solving frame field","island":0,"islands":3}
{"schema":"v1","type":"phase","seq":180,"ts":36120,"name":"Quad extract (accumulated)","order":6.0,"us":12500000,"island":-1,"accumulated":true}
{"schema":"v1","type":"done","seq":241,"ts":36340,"quads":4720,"non_quads":360,"vertices":5376,"wall_s":36.34,"exit":0}
~~~

### 12.4 进阶路径：`autoremesher_capi` + cgo（本轮不实施）

形态（只有骨架，不实现）：

~~~c
/* autoremesher_capi.h —— 纯 C，不出现 C++/STL 类型 */
typedef struct ar_context ar_context;
typedef void (*ar_event_fn)(void* tag, const char* json, size_t len);

ar_context* ar_create(void);
void  ar_set_input_obj(ar_context*, const char* path);
void  ar_set_target_quads(ar_context*, int count);
void  ar_set_events(ar_context*, ar_event_fn, void* tag);
int   ar_run(ar_context*);           /* 0 = Ok，其余见 StatusCode */
void  ar_destroy(ar_context*);
~~~

**触发条件**（满足任一才考虑上 cgo）：

1. **长会话**：一个进程内要反复重网格（例如拖动参数实时预览），每次重启 CLI 都要重建 TBB 线程池 + 重读 OBJ。
2. **多作业并发**：UI 侧需要 N 个作业共享一个 arena / 内存预算。
3. **零拷贝预览**：UI 需要直接读中间网格的顶点缓冲，而不是等 OBJ 落盘再读。

**现在不做的理由**（两条，都是可量化的）：

- **ABI 与生命周期风险**：handle 跨语言、异常不能穿 C 边界、线程模型要固定、`ar_destroy` 与在飞回调的顺序要定义。这些风险换来的是 0 收益，见下条。
- **无收益**：当前 UI 一次只跑一个作业；CLI 进程启动成本 `0.03~0.04 s`（§10.1 实测 cube 全流程 0.036 s）；事件量约 200 条/次（§12.5 的 G14 抽样基数即来自这个量级）。进程边界在这个量级上不构成瓶颈。

### 12.5 迁移与验收

**legacy 迁移清单**（由 task-6 执行，本节只定义范围）：

| 项 | 内容 | 处置 |
| --- | --- | --- |
| GUI 源 | `src/*.cpp|*.h`（47 文件，6,184 非空行） | → `legacy/gui-qt/` |
| Qt 资源 | `resources.qrc`、`resources/`、`shaders/`（9 个 GLSL） | → `legacy/gui-qt/` |
| 图标/元数据 | `autoremesher.rc`、`autoremesher.icns`、`favicon.ico`、`ACKNOWLEDGEMENTS.html` | → `legacy/gui-qt/` |
| 构建入口 | `autoremesher.pro` | → `legacy/gui-qt/`（§8.1 的 qmake 行随之改写） |
| 转发头 | `include/AutoRemesher/*` | **只保留 core 侧**；GUI 专属的 `QuadRemesher` 死头删除（P2-5） |
| thirdparty | `QtAwesome` / `QtWaitingSpinner` | 随 GUI 一起移入 `legacy/gui-qt/thirdparty/` |

**验收门**：

- **G1~G13 不包含 GUI**：它们全部在 `autoremesher_core` / `autoremesher_cli` 上判定（§5.1 的用例集合本就不含 GUI）。
- **ui-go 冒烟门**：`ui-go build` 在 Windows 上通过、能启动、能发起一次作业并展示 `done` 事件（V16 覆盖非 Windows 平台）。
- **新增 G14：CLI 事件契约门**（原文，逐条判定）：

> **G14 CLI 事件契约门**
> 1. 从一次完整运行中**抽样至少 20 条事件**（必须覆盖 `progress` / `phase` / `done` 三类；`error` 由专门用例覆盖），按 schema v1 解析**全部通过**。
> 2. 向事件流**注入一个未知字段**（例如 `"x_future":1`）后，ui-go **不报错、不崩溃**，且能继续消费后续事件。
> 3. `done` 事件**必含** `quads` / `non_quads` / `vertices` / `exit` 四个字段，且类型正确（整数）；
> 4. `phase` 事件的**行数与同次运行 `phaseReport()` 的行数完全一致**（±0，不允许丢行或多行）。
> 5. 任一条件不满足 → **G14 不通过**。

### 12.6 风险与回滚

| 风险 | 说明 | 缓解 / 回滚 |
| --- | --- | --- |
| **Qt 恢复成本** | 恢复步骤要点：① 从 `legacy/gui-qt/` 移回 `src/gui/`；② `.pro` 的 include 路径重新指回；③ CMake 的 `AUTOREMESHER_BUILD_GUI=ON` 实现为真构建（不是 warning）；④ P0-3 / P0-4 / P2-8 / P2-9 的四处修复重做或移植。估 **2~3 人日**，且**必须有一台装有 Qt 的机器** | 冻结 ≠ 删除：源码、qrc、shaders、.pro 全部原样保留；恢复是"搬回来 + 修四处"，不是重写 |
| **Go 前端跨平台打包** | 无 cgo 的好处：单一静态二进制、`GOOS/GOARCH` 交叉编译、**不需要 Qt/TBB 运行时**、不需要装 MSVC | 代价：事件 JSON 解析开销与进程启动开销（§12.4 已量化，可接受） |
| **事件 schema 漂移** | CLI 与 ui-go 各自演进，容易在不升版的情况下改语义 | 四条兼容规则（§12.3）+ G14 的未知字段注入 + V17 的跨版本兼容用例 |

---
## 13. M4 实测（负结果与默认值裁决）

> **本章是负结果记录**。它同时证明了两件相反的事：① M4 的两个算法修补**都不能转默认**；② 验收门（G1/G4-b/G5/G6/G7）第一次实战就拦住了它们 —— 门是有效的。
> 证据路径：`build/probe/abl/{off,round,crease,round_ref,crease_ref,parity2,parity_body}.{json,obj,txt,err}` 与 `artifacts/m4/{head10k,body15k,multi12k}/`。

### 13.1 实验设置

- **用例**：head10k = `head_in.obj`，`--target-quads 10000 --sharp-edge 60 --adaptivity 0.5 --anisotropy 1`
- **四个变体**（每变体独立 build 目录，由 CMake 开关控制）：
  - 基线：两个开关都 OFF（`off.json`）
  - 仅 M4-1：`AUTOREMESHER_M4_PROGRESSIVE_ROUNDING=ON`（`round.json`）
  - 仅 M4-5：`AUTOREMESHER_M4_CREASE_AVERAGE=ON`（`crease.json`）
  - 两者同时（`round_ref.json` / `crease_ref.json` 参考测量）
- **开关位置**：`CMakeLists.txt:63-66`（两个 `option(...)`，默认 OFF），`:168-173` 转成 `target_compile_definitions`

**零回归前提（已验）**：两开关都 OFF 时，head10k 输出 sha = `F35B937797976BE4394FC669F030AB74D9DDC709E7F9D1CDDACE2CE5A105EEDE`，与 §11.2.1 的基线**逐字节相同**；body_vox 输出 8,101 quads、sha `9377D56A…C0427`（`parity2.obj` / `parity_body.obj`），同样逐字节相同。→ **开关框架本身零回归，下表差异只能来自开关。** 这与 §11.2.1 的方法论一致（先用 L1 字节层锁死）。

**计时说明**：四个变体全部落在 **18~22 s**。§11.1 的基线 36.3 s 是当时的机器负载值，**不可与本章比较**（§11.3 已单独说明，噪声可达 2 倍）。

### 13.2 结果

| 变体 | quads | quad_ratio | 非流形边 | 分量 | top3 | small(≤3 面) | 退化面 | dev_p95(rel) | 覆盖率 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 基线 | 4,720 | 0.9291 | 124 | 70 | 0.9573 | 48 | 21 | 0.002192 | 0.6111 |
| 仅 M4-1 逐步取整 | **4,358（−7.7%）** | 0.9294（+0.03 pp） | **84（−32.3%）** | 72 | 0.9544 | 53（+10.4%） | 25 | **0.002281（+4.1%）** | 0.5935（−1.76 pp） |
| 仅 M4-5 折痕平均 | **4,959（+5.1%）** | 0.9321（+0.30 pp） | **136（+9.7%）** | **119（+49）** | **0.9352（−2.21 pp）** | **87（+81%）** | 30 | 0.002169（−1.0%） | 0.5917（−1.94 pp） |
| 两者同时 | 4,607（−2.4%） | 0.9290（−0.01 pp） | 132（+6.5%） | 116（+46） | 0.9318（−2.55 pp） | 85（+77%） | 29 | 0.002169（−1.0%） | 0.5927（−1.84 pp） |
### 13.3 五项判定（严格按门编号：G1 / G4-b / G5 / G6 / G7）

| 变体 | G1 `quad_ratio` | G5 非流形边 | G6 `coverage` | G7 `dev_p95(rel)` | G4-b `top-3` | 结论 |
| --- | --- | --- | --- | --- | --- | --- |
| 仅 M4-1 | ✅ +0.03 pp | ✅ **84（−32.3%）** | ❌ 0.5935（−1.76 pp） | ❌ **0.002281（+4.1%）** | ✅ −0.29 pp | **不通过（G6 / G7）** |
| 仅 M4-5 | ✅ +0.30 pp | ❌ 136（+9.7%） | ❌ 0.5917（−1.94 pp） | ✅ 0.002169（−1.0%） | ❌ **−2.21 pp** | **不通过（G4-b / G5 / G6）** |
| 两者同时 | ✅ −0.01 pp | ❌ 132（+6.5%） | ❌ 0.5927（−1.84 pp） | ✅ 0.002169（−1.0%） | ❌ −2.55 pp | **不通过（G4-b / G5 / G6）** |

**表注**：**G7 用 `dev_p95(rel)`，容差 2%**（§5.1）；G6 是覆盖率 `covered_ratio`，与 G7 分开列，**不得互相替代**；`valence4_ratio` / `irregular_vertices`（G2）**本轮未采**，故不列 —— 不用其它指标替代。

**逐项读法**：

- **M4-1 是本轮唯一的正面信号**：G5 非流形边 124 → **84（−32.3%）**，是五项里唯一显著的改善。但 **G7 `dev_p95` +4.1%**（超 2% 容差一倍多）、**G6 覆盖率 −1.76 pp**、quads **−7.7%** —— **逐步取整让网格变稀、贴合变松**。它确实「取整更谨慎」，但谨慎的代价是把顶点推离了原表面：减少了整数冲突（非流形），同时牺牲了贴合与覆盖。
- **M4-5 把碎片化放大了**：G4-b top-3 从 0.9573 掉到 0.9352（−2.21 pp），分量 70 → **119（+49）**，small 48 → **87（+81%）**，**G5 非流形 +9.7%**。折痕平均不但没改善锐边表达，反而让方向场更碎 —— **它把「面内多条锐边」的矛盾压成一个平均方向，丢掉了锐边本身的信息**。
- **两者同时更差**：G4-b −2.55 pp、G5 +6.5%。两个修补各自的问题叠加，没有互补。
- **三个变体全部挂 G6（覆盖率）**：0.5935 / 0.5917 / 0.5927，无一达到基线的 0.6111。这是本轮最一致的一条负面结论 —— **两个算法修补都以「少覆盖一点表面」为代价**。

### 13.4 D11 裁决：两个开关默认 OFF

**两个开关默认 OFF**（`CMakeLists.txt:63-66` 已改，注释写明原因），但**不删除** —— M4-1b / M4-5b 要继续用它们做 A/B。

三条理由（按权重排序）：

1. **没有任何单项满足全部门。** M4-1 过 G1/G4-b/G5 但挂 G6/G7；M4-5 过 G1/G7 但挂 G4-b/G5/G6；两者同时挂 G4-b/G5/G6。按 §8.4 的回滚触发条件（**G7 > 1.02× 即回滚**），M4-1 必须回滚。
2. **改默认值的成本远高于保留开关的成本。** 留着开关 = 一个布尔 + 两行 CMake；翻默认 = 重建 baseline + 全部 G1~G13 重跑 + §11 全部数字作废。
3. **M4-1 的收益方向值得保留**（非流形 −32% 是真实信号），但它现在绑在一个错误的实现上。删掉开关会丢掉复现这条信号的唯一手段。
### 13.5 新增 backlog（M4-1b / M4-5b）

| ID | 内容 | 人日 | 并行 | 验收目标 |
| --- | --- | --- | --- | --- |
| **M4-1b** | 把「逐轮固定整数变量」改成 **MIQ 真正需要的做法：取整后重新优化 + 覆盖整数约束回代**（当前实现只是把固定时机推迟，没有把整数约束代回连续问题） | **2.5** | **不并行**（改 cover 求解核心） | **保住非流形 −32%（≤84）**，同时 `dev_p95 ≤ 基线 × 1.02`、覆盖率 ≥ 基线（0.6111）、quads 不跌超 1% |
| **M4-5b** | 折痕约束**不能直接算术平均**：改为按 cotan / 边长平方**加权**，并在 **FrameField 内部**与 `applyCurlCorrection` 联立（原前提「先定 --field-brush 顺序」已由 **D14 解除**，见 §15.6） | **1.0** | 是 | `top-3 ≥ baseline − 2 pp`、`small ≤ baseline × 1.10`、非流形不增、覆盖率 ≥ 基线 |

M4-5b 的关键判断在括号里：`quadparameterizer.cpp:680-682` 的**刷树会覆盖 FrameField 的解**（P1-6），所以在刷树前后各做一次平均，等于**用两次独立平均抹掉彼此** —— 方向信息在这个过程中丢失，而不是被加权融合。必须先定 P1-6 的顺序（`--field-brush`），再谈折痕加权。

### 13.6 工程陷阱：CMakeCache 会记住 option 的旧值

**这是本轮实际踩到的坑，不是理论风险。**

- 现象：把 `option(...)` 的默认值从 `ON` 改成 `OFF` 之后，**已有 build 目录里的缓存仍然记着 ON** —— `CMakeCache.txt` 的优先级高于 `option()` 的默认值。
- 后果：**改了默认值，但测的还是旧行为。** 本轮就是这么被骗了一次：`build/core` 给出 **7,954 quads**，而删缓存 / 显式传参后回到 **8,101 quads**。
- 正确做法（已写进 §5.2 作为 **T7**）：A/B 必须**显式传 `-D`** 给每个开关，或用**全新 build 目录**；并且 `run.json` 必须记录 `CMakeCache.txt` 里该开关的**实际取值**，而不是命令行意图。

这条与 §5.2.1 的 T1~T6 同类：**测量必须记录「实际生效的配置」，而不是「我以为生效的配置」**。

**开关之外，env 旋钮同样要记。** M4-1b 引入了 `AUTOREMESHER_MILS_BAND` / `_BATCH` / `_DEBUG`（§14.1），它们**不经过 CMake**，因此 `CMakeCache.txt` 里看不到——`run.json` 必须显式记录这些环境变量的取值，否则同一份二进制 + 同一份缓存仍可复现不出同一次运行。**T7 的适用范围由此从「CMake 开关」扩展到「一切影响数值行为的输入」**：CMake 开关、env 变量、CLI 参数、输入文件。

#### 13.6.1 附：事件契约（G14）初检结果

事件流已经能跑通，G14 的 **第 1 / 3 / 4 条已可自动判定**，第 2 条（未知字段注入）等 ui-go 冒烟时一并验证。

| 项 | 实测（cube 用例） | 对应 G14 条款 |
| --- | --- | --- |
| 事件总量与分布 | **62 条 `progress` + 1 条 `phase` + 1 条 `done`**（共 64 条） | 第 1 条（抽样 ≥20 且覆盖三类）✅ |
| `schema_version` | **1** | 第 1 条（按 v1 解析）✅ |
| `done` 内容 | `quads=164` / `non_quads=0` / `vertices=166` / `exit=0` | 第 3 条（四字段齐全、类型正确）✅ |
| `phase` 行数 | **55 行**，与同次运行 `phaseReport()` 一致 | 第 4 条（±0）✅ |
| 未知字段注入 | 未做 | 第 2 条 ⏳ 等 ui-go 冒烟 |

**一个必须记住的细节**：该用例 stderr 共 **136 行**，其中 **72 行是库自身的 `std::cerr`**（`Extract connections...`、`Searching boundaries...` 等，见 P2-3）——它们**不是 JSON**。ui-go 必须把这 72 行当 **log** 处理（原样转发到日志面板），**不能**混入事件流解析。这也再次说明 §3.4 的「库内零输出」不是洁癖：只要 stderr 里混着非结构化文本，任何消费方都要额外写一层过滤。

**结论**：G14 不再是一个「设计中的门」，它已经有可复现的通过证据（第 1/3/4 条）。等第 2 条补上，G14 即可转入常规 CI 门禁。

### 13.7 这个负结果对计划的意义

1. **验收门通过了第一次实战检验。** G5 的 2% 容差把 M4-1 的 +4.1% 拦住了 —— 如果容差是 5%，这个负结果会被当成正面结果收下。**阈值定得紧是对的。**
2. **G4-b 的 top-k 口径第一次抓到真回归**：top-3 −2.21 pp 配合分量 +49、small +39，三个信号同向；而 largest 在这个用例上根本看不出问题（0.9522 与 0.9544 都在「正常」范围内）。D6/D8 改口径的价值在这一轮得到验证。
3. **「修 bug」不等于「指标改善」。** M4-1 修的是 §6.2 P1-1 里 `max` / `min` 那个错误，逻辑上毫无疑问是改对了，但端到端指标反而更差。**算法正确性与全局质量不是同一件事** —— 这正是 §5.1 要求「成组判据 + 豁免规则」的原因。
4. **M4-1 的 −32% 非流形仍是真实收益**，只是当前实现把代价放在了 `dev_p95` 上。M4-1b 的方向是把这笔收益「取回来」，而不是放弃它。

**V9 不受本章影响**：`--field-brush legacy|always` 的 A/B **仍未跑**（§10.3 的 V9），而它是 M4-5b 的前置条件之一（见 §13.5）。

---
## 14. M4-1b：取整调度参数化、自适应迭代上限与否决

> **本章同样是负结果**，但它比 §13 更有价值：它把「为什么修不好」定位到了具体的一行自由度缺失，并且顺手交付了一个**独立的健壮性修复**（自适应迭代上限）。
> 证据：`src/AutoRemesher/mixedintegerleastsquares.{h,cpp}`、`src/AutoRemesher/quadparameterizer.cpp:590-613`，以及本章的调度扫描表。

### 14.1 已落地的三件事（默认 OFF，零回归已验）

| # | 内容 | 位置 | 默认行为 |
| --- | --- | --- | --- |
| 1 | **MILS 调度参数化**：`setRoundingSchedule(band, batchFraction)`；env 覆盖 `AUTOREMESHER_MILS_BAND` / `_BATCH` / `_DEBUG`（只在实验时读，未设置时行为不变） | `mixedintegerleastsquares.h:66`、`.cpp:79-85`；env 在 `.cpp:42/51/75-76` | class 默认 `band=0.0`、`batch=0.10`（`.h:139-140`），与历史行为一致 |
| 2 | **新增 `plannedRoundCount()`**：由 batch 推导所需轮数 = `ceil(log(1e-3)/log(1-batch)) + 2`（下限 4） | `mixedintegerleastsquares.cpp:87-96` | `batch=0.10` → **68 轮** |
| 3 | **迭代上限自适应**：`quadparameterizer.cpp` 的 100 轮硬上限改为 `min(512, max(100, planned + 8))` | `quadparameterizer.cpp:597-600` | 默认路径 `planned=68` → `max(100, 76) = 100`，**仍是 100，行为不变** |

**可复现性细节（本轮实测）**：四次 sweep 的 `--mils-round` 调试记录分别是 **11,242 / 11,287 / 11,033 / 11,178 条**。这个量级说明一件关键的事：

> **head_in.obj 被拆成数千个岛，每个岛独立跑一次 MILS**（每次 MILS 只有几十轮，几千个岛 × 几十轮 = 万级记录）。

这直接解释了 §14.2 的悬崖为什么先在小岛上出现：`batch=0.05` 需要 ~135 轮才收敛（`plannedRoundCount` 的解析解），而**小岛本来就只有几十个整数变量**，100 轮上限在它们身上更早触发 —— 一旦触发，那个岛被整块丢弃，最终表现为「quads 从 2,453 掉到 192」。

这条同时也与 §0.3 的输入统计**对上了**：`head_in.obj` 有 **8,979 个边连通壳**、14,520 个边界环（§0.3 表），正是这几千个岛的来源。两条证据（调试行数量级 + 输入壳数）互相印证，可以放心用来解释悬崖机制。

**零回归验证**：默认路径下 body_vox 输出 **8,101 quads / sha `9377D56A…C0427`**，与 §11.1 基线**逐字节相同** → 三件事在默认配置下没有改变任何数值行为（`planned=68 < 100` 是关键：自适应只在「更细的调度」下才生效）。

### 14.2 关键发现 A：100 轮上限是「静默丢岛」的悬崖

`batch=0.05` + **旧 100 轮上限** → 4,000 目标下：quads **192**、覆盖率 **0.0116**、quad 占比 **0.7967**（近乎失效）。
把上限改成自适应后、同一配置 → quads **2,103**、覆盖率 **0.3583**。

**机制**：`quadparameterizer.cpp:613` 是 `return s.converged()`；轮次用尽而 `converged()` 为假 → **参数化失败 → 整岛丢弃**（`generate()` 里 catch 之外没有回退路径，见 §6.2 P1-4 与 §3.1 的 `failedIslands` 设计）。所以：

> **换更细的取整调度时，迭代上限必须同步；否则失败是完全静默的** —— 没有异常、没有 stderr、输出只是「突然变少」，而 `exit=0`。

这正是 §14.1 第 3 件事值得**独立保留**的理由：它不是算法修补，是**让任何调度都不会再因上限静默丢岛**的健壮性修复。

### 14.3 调度扫描（`head_in.obj`，target 4,000，ref 自身，`--samples 20000`）

| 配置 | quads | ratio | nm | top3 | dev_p95 | cov |
| --- | --- | --- | --- | --- | --- | --- |
| 基线（M4-1 关） | **2,453** | 0.9095 | **64** | **0.9177** | **0.003863** | **0.3728** |
| band0 / batch0.10 | 2,056 | 0.9170 | 72 | 0.9041 | 0.004159 | 0.3657 |
| band0.02 / batch0.10（上一轮 M4-1 配置） | 2,121 | 0.9194 | 76 | 0.9059 | 0.003727 | 0.3736 |
| band0 / batch0.20 | 2,045 | 0.9175 | 84 | 0.9031 | 0.004143 | 0.3703 |
| band0 / batch0.05（自适应上限） | 2,103 | 0.9167 | 76 | 0.9063 | 0.003975 | 0.3583 |

**读法**：

- **coverage 全线下滑**（0.3728 → 0.3583~0.3736，除了 band0.02 勉强持平 +0.08pp）。这是本轮**最一致**的信号，也是根因所在（§14.5）。
- **quads 全线下降**（−14%~−17%）：取整更细致，反而产出更少的面。
- **quad_ratio 全线上升**（+0.72~+0.99 pp）：纯粹因为分母（总面数）掉得更快——**这不是改善，是变小**。这正是 §11.5 说的「单指标会被骗」。

  > **所以：M4-1 的 quad_ratio「改善」不可作为收益引用。** 它的分母（总面数）同时下降了 14~17%，ratio 上升只是**分母效应的副产品**。任何人拿「ratio 从 0.9095 升到 0.9194」当正面结论，都是在读一个被缩放过的分数。
- **nm 全线变差**（64 → 72/76/84）：与上一轮 target 10,000 的「nm −32%」**方向相反**（§14.4）。
- **唯一的例外**：band0.02 的 dev_p95 0.003727 **优于**基线 0.003863（−3.5%），说明 band 参数确实能压偏差——但它压不住覆盖率。

### 14.4 关键发现 B：上一轮的「nm −32%」不可复现，收益是操作点相关的

同一份 progressive 代码，换目标密度：

| 操作点 | quads | ratio | nm | dev_p95 | cov |
| --- | --- | --- | --- | --- | --- |
| target 10,000（上一轮 §13.3 的 M4-1） | 4,358 | 0.9294 | **124 → 84（−32%）** | +4.1% | −1.76 pp |
| target 10,000 + batch0.05（自适应上限） | 4,504 | 0.9389（+0.98 pp） | **112（−10%）** | +4.6% | −1.25 pp |
| target 4,000 | — | 0.9167~0.9194 | **64 → 72/76/84（变差）** | +3~8% | −1~4 pp |

**同一算法在不同目标密度下 nm 方向相反**。结论：

> M4-1 不是「修好了一个 bug」，而是**把解推到了另一个操作点**。收益（nm）与代价（dev / coverage）都随目标密度变化，没有普适的改善 —— 这也是为什么它在 §13 里只能拿 1 项通过、3 项不通过。

这一条同时纠正了 §13.7 里那句「M4-1 的 −32% 仍是真实收益」：**它真实，但只在 target=10,000 这一个操作点上真实**。

### 14.5 根因：t 与「尺度/保角缩放」没有联合优化

真正的 MIQ 里，整数平移 `t` 与参数化的**尺度 / 保角缩放**是**联合优化**的：`t` 的取整误差本可以由缩放吸收。我们的实现把 `t` **硬固定**之后再解 UV，等于**去掉了「用缩放吸收取整误差」的自由度**。

所以取整越细：
- 整数冲突（非流形）在 target 10,000 上确实减少（有自由度被释放），
- 但 UV 解失去吸收误差的能力 → **dev 变大、覆盖率下降**（本章扫描里 coverage 全线下滑就是这条的直接后果）。

**这不是调 band / batch 能解决的**——M4-1b 的最终结论就是：**参数化扫描已到边界**。

### 14.6 D12 裁决

1. **M4-1b 关闭为「已实现、已测量、被否决」**；`AUTOREMESHER_M4_PROGRESSIVE_ROUNDING` 默认保持 **OFF**。
2. **保留自适应迭代上限**（`quadparameterizer.cpp:597-600`）——它是独立于算法的健壮性修复，任何调度都不再因上限静默丢岛。**默认路径数值不变**（`planned=68 < 100`）。
3. **保留** band/batch env 旋钮与 `plannedRoundCount()` 作为**诊断工具**，写进 §7.2 的可观测性清单（§7.2 已补）。
4. **M4-1 线不再排期**；M4-1c 仅在「有人愿意投入 4~6 人日做尺度耦合」时启动，否则从 backlog 移除。

### 14.7 M4-1c 提案（不排期，4~6 人日）

| 项 | 内容 |
| --- | --- |
| 动作 | 在 `solveQuadCover` 里加入**每面 log 尺度自由度**（与 `applyCurlCorrection` 相同的 3F 未知数）；把 `t` 的固定做成**软约束 + 最终取整**，再**重解一次 UV** |
| 依赖 | 与 M4-5b 共享「`applyCurlCorrection` 联立」的机制（§13.5）；两者应同批设计，否则会各改一遍 |
| 验收 | **两点矩阵（D13）**：两个操作点**同时**满足各自的 nm / dev / cov / top3 才算通过；**不接受单点基线**。矩阵见本节末 |
| 风险 | 未知数从 4C 涨到 4C+3F（近似再翻一倍），`solveQuadCover` 的规模与时间会显著上升；若 G8 不过，需要先做 M5 的岛内并行 |
| 排期 | **未排期**。启动条件：有人愿意投 4~6 人日，且 M5 已交付（否则时间门必挂） |

#### 14.7.1 M4-1c 验收矩阵（D13）

两个操作点，**每点的阈值由该点自己的基线推导**（§11.1）：

| 点 | 用例与参数 | 该点基线 | 通过条件 |
| --- | --- | --- | --- |
| **A** | `head_in.obj`，`--target-quads 10000 --sharp-edge 60 --adaptivity 0.5 --anisotropy 1` | quads 4,720 / ratio 0.9291 / **nm 124** / **top3 0.9573** / **dev 0.002192** / **cov 0.6111** | nm ≤ 124、dev ≤ 0.002192×1.02、cov ≥ 0.6111、top3 ≥ 0.9373（=0.9573−2pp） |
| **B** | `body_clean.obj`，`--target-quads 15000 --sharp-edge 60 --adaptivity 0.3 --anisotropy 1` | quads 9,982 / ratio 0.9937 / **nm 8** / **top3 1.0000** / **dev 8.181e-4** / **cov 0.9552** | nm ≤ 8、dev ≤ 8.181e-4×1.02、cov ≥ 0.9552、top3 ≥ 0.9800（=1.0000−2pp） |

**判定规则**：

1. **两点必须同时满足**上表各自的四项条件，才算 M4-1c 通过。
2. **只在一个点上通过 = 不通过。** 依据就是 §14.4：同一份 progressive 代码在 target 10000 上 nm −32%、在 target 4000 上 nm +12.5%~+31%（64→72/76/84），**方向会反转**。
3. **不把操作点固定成单点**：固定单点等于鼓励下一轮继续「挑点验收益」。本轮已经用 4,000 / 10,000 两个点证明了挑点会把结论翻过来，规则必须堵住这条路。
4. 点 B 的意义在于它是**单岛、高 quad 占比（99.37%）、几乎无碎片（nm 8、top3 1.0）**的干净用例 —— 与点 A 的「多岛、碎片多」形成对照。任何只在碎模型上成立的收益，在点 B 上会当场露馅。

### 14.8 本章对计划的意义

1. **「参数扫描」是一种有效的失败方式**：M4-1b 花了 0.5 人日就证明了「这条路走到头了」，比再做两轮微调便宜得多。
2. **两个「顺手」的产出比主结论更有价值**：自适应上限（真修复）+ `plannedRoundCount` 诊断（可复用）。**负结果里也要把边角料收干净。**
3. **操作点相关性必须写进文档**：同一算法在不同 target 下 nm 方向相反 —— 任何「某指标改善 X%」的结论都必须附操作点，否则不可复核。这条与 §5.2 的「测量必须记录实际配置」是同一条原则的两个面。

---
## 15. V9 场算法归属：FrameField + brush 是有效链路，smoothCrossField 是死代码

> 本章回答 §6.2 P1-6 悬了很久的问题：**quadparameterizer 里那两套场算法，到底哪一套在工作。** 答案比「两套并列、需要选一个」更干净 —— **一套在用，一套已死**。
> 证据归档：`artifacts/v9/`（`A_legacy.txt` / `A_always.txt` / `A_always_m.json` / `B_legacy.txt` / `B_always.txt` / `B_always_m.json` / `A_off_stdout.txt`）。

### 15.1 实验设置

- **开关**：`quadparameterizer.cpp:60` 读 `AUTOREMESHER_FIELD_BRUSH`，取值 `legacy | off | always`，**默认 legacy**；每进程打印一行 `Field brush mode: ...`（`:740`）。`off` 模式额外打印 WARNING（诊断用，结果不受支持）。
- **零回归验证**：默认 `legacy` 下 body_clean @15,000 = **9,982 quads / 63 non-quads / sha `C5325AD4…0F53`**，与 §11.2.1 基线**逐字节相同**。
- **两个操作点**（沿用 §14.7.1 的 D13 矩阵，`--samples 20000`）：
  - 点 A：`head_in.obj` @ `--target-quads 10000 --sharp-edge 60 --adaptivity 0.5 --anisotropy 1`，ref = head_in
  - 点 B：`body_clean.obj` @ `--target-quads 15000 --sharp-edge 60 --adaptivity 0.3 --anisotropy 1`，ref = body_clean

### 15.2 点 A（head_in @10,000）

| mode | quads | ratio | nm | comp | top3 | dev_p95 | cov | wall |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **legacy（基线）** | 4,720 | 0.9291 | **124** | 70 | 0.9573 | **2.192e-3** | **0.6111** | ~18 s |
| always（强制平滑 + brush） | **5,491（+16.3%）** | 0.9318 | **136（+9.7%）** | 74 | 0.9618 | **2.275e-3**（+3.8%） | 0.5952（−1.59 pp） | 22~30 s |
| off（无平滑无 brush） | **无有效输出** —— 见 §15.4 第 2 条 | — | — | — | — | — | — | CPU 134 s / 216 s 仍无结果 |

### 15.3 点 B（body_clean @15,000）

| mode | quads | ratio | nm | comp | top3 | dev_p95 | cov | wall |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **legacy（基线）** | 9,982 | 0.9937 | **8** | 1 | 1.0000 | **8.181e-4** | 0.9552 | 10.7 s |
| always | 10,031（+0.5%） | 0.9904（−0.33 pp） | **16（翻倍）** | 1 | 1.0000 | **8.170e-4**（−0.13%） | 0.9587（+0.35 pp） | 12.7 s |

**数字口径**：`dev_p95(rel)` 一律用**全精度科学计数法**（点 B 基线 `8.181e-4` 取自 `artifacts/baseline/body15k/metrics.json` 的 `ref.deviation.p95_rel` = 0.0008180749…；早前出现的 `0.000818` 是六位小数截断，**不是另一次实验**）。同理 `2.192e-3` / `2.275e-3` / `8.170e-4` 均为全精度。

**两个点同向的结论**：`always` 在 A 点把 nm 推高 9.7%、偏差 +3.8%、覆盖 −1.59 pp（同时多出 16.3% 的面）；在 B 点直接把非流形边**翻倍**（8→16）、ratio 掉 0.33 pp、覆盖率只换来 +0.35 pp。**它不是「被埋没的更便宜方案」，而是纯粹的负收益。**

### 15.4 四条判定（死代码 / 承重墙 / 唯一链路 / 运行时指纹）

**① `smoothCrossField` 是死代码。** `Parameterizer` 永远传 non-null `guidance`（`quadparameterizer.cpp:680`），所以调用点条件 `if (!(guidance && ...))` **恒假** —— 它在默认路径上从来没有执行过。V9 用 `always` 强制打开它，两个点都不是收益（A：nm +9.7% / dev +3.8% / cov −1.59 pp；B：nm 翻倍 / ratio −0.33 pp / cov 仅 +0.35 pp）。**结论：删掉它不会损失任何东西。**

**② `brushFieldAlongSpanningTree` 是承重墙。** 关掉它（`off`）后，每个面的 rotation 不再沿生成树保持一致，**seam 集合爆炸，cover 求解进入病态**：两次尝试的 CPU 时间分别是 **134 s / 216 s** 仍无输出（legacy 只要 18 s）。有一次勉强跑完，产物状态自相矛盾（report 有数但 obj 缺失），按「不采信」处理。

> 所以 §6.2 P1-6 里「brush 覆盖了 FrameField 的结果」这个方向是对的，但**不能靠删 brush 来消解** —— 删了整条链路就塌。

**③ 当前有效链路是唯一的**：

~~~text
FrameField（全局 LS 求场）  ─→  brushFieldAlongSpanningTree（生成树对齐）  ─→  applyCurlCorrection（尺度/旋度修正）
       有效，在用                  有效，承重墙                            有效，在用

smoothCrossField  ✗  在本流水线里从未执行（恒假分支）
~~~

**`smoothCrossField` 与 `brush` 不是「两套并列算法」，而是「一套在用 + 一套已死」。** 这是 P1-6 需要的最终答案。

**④ brush 的可观测性要留证据。** 每次运行都会打印 `Field brush mode: legacy`（`quadparameterizer.cpp:740`）——这一行就是**链路完整性**的运行时指纹。若哪天它消失了、或值变了，说明有人动了这条链路；**比事后 diff 几何快得多**，也比读代码可靠。所以它进 §7.2 的必记清单（并入 T7）。

### 15.5 D14 裁决

1. **`smoothCrossField` 标记为 dead code**：代码里已加注释写明 V9 的数字与「不要打开」（`quadparameterizer.cpp:172-178`）；**M3 搬目录时删除**，不进任何默认路径。**删除协议见 D15 / §15.8**：不用 `#if 0`，函数签名缩成 3~4 行墓碑（含 V9 数字与本节索引），实现原文整段移入 §15.8 并存档 commit sha。
2. **`brush` 保留**：`autoremesher` 不许删 brush；`off` 仅作诊断用途，且已加 WARNING。
3. **M4-5b 解除阻塞**：V9 证明 brush **只做对齐、不做融合**，所以折痕加权应当在 **FrameField 内部**完成、brush 原样保留。原计划「先定 `--field-brush` 顺序再谈折痕」这条前置**取消**，M4-5b 从 1.5 人日下调为 **1.0 人日**（少一步验证）；验收按 **D13 两点矩阵**。
4. **关闭 V9**；§10.3 的 V9 标为「已回答（§15）」。

### 15.6 对 M4-5b 的具体影响

| 项 | 原计划（§13.5） | D14 之后 |
| --- | --- | --- |
| 前置 | 先跑 V9 定 `--field-brush` 顺序 | **取消**（V9 已证明 brush 不做融合，顺序不是变量） |
| 改动位置 | 折痕约束 + 与 `applyCurlCorrection` 联立 | **不变**，但明确在 FrameField 内部做加权（brush 原样保留） |
| 人日 | 1.5 | **1.0** |
| 验收 | top-3 ≥ baseline−2pp、small ≤ baseline×1.10、nm 不增、cov ≥ 基线 | **D13 两点矩阵**（A/B 两点同时满足） |

### 15.7 本章对计划的意义

1. **「有没有在跑」比「哪个更好」更该先问。** P1-6 写了很久，一直默认两套并列、需要调参选优；V9 一问就发现其中一套**从未执行**。这是最便宜的一类诊断。
2. **死代码会持续产生维护成本**：`smoothCrossField` 有 60 行、有调用点、有注释，任何人读 `quadparameterizer.cpp:680` 都会以为它是一条可选路径。**注释里写明实测数字**（`quadparameterizer.cpp:174-175` 已经这么做了）比删掉更保险 —— 删掉的代码会被重新发明，带数字的注释不会。
3. **承重墙与死代码的特征完全不同**：死代码删了没事，承重墙删了整条链路病态到「CPU 216 s 无输出」。判断方法就是**关掉它看会不会塌** —— 这也正是 V9 的做法。

---
### 15.8 附录：`smoothCrossField` 原文（M3 删除时迁入，勿用 `#if 0`）

**删除协议（D14 第 1 条的实施细则，D15）**：

1. **不用 `#if 0`。** 不要留注释块式的死代码——它会在每次 `grep`、每次重构、每次编辑器折叠里继续产生成本。
2. 删除时把函数**签名**保留成 3~4 行的墓碑（tombstone），其余实现整段移入本节（下面的 fenced 块已是原文，可直接保留）。
3. 墓碑文字（M3 照抄）：

~~~cpp
// REMOVED (M3): smoothCrossField. Dead in the shipping pipeline -- the call site
// only ran it when guidance was null, and Parameterizer always passes a solved
// FrameField. V9 measured forcing it on: head_in @10k nm 124->136, dev +3.8%,
// cov -1.59pp; body_clean @15k nm 8->16. Reason and full source: docs/
// ARCHITECTURE-REFACTOR-PLAN.md 15.8 (D14).
~~~

4. 删完后本节标题旁加一行「已迁移于 commit <sha>」，让读者能确认这段原文与仓库状态对齐。

**原文快照**（`src/AutoRemesher/quadparameterizer.cpp:172-238`，2026-11-10 工作树状态，含上方注释块）：

~~~cpp
    // Dead in the shipping pipeline: Parameterizer always passes a solved
    // FrameField as guidance, and the call site below only runs this when
    // guidance is absent.  V9 measured what happens if it is forced on
    // (AUTOREMESHER_FIELD_BRUSH=always): head_in @10k nm 124 -> 136,
    // deviation +3.8%, coverage -1.59 pp; body_clean @15k nm 8 -> 16.
    // So it is not a cheaper substitute for FrameField, and it stays dead
    // until someone has a reason and a two point A/B to back it.
    void smoothCrossField(const SurfaceMesh& mesh, const std::vector<Vector3>& normals,
        double hardEdgeDegrees, std::vector<Vector3>* field)
    {
        std::vector<double> alpha(2 * mesh.faceCount(), 0.0);
        std::vector<char> locked(mesh.faceCount(), 0);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, mesh.faceCount()), [&](const tbb::blocked_range<size_t>& range) {
            for (size_t faceIndex = range.begin(); faceIndex != range.end(); ++faceIndex) {
                alpha[2 * faceIndex] = 1.0;
                for (size_t localCorner = 0; localCorner < 3; ++localCorner) {
                    const size_t cornerIndex = 3 * faceIndex + localCorner, oppositeCornerIndex = mesh.oppositeCorner(cornerIndex);
                    if (oppositeCornerIndex != SurfaceMesh::npos && std::fabs(mesh.normalAngle(cornerIndex)) * 180.0 / M_PI < hardEdgeDegrees)
                        continue;
                    const Vector3 edge = unit(mesh.edgeVector(cornerIndex), Vector3(1, 0, 0));
                    const Vector3 fieldDirection = (*field)[faceIndex];
                    const Vector3 perpendicular = unit(Vector3::crossProduct(normals[faceIndex], fieldDirection), Vector3(0, 1, 0));
                    const double fieldAngle = std::atan2(Vector3::dotProduct(edge, perpendicular), Vector3::dotProduct(edge, fieldDirection));
                    alpha[2 * faceIndex] = std::cos(4.0 * fieldAngle);
                    alpha[2 * faceIndex + 1] = std::sin(4.0 * fieldAngle);
                    locked[faceIndex] = 1;
                }
            }
        });
        for (int iteration = 0; iteration < 40; ++iteration) {
            std::vector<double> next = alpha;
            tbb::parallel_for(tbb::blocked_range<size_t>(0, mesh.faceCount()), [&](const tbb::blocked_range<size_t>& range) {
                for (size_t f = range.begin(); f != range.end(); ++f)
                    if (!locked[f]) {
                        double x = alpha[2 * f], y = alpha[2 * f + 1];
                        const Vector3 bf = (*field)[f];
                        const Vector3 btf = unit(Vector3::crossProduct(normals[f], bf), Vector3(0, 1, 0));
                        for (size_t l = 0; l < 3; ++l) {
                            const size_t oc = mesh.oppositeCorner(3 * f + l);
                            if (oc == SurfaceMesh::npos)
                                continue;
                            const size_t g = mesh.cornerFace(oc);
                            Vector3 bg = (*field)[g];
                            bg = unit(bg - normals[f] * Vector3::dotProduct(bg, normals[f]), bf);
                            const double d = std::atan2(Vector3::dotProduct(bg, btf), Vector3::dotProduct(bg, bf));
                            const double cs = std::cos(4.0 * d), sn = std::sin(4.0 * d);
                            x += cs * alpha[2 * g] - sn * alpha[2 * g + 1];
                            y += sn * alpha[2 * g] + cs * alpha[2 * g + 1];
                        }
                        const double length = std::hypot(x, y);
                        if (length > 1e-12) {
                            next[2 * f] = x / length;
                            next[2 * f + 1] = y / length;
                        }
                    }
            });
            alpha.swap(next);
        }
        tbb::parallel_for(tbb::blocked_range<size_t>(0, mesh.faceCount()), [&](const tbb::blocked_range<size_t>& range) {
            for (size_t faceIndex = range.begin(); faceIndex != range.end(); ++faceIndex) {
                const double fieldAngle = .25 * std::atan2(alpha[2 * faceIndex + 1], alpha[2 * faceIndex]);
                const Vector3 fieldDirection = (*field)[faceIndex];
                const Vector3 perpendicular = unit(Vector3::crossProduct(normals[faceIndex], fieldDirection), Vector3(0, 1, 0));
                (*field)[faceIndex] = unit(fieldDirection * std::cos(fieldAngle) + perpendicular * std::sin(fieldAngle), fieldDirection);
            }
        });
    }
~~~

**为什么把原文放文档而不是留代码**：`smoothCrossField` 有 60 行、有调用点、有注释，任何人读调用点都会以为它是一条可选路径。留在仓库里的成本是**持续的误读成本**；而完全删掉又会让「它到底做过什么」不可查。折中就是——**代码里只留墓碑 + 文档里留原文**：索引与可复原性都有了，误读成本归零。

---
## 16. M4-5b：折痕权重扫描与两点否决

> **第三个连续的负结果**（§13 M4 / §14 M4-1b / §16 M4-5b），但它比前两个更接近根因：本章把「为什么折痕加权在这里不起作用」定位到了 `applyCurlCorrection` 的**重新优化**上，并给出了 M4-5c 的修法方向。
> 证据归档：`artifacts/m4-5b/`（A 四组 + B 两组：`{A,B}_{length,invlen2,cotan,cotanlen}{.txt,_m.json}` + `parity.txt`）。

### 16.1 实现与零回归

- **权重模式**：`framefield.cpp:67-90` 的 `CreaseWeightMode` + `creaseWeightModeFromEnvironment()`；四种取值 `length`（默认 = M4-5 行为）| `invlen2` | `cotan` | `cotanlen`，由 env `AUTOREMESHER_CREASE_WEIGHT` 选择（`:77`）。实际权重在 `creaseWeight()`（`:107-126`），在折痕累积处调用（`:202`）。
- **总控仍在 CMake**：`AUTOREMESHER_M4_CREASE_AVERAGE`（默认 **OFF**）——env 只在开关打开时才有意义。
- **零回归**：默认路径 body_vox = **8,101 quads / sha `9377D56A…C0427`**，与基线**逐字节相同**（`artifacts/m4-5b/parity.txt`）。

### 16.2 点 A（`head_in` @10,000，基线 4,720 / .9291 / nm 124 / comp 70 / top3 .9573 / small 48 / dev 2.1920e-3 / cov .6111）

| 权重 | quads | ratio | nm | comp | top3 | small | dev_p95 | cov |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `length`（= M4-5） | 4,959 | .9321 | 136 | 119 | .9352 | 87 | 2.1690e-3（−1.0%） | .5917 |
| `invlen2` | 4,645 | .9355 | 136 | 77 | .9583 | 57 | 2.2113e-3（+0.9%） | .6076 |
| `cotan` | 4,705 | .9311 | **108（−13%）** | **64** | **.9638（+0.65 pp）** | **45** | 2.3363e-3（**+6.6%**） | .6089 |
| `cotanlen` | 4,862 | .9330 | 132 | 66 | .9635 | 46 | 2.2510e-3（+2.7%） | .6105 |

### 16.3 点 B（`body_clean` @15,000，基线 9,982 / .9937 / nm 8 / comp 1 / top3 1.0 / dev 8.1810e-4 / cov .9552）

| 权重 | quads | ratio | nm | comp | top3 | dev_p95 | cov |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `cotan` | 9,929 | .9944 | **28（3.5×）** | 1 | 1.0 | 8.2568e-4（+0.9%） | .9564 |
| `invlen2` | 9,905 | .9926 | **20（2.5×）** | 1 | 1.0 | 8.2035e-4（+0.3%） | .9557 |

（`length` / `cotanlen` 在点 B 未测——A 点已否决，按 D13「只在一个点通过 = 不通过」无需继续。）

### 16.4 四项判定（按 D13 两点矩阵）

| 权重 | 点 A | 点 B | 结论 |
| --- | --- | --- | --- |
| `length` | 挂 **G5**（nm 136）、**G6**（cov −1.94 pp）、**G4-b**（top3 −2.21 pp） | 未测 | **不通过**（延续 §13 的 M4-5 结论） |
| `invlen2` | 挂 **G5**（nm 136） | 挂 **G5**（nm 8→20） | **不通过** |
| `cotan` | 拓扑是本轮最好：**nm 124→108（−13%）**、top3 **+0.65 pp**、small 48→45、comp 70→64；但挂 **G7**（dev **+6.6%** ≫ 2%） | 挂 **G5**（nm 8→28，3.5×） | **不通过** |
| `cotanlen` | 挂 **G7**（dev +2.7%） | 未测（A 已否决） | **不通过** |

**四种权重没有一个能同时过 A/B。**

### 16.5 方向不一致：这才是关键信息

| 点 | 「折痕越贴切」（length → cotan）发生什么 |
| --- | --- |
| **A** | 拓扑变好（nm −13%、top3 +0.65 pp、small −3、comp −6），**几何变差**（dev +6.6%） |
| **B** | **非流形边变多**（8 → 28，3.5×），dev 基本不动（+0.9%）、cov +0.12 pp |

同一个参数在两个点上的**收益方向相反**。这不是调参能解决的——它说明「折痕权重」这个自由度作用的位置不对（§16.6）。

### 16.6 根因：`applyCurlCorrection` 把权重差异重新抹平了

折痕加权**只改了 FrameField 的初值方向**。随后 `applyCurlCorrection` 会**重新优化** θ 与 su/sv 去最小化 1-形式残差——这一步在几何上是「自由」的，于是它把折痕权重带来的方向差异**又抹平了**。

- A 点的 dev 上升、B 点的 nm 上升，都是这个过程的**副产物**：加权让初值更贴折痕 → curl correction 为了消残差去拉 UV → 几何被推离表面（dev↑）；同时在单岛干净网格上把非流形推上去（B 的 8→28）。
- 换句话说：**在 FrameField 里调权重，等于给一个后面会被重新求解的量换初值。** 初值只在收敛路径上有意义，不在最终解上。

**所以正解不是在 FrameField 里调权重**，而是——

### 16.7 D16 裁决

1. **M4-5b 关闭为「已实现、已测量、被否决」**；`AUTOREMESHER_M4_CREASE_AVERAGE` 默认保持 **OFF**；四种权重**保留为诊断手段**（默认 `length`）。
2. **`cotan` 的拓扑收益记录在案**，作为 M4-5c 的**对照下限**：M4-5c 必须至少在 A 点复现 **nm ≤108、top3 ≥.9638、small ≤45**，且**同时**满足 **G7 ≤×1.02** 与 **B 点 G5 ≤8**。
3. **M4-5 线随 D16 关闭**；仅保留 M4-5c 提案（未排期）。

### 16.8 M4-5c 提案（未排期，1.5~2 人日）

| 项 | 内容 |
| --- | --- |
| 动作 | **把折痕方向作为约束加进 curl correction 的 3F 线性系统**（而不是在 FrameField 里调初值权重）。与 §14 的 M4-1c 是**同一类修法**：把自由度放进联合优化，而不是喂初值 |
| 依赖 | **M5 已交付**——3F 未知数会放大 `solveQuadCover` / `applyCurlCorrection` 的求解规模，时间门大概率挂 |
| 验收 | **D13 两点矩阵** + **D16 第 2 条的对照下限**（A：nm ≤108、top3 ≥.9638、small ≤45；同时 dev ≤2.1920e-3×1.02、cov ≥.6111；B：nm ≤8、dev ≤8.1810e-4×1.02、cov ≥.9552） |
| 风险 | 与 M4-1c 同源：未知数增加 → 规模上升 → 可能连 G8 一起挂。两者若同期做，**必须先做 M5**，否则无法判断收益是不是被求解时间抵消 |
| 排期 | **未排期** |

### 16.9 本章对计划的意义

1. **三次负结果已经形成一条清晰的教训**：M4（取整）→ M4-1b（取整调度）→ M4-5b（折痕权重），三次都是在**同一个位置**（给下游求解器喂更好的输入）做优化，三次都被下游的**重新优化**吃掉。M4-1c / M4-5c 的共同点正是「把自由度放进那个重新优化的系统里」——这条已升级为**§2.5 的优化位置原则（D17）**。
2. **`cotan` 的 −13% nm 不是废数据**：它证明了「折痕信息确实能改善拓扑」，只是当前放错了位置。D16 把它定为 M4-5c 的对照下限——**负结果里的局部最优，是下一轮的最低门槛**。
3. **两点矩阵第三次证明自己有效**：`cotan` 在 A 点看起来像重大突破（nm −13%、top3 +0.65 pp），单看 A 会立刻转默认；加上 B 点（nm 8→28）才现出原形。**D13 不是流程洁癖，是这一轮唯一挡住错误结论的机制。**

---
## 17. M3-1：core 分层、生成式源清单、边界门禁、D15 删除

> M3 的第一刀，四个动作一次落地：**目录分层 → 源清单生成 → 边界门禁 → 死代码删除**。四个动作全部**零数值改动**（三份 sha 逐字节不变），这是 M3 唯一可接受的验收标准（§5.7：M3 不改算法）。
> 执行方式：原任务卡住无产出，由 Lead 中断后直接完成。

### 17.1 目录分层：29 个文件 → 8 个模块

| 模块 | 层 | 文件数 | 内容 |
| --- | --- | --- | --- |
| `base/` | 0 | 7 | Progress / Double 等基础类型（Result / Log / Span / Csr 为占位） |
| `geom/` | 1 | 4 | Vector2 / Vector3 / PositionKey / MeshSeparator |
| `numeric/` | 2 | 4 | ConstrainedLeastSquares / MixedIntegerLeastSquares（.cpp + .h） |
| `field/` | 3 | 4 | SurfaceMesh / FrameField / SingularitySimplifier（含 .h） |
| `cover/` | 4 | 2 | QuadParameterizer |
| `param/` | 5 | 2 | Parameterizer |
| `extract/` | 6 | 2 | QuadExtractor |
| `pipeline/` | 7 | 4 | AutoRemesher / IsotropicRemesher |

合计 **29 个文件**，与 §2.1 的目录布局逐模块对齐。原 `src/AutoRemesher` 的 29 个文件全部 `git mv` 过去，**目录已从磁盘删除**（确认目录内 0 项后 `Remove-Item`；git 本就不跟踪空目录，两侧一致）。`Test-Path src/AutoRemesher` 现为 `False`。

### 17.2 转发头修复（P2-5 / P2-6 落地）

- 17 个转发头统一改成 `../../src/core/<模块>/<文件>` —— **物理正确**，不再依赖 `-I` 回退搜索（原文 11 个 `../src/...`、6 个 `../../src/...`，两种深度混杂，见 §6.3 P2-6）。
- 顺手删掉指向不存在文件的死转发头 `QuadRemesher`（→ `quadremesher.h` 全仓不存在，见 §6.3 P2-5）。
- **对外 `#include <AutoRemesher/X>` 完全不变** —— 这正是保留转发头的唯一理由：目录重构对外不可见。

### 17.3 生成式源清单（D2 单输出版本落地）

- 新增 `tools/gen_sources.py`（支持 `--check`）。
- `cmake/core_sources.cmake` 由它生成：**12 个 `.cpp`**，按模块顺序确定性排序（可重复生成、diff 稳定）。
- `CMakeLists.txt:122` 的 `file(GLOB ...)` 换成 `include(cmake/core_sources.cmake)` —— 新增 .cpp 不再「不重跑 cmake 就静默不编译」（§2.2 两个必须改掉的细节之一，现已消除）。
- 实测：`python tools/gen_sources.py --check` → **exit 0**，输出 `up to date: 12 sources`。

### 17.4 边界门禁：`tools/check_boundaries.py`（新增 G15）

按 §2.3 的层序 `base < geom < numeric < field < cover < param < extract < pipeline` 判定：

| 检查 | 内容 | 实测 |
| --- | --- | --- |
| 向上 include | 模块 M 不得 include 层号高于自己的模块 | 29 文件 **0 违规** |
| Qt 头 | 禁止 `#include <Q...>` / `<Qt...>`（R1） | 0 命中 |
| 未知 include | 只允许标准库 + `Eigen` / `tbb` / `meshoptimizer` / `isotropicremesher` 白名单 | 0 命中 |

- **正例（三种调用都通过）**：

| 调用 | 结果 |
| --- | --- |
| `python tools/check_boundaries.py`（repo 根） | `checked 29 files, 0 violation(s)` / **exit 0** |
| `python tools/check_boundaries.py src` | 同上 / **exit 0** |
| `python tools/check_boundaries.py src/core` | 同上 / **exit 0** |

- **负例**（已注入验证）：在 `base/double.h` 加 `#include <AutoRemesher/QuadExtractor>` → 报 `base includes upward into extract`，**1 违规 / exit 1**；还原后回到 exit 0。

> **调用形式**：`python tools/check_boundaries.py [任意层级路径，缺省为当前目录]`。脚本内部的 `resolve_root()` 对传入路径依次尝试**自身 → 父目录 → 祖父目录**，找到含 `src/core` 的那一层作为根，所以 repo 根 / `src` / `src/core` 三种调用等价。**只有找不到 `src/core` 时才 exit 2** —— 那说明目录结构坏了（或根本不在仓库里），**不是参数用错**。这条已写进 §5.1 的 G15 行。

### 17.5 D15 删除：`smoothCrossField`

- **删除前核对**：按 §15.8 的快照与当前实现逐行比对，**忽略缩进后完全一致（60 行）** —— 这是 D15 反转条件要求的动作（若不一致应以 git 为准并更新快照）。
- **删除**：换成 **5 行墓碑**（含 V9 数字与 §15.8 索引），**无 `#if 0`**。
- **连带清理**：`FieldBrushMode::Always` 已无实现，改为 **legacy 的别名**并打印 `NOTE`；调用点相应简化。

### 17.6 验收：三份逐字节不变（每一步都验过）

| 用例 | quads | non-quads | sha256 | 与基线 |
| --- | --- | --- | --- | --- |
| `body_vox` @15,000 | 8,101 | 83 | `9377D56A…C0427` | **逐字节相同** |
| `body_clean` @15,000 | 9,982 | 63 | `C5325AD4…0F53` | **逐字节相同** |
| `head_in` @10,000 | 4,720 | 360 | `F35B9377…EEDE` | **逐字节相同** |

→ **目录分层、生成式清单、死代码删除三个动作都没有改变数值行为。** 这是 §5.7 对 M3 的要求（G1~G9 必须与基线逐字节一致），也是 §2.5 优化位置原则的反面验证：M3 没有碰任何求解系统，所以它**应该**零变化——也确实零变化。

### 17.7 D19 裁决

1. **M3-1 与 M3-2 合并为「第一刀」并关闭**：目录分层、转发头修复、生成式源清单、边界门禁、D15 删除，五项一次落地，实花 **0.5 人日**（原估 2.0 + 1.5 = 3.5，省 3.0）。
2. **新增 G15 边界门**（与 G14 并列）：退出码直接用，CI 必跑，调用形式写死为「repo 根、无参数」。
3. **M3 剩余项未动**：M3-3（Result/Status + 58 处 cerr 清理）、M3-4（ProgressSink + PhaseEvent）、M3-5（Span/Csr + SurfaceMesh 邻接）。
4. `src/AutoRemesher` 目录已从磁盘删除，git 侧与文件系统一致（无残留空目录）。

### 17.8 状态快照（工作树）

`R:80` + `RM:13`（转发头改写）+ `M:17` + `D:1`（死转发头）+ `??:8`（CMakeLists / cmake / docs / legacy / src/cli / tools / ui-go / artifacts）；**0 commit**（按团队规则不提交）。

---
## 18. M3-5 第一切片：CSR 容器与 SurfaceMesh 邻接

> M3-5 的第一个切片：把 `SurfaceMesh::m_cornersAroundVertex` 从 `vector<vector<size_t>>` 换成 CSR，并新增可复用的 `CsrView` / `Csr` 容器。**三份 sha 逐字节不变**，符合 D19 的闸门。

### 18.1 改动

| # | 内容 | 位置 |
| --- | --- | --- |
| 1 | 新增 `CsrView`（两指针视图：`begin/end/size/empty/[]/front/back/data`）+ `Csr`（`reset(rowSizes)` / `row()` / `rowStart()` / `set()` / `offsets()` / `indices()`） | `src/core/base/csr.{h,cpp}`（h 3,383 B、cpp 340 B）；转发头 `include/AutoRemesher/Csr` → `../../src/core/base/csr.h` |
| 2 | `SurfaceMesh::m_cornersAroundVertex` 从 `vector<vector<size_t>>`（**V 次分配**）改为 `Csr`（**2 次连续分配**）；构造改**两趟**：先按顶点计数 → 再按**升序 corner 顺序**填充 | `src/core/field/surfacemesh.{h,cpp}` |
| 3 | 访问器改为**按值返回 `CsrView`**（16 字节），4 处调用点改为值绑定 | `singularitysimplifier.cpp:101、188`（并改用 `.front()`）、`quadparameterizer.cpp:519、620`；`parameterizer.cpp:96` 的 range-for 不用改 |
| 4 | 重新生成源清单 → **13 sources**（新增 `csr.cpp`），`--check` **exit 0** | `tools/gen_sources.py` / `cmake/core_sources.cmake` |

**为什么「升序 corner 顺序填充」是关键**：CSS 化本身会改变**行内顺序**，而行内顺序会改变下游遍历顺序（`simplifyGraph` 的 `delayPairs`、`fixHoles` 的环起点、`smoothAndProject` 的加权求和顺序）。保持「行升序 + 行内插入序」与旧 `push_back` 完全一致，是这次能做到**逐字节零变化**的根本原因（`csr.h` 的注释也写明了这一点）。

### 18.2 唯一踩到的坑：视图的生命周期

- 访问器一旦返回**视图**（指向 `Csr` 内部数组的两根指针），写法就从「值」变成了「借用」。若签名写成 `const CsrView& cornersAroundVertex(...)`（内部对临时 `row()` 取引用），返回的就是悬垂引用；即使签名按值返回，写成 `const auto& v = mesh.cornersAroundVertex(i);` 也容易让人误以为 `v` 能活得比视图更久。
- **处置**：统一为 **按值返回 + 按值绑定**（`const CsrView corners = ...`），从类型层面消除歧义。
- **range-for 是安全的**：`for (const size_t c : mesh.cornersAroundVertex(v))` 里 range 被绑定到隐藏引用并**延长生命周期**，所以 `parameterizer.cpp:96`、`singularitysimplifier.cpp:209、250` 这类写法不需要改。

### 18.3 门禁：三份逐字节不变

| 用例 | quads | non-quads | sha256 | 结果 |
| --- | --- | --- | --- | --- |
| `body_vox` @15,000 | 8,101 | 83 | `9377D56A…C0427` | **逐字节相同** |
| `body_clean` @15,000 | 9,982 | 63 | `C5325AD4…0F53` | **逐字节相同** |
| `head_in` @10,000 | 4,720 | 360 | `F35B9377…EEDE` | **逐字节相同** |

→ **容器换了、分配次数从 V 次降到 2 次连续数组，但数值行为完全等价。** 这是 D20 第 1 类判定的教科书样本：形状变了，字节没变。

### 18.4 一个重要陷阱：构建产物状态被误判成算法回归

过程中出现过 **「Release 必崩、ASan 与 RelWithDebInfo 正常」** 的假故障。真实原因不是代码：`build/m3` 这个**增量**目录在之前一次编译中断后留下了不一致的产物；**clean 重建 `build/m3b` 后，同一份源码完全正常**（cube 用例 164 quads，与基线一致）。

> **教训**：门禁用例必须在**全新 build 目录**（或 clean 后）跑，否则会把「构建产物问题」误判成「算法回归」。

这条与 T7（记录 `CMakeCache` / env 的实际取值）是**同一类问题**：都是「环境/产物状态没有被记录，导致结论不可复现」。因此合并为一条**门禁前置条件**（见 §5.2.1 的 T7 复合条款，a=记录实际配置、b=全新构建）。

### 18.5 D20 裁决：等价实现判定（G16）

**结构类改动**（容器 / 遍历顺序 / 分配策略）的判定**必须按以下顺序**，不许跳步：

| 步 | 条件 | 判定 | 后续动作 |
| --- | --- | --- | --- |
| 1 | 三份 sha **全等** | **逐字节等价**——直接通过 | **不需要**重跑基线 |
| 2 | sha 变，但拓扑四项（`components` / `edges_non_manifold` / `boundary_loops` / `valence_histogram`）**全等** | **浮点累加顺序变化**，属**可接受的等价实现** | **必须重跑基线并更新 §11.1**，否则后续 A/B 全部以旧 sha 为准会一直红 |
| 3 | 拓扑四项**也变** | **行为改变**（不是等价） | 按 D19 反转条件**定位到具体动作**，不许当作「等价」放行 |

**本次 CSR = G16 的第一个正例**：三项 sha 全等，落在第 1 步。

### 18.6 D20 记录

1. **新增 G16「结构等价门」**：判定规则即 §18.5 的三步；适用于一切容器 / 遍历顺序 / 分配策略类改动。
2. **第一个正例登记在案**：M3-5 第一切片，三份 sha 全等（§18.3）。
3. **T7 升级为复合条款**（记录实际配置 + 全新构建），见 §5.2.1。

### 18.7 本切片之后仍未动

| 项 | 状态 |
| --- | --- |
| M3-5a CSR 容器 + SurfaceMesh 邻接 | ✅ **本切片完成**（实花 0.5，原估 2.0） |
| M3-5b 面表 `vector<vector<size_t>>` → CSR | 未动 |
| M3-5c 抽取图数组化（halfEdges / crossPointMap / edgeConnectMap） | 未动（A/B 风险最高的一块，见 §4.4） |
| M3-3 Result/Status + 58 处 cerr | 未动 |
| M3-4 ProgressSink + PhaseEvent | 未动 |

---
## 附：与既有文档的关系

| 文档 | 关系 |
| --- | --- |
| `docs/BUILD-CORE.md` | M1 的构建手册与实测记录。本文 §0.1 引用其数字，不重复其内容 |
| `AUTOREMESHER_DECOMPOSITION.md`（主检出根目录） | 本文的输入参考。§1.5 给出核验结论：10 条 P0/P1 全部确认、行数口径需说明、新增 4 条（N1~N4） |
| `artifacts/COMPARISON.md`（待产出） | 本文 §5 验收门的实际数字来源 |
| 本文 | 施工图：§2/§3/§4 给结构，§5 给里程碑与门，§6 给逐条改法，§7 给并行与可观测性，§8 给兼容策略，§9 给排期，§10 给证据与待验证 |

## 附：决策记录

记录已裁决事项，供后续评审追溯。每条都写明**理由**与**反转条件**——没有反转条件的决策不是决策，是习惯。

| ID | 日期 | 决策 | 理由 | 反转条件 |
| --- | --- | --- | --- | --- |
| D1 | 2026-11-10 | 基线 JSON（V1）完成即视为 **M0 收口**；此后 **M3（目录+契约+CSR 容器）与 M4-1（MILS 取整阈值）/ M4-5（FrameField 多锐边）并行开工** | 两者技术不互相依赖（MILS 只在 `mixedintegerleastsquares.cpp` 内部、FrameField 只在 `framefield.cpp:101-110`、CSR 容器在 `base/Csr.h`）；且 M4 的质量收益需尽快拿到，用于**校准验收门 G1/G5/G7 的阈值**（原本是按经验定的） | 若并行期出现"M4 改动在 M3 的容器上不可比对"（diff 不可读/指标对不上），或 P2 约束（见 §9.2）触发两次以上，则退回串行 |
| D1-a | 2026-11-10 | 并行期**每条 PR 必须跑 G1/G5/G7** 并在 `artifacts/COMPARISON.md` 留一行 | 并行期的唯一保障是"每个改动都自带指标"，否则回归无法归因 | 无（该约束不可撤销；撤销即等于放弃并行） |
| D1-b | 2026-11-10 | **M5（并行化）严格排在 M3 之后** | M5 要改的邻接容器正是 M3 的交付物，提前并行会让 diff 不可读、指标无法归因 | 无 |
| D2 | 2026-11-10 | qmake **保留为"仅 GUI 薄壳"**，不删 `autoremesher.pro`；core 源清单以 **CMake 为单一来源**，由 `tools/gen_sources.py` 生成 `cmake/core_sources.cmake` 与 `qmake/core_sources.pri` 两份 | 本机 **无 Qt**，qmake 路径无法验证；删除构建入口属**不可回滚动作**。而 .pro 保留的成本仅是"多一份生成物的 include" | Qt 侧 CI 恢复绿色、`autoremesher_gui` 跑通后，再提案"GUI 也迁 CMake 并删除 .pro"。届时需一次完整三平台打包验证 |
| D2-a | 2026-11-10 | 生成物（`core_sources.cmake` / `core_sources.pri`）**进版本库**，CI 增加 `gen_sources.py --check` | 生成物 diff 可读，且避免"忘了跑脚本"导致的构建差异；`--check` 保证产物与脚本一致 | 若生成物 diff 噪音过大（例如排序不稳定），改 `.gitignore` + 构建期生成，但必须同时把 `--check` 挪到 configure 阶段 |
| D3 | 2026-11-10 | `--field-brush` 默认 **legacy**（即保持现状：`smoothCrossField` 永不执行），新增显式开关 `--field-brush=legacy|always` | 改默认值等于换算法，会动 G1/G5/G7 的**全部**基线；先有开关与 A/B，再由数据决定是否翻默认 | 若 `--field-brush=always` 在 4 模型上 G1/G5/G7 全不劣于 legacy，可提案翻默认；**翻默认必须由 task-4 新建 baseline 目录**（baseline 只由它写） |
| D4 | 2026-11-10 | `--obj-precision` 默认维持 **6 位有效数字**（与 Qt 版逐字节对齐），新增 `--obj-precision=N`（允许 6 / 9 / 17） | 默认值与 Qt 版对齐是 SHA 层 A/B 的前提；把默认改大会让 baseline 与 after 的字节比较失效 | 任何「默认改大」的动议必须**同时重建 baseline 与 after 两套 JSON**；且这是「输出保真」而非「算法质量」，验收门里单列（G13），不混进 G1/G5/G7 |
| D5 | 2026-11-10 | **M3 期间不升 C++17**，保留 §3.3 的自研 Span | 升标准会牵动 Qt 6 + TBB 2017 的既有 CI 组合，而收益（Span）40 行就够 | 若 M3 后要升，三条前置**缺一不升**：① CMake 与 .pro 同步改标准 ② TBB 2017 / Eigen 3.5 在 MSVC 下无回归 ③ 全量 G1~G12 有 before/after 两套数据 |
| D6 | 2026-11-10 | **G4 主壳占比不硬编码 0.95**，改为双条件 `max(0.85, baseline_ratio − 0.02)`；并把原先两条豁免规则合并为一条 | 硬编码 0.95 会让 multi12k（基线 47.79%）**永远过不了自己的门**，而 head10k 只比 0.95 高 0.22 pp（随时翻车）；阈值必须由基线自身校准 | 阈值随基线更新自动重算——**禁止把某次实测值当常数抄进文档或代码**（防漂移）。若某用例基线 ratio < 0.87，则 0.85 地板生效并自动失去约束力，需在 report 里明示 |
| D7 | 2026-11-10 | **指标必须连工具版本一起记录**：凡引用指标 JSON 处，必须同时记录 `metrics_meta.json` 里的 `mesh_metrics_sha256`（当前 `5defe0ed…`）；工具 sha 已授权刷新到当前版本 | 指标 JSON 是某个版本工具的输出，不记 sha 就无法复现，也无法判断两份 JSON 是否可比 | 换工具版本 → 必须重建 baseline 与 after 两套 JSON，并在 `COMPARISON.md` 头部更新 sha；旧 sha 的 JSON 只能作历史参考 |
| D8-1 | 2026-11-10 | **前置失效条款**：基线处于退化态（bbox 尺度误差 >5% 或 coverage <35%；scaled10k 实测 11.99% / 28.19%）时，G4-a/G4-b 的计数判据**不适用**，改由 G4-c 与 G1/G5/G7 承担；该用例上**禁止用「碎片增加」当回归证据** | 塌陷态的碎片数是被焊掉的假数字，拿它当分母等于要求修复后继续塌着 | 退化态判据本身可调（5% / 35% 为当前经验值）；基线修复后自动回到常规判据 |
| D8-2 | 2026-11-10 | `small_face_count` 阈值写法固定为 **≤ 基线 × 1.10（含等号）**；multi12k 的 100→110 记为**恰好压线、判通过** | 边界值已经出现一次（multi12k），`<` 与 `≤` 必须唯一解释，否则同一结果两个结论 | 无 |
| D8-3 | 2026-11-10 | **G4-0**：进入 G4 判定的用例必须标注 `scale_class`（unit/small/large）与 `parts_class`（single/multi），**未标注不得判定**；G4-b 按 parts_class 选 largest 或 top-k | 防下一轮拿 multi-part 的 largest 套单部件阈值（multi12k largest 0.4779 会得出「主结构塌 47%」的错误结论） | 无 |
| D9 | 2026-11-10 | **Qt GUI 冻结为 `legacy/gui-qt/`**：不进默认 target / CI / 发布，不接 P0~P2 修复（恢复所需除外）。新前端走进程边界，不链接 core | GUI 在本机**不可构建、不可验证**（H1/H2），四条待验证项（V8）因此永远悬空；而 CLI 已由 M1/M2 解耦为唯一稳定接口，GUI 不再是任何链路的必需品 | 恢复步骤见 §12.6（搬回 + 修四处 + 需 Qt 机器，估 2~3 人日）。**冻结 ≠ 删除**：源码/qrc/shaders/.pro 原样保留 |
| D10 | 2026-11-10 | Go 前端**优先进程 + NDJSON**，不优先 cgo；`autoremesher_capi` + cgo 留作进阶（§12.4） | 零 ABI 风险、离线可构建（无 cgo → 单静态二进制、不需要 Qt/TBB/MSVC 运行时）、崩溃隔离（前端崩了不影响 remesh）。capi 的收益在当前负载下为零：进程启动 0.03~0.04 s、事件量约 200 条/次 | 触发条件（满足任一才上 capi）：长会话 / 多作业并发 / 零拷贝预览（§12.4 已量化） |
| D11 | 2026-11-10 | **M4 两个开关默认 OFF**（M4-1 逐步取整 / M4-5 折痕平均），保留开关供后续 A/B；两项「修好」降级为 backlog **M4-1b（2.5 人日）/ M4-5b（1.5 人日）** | 四个变体**无一项满足全部门**：M4-1 挂 **G6**（覆盖率 −1.76 pp）与 **G7**（dev +4.1%）；M4-5 挂 **G4-b**（top-3 −2.21 pp、分量 70→119、small 48→87）、**G5**（非流形 +9.7%）、**G6**（覆盖率 −1.94 pp）；两者同时挂 G4-b/G5/G6。**三个变体全部挂 G6**（§13.2/§13.3） | M4-1b 达标的判据：nm ≤84 且 `dev_p95 ≤ 基线×1.02` 且覆盖率 ≥0.6111；M4-5b 达标的判据：top-3 ≥ baseline−2 pp 且 small ≤ baseline×1.10。达标后才重启「转默认」议题 |
| D12 | 2026-11-10 | **M4-1b 关闭为「已实现、已测量、被否决」**；`AUTOREMESHER_M4_PROGRESSIVE_ROUNDING` 默认保持 OFF；**保留**自适应迭代上限（独立健壮性修复）与 band/batch env 旋钮 + `plannedRoundCount()`（诊断工具）；**M4-1 线不再排期**，M4-1c 仅在有人投 4~6 人日做尺度耦合时启动 | 调度扫描（§14.3）显示 coverage 全线下滑、quads 全线 −14~−17%、nm 在 target 4000 上反而变差；§14.4 证明「nm −32%」只在 target 10000 这一个操作点成立；§14.5 定位根因＝`t` 与尺度/保角缩放**没有联合优化**，不是调参能救的 | M4-1c 启动条件：有人投 4~6 人日 **且** M5 已交付；验收仍用 D11 三条（nm ≤84 / dev ≤×1.02 / coverage ≥0.6111），换操作点须同时换基线 |
| D13 | 2026-11-10 | **M4-1c 的验收改为两点矩阵**（A：head_in @target 10000；B：body_clean @target 15000），每点阈值由该点自己的基线推导，**两点同时满足**才通过；**只在一个点通过 = 不通过**；**不接受单点基线、不固定单一操作点** | §14.4 已证明同一算法在 target 10000 上 nm −32%、在 target 4000 上 nm 变差——**方向会反转**。固定单点等于鼓励下一轮「挑点验收益」，而挑点会让结论翻转 | 若 M4-1c 换用第三/第四个操作点，必须**扩充矩阵**（不是替换点 A/B）；基线一律由 task-4 重建 |
| D14 | 2026-11-10 | **`smoothCrossField` 标记 dead code**（M3 搬目录时删除）；**`brush` 保留、不许删**（`off` 仅诊断且带 WARNING）；**M4-5b 解除阻塞**并从 1.5 → **1.0 人日**，折痕加权改在 FrameField 内部做、brush 原样保留；关闭 V9 | V9 实测（§15）：`smoothCrossField` 的调用条件 `guidance` 恒非空 → **从未执行**，强制打开（`always`）在 A/B 两点都是负收益（A: nm +9.7%/dev +3.8%/cov −1.59pp；B: nm 8→16 翻倍）；`brush` 关掉后 seam 爆炸、cover 病态（CPU 134 s/216 s 无输出）。**「两套并列算法」的前提被证伪：一套在用 + 一套已死** | 若将来重提 `smoothCrossField`，必须先给出它在 A/B 两点都不劣于 legacy 的数据；`brush` 的删除动议一律拒绝，除非能证明 off 模式下有等效替代且不病态 |
| D15 | 2026-11-10 | **死代码删除协议**：不用 `#if 0`；删除时函数签名缩成 3~4 行墓碑（含 V9 数字与 §15.8 索引），实现原文整段移入 §15.8 并存档 commit sha | `#if 0` 只是把死代码藏起来，`grep`/重构/折叠里照样产生成本；而完全删掉又会丢掉「它到底做过什么」的可查性。墓碑 + 文档原文 = 索引与可复原性都有，误读成本归零 | 若 §15.8 的原文与仓库状态不一致（例如 M3 前又改过该函数），以 `git show <sha>` 为准并更新本节与 sha 标注 |
| D16 | 2026-11-10 | **M4-5b 关闭为「已实现、已测量、被否决」**；`AUTOREMESHER_M4_CREASE_AVERAGE` 默认 OFF，四种权重保留为诊断（默认 `length`）；**`cotan` 的拓扑收益定为 M4-5c 的对照下限**；M4-5 线关闭，仅留 M4-5c 提案（1.5~2 人日，未排期） | 四种权重**没有一个能同时过 A/B**（§16.4），且方向相反：A 上折痕越贴切→拓扑越好但 dev 越差，B 上→非流形边翻倍（8→28）。根因：折痕加权只改 FrameField 初值，`applyCurlCorrection` 重新优化时把差异抹平（§16.6） | M4-5c 必须至少在 A 点复现 nm ≤108 / top3 ≥.9638 / small ≤45，**且**同时满足 A 的 G7 ≤×1.02 与 B 的 G5 ≤8；启动前提 M5 已交付 |
| D17 | 2026-11-10 | **优化位置原则**（§2.5）：改进必须落在「自由度所在的那个系统」里。给下游求解器喂更好初值/调度/权重的改动，会被下游重新优化吃掉（M4 / M4-1b / M4-5b 三次实证）；能被保留的只有把新自由度/约束加进那个重新求解的系统（M4-1c 尺度、M4-5c 折痕约束） | 三个负结果的共同结构是「优化位置错了」，而不是「参数没调对」。写成原则后，评审任何优化项都先问「改的是初值还是自由度」，避免第四次同类重试 | 若出现「初值侧改动却在两个操作点上都稳定改善」的实证，本原则需要放宽并记录反例 |
| D18 | 2026-11-10 | **人日口径**：§9.1 的合计只由**算式脚注**定义（**当次**为 base 47.0 + 4.5 = 51.5；M3 第一刀结算后为 48.5，见 §9.1）；「已花」与「未排期」**必须分栏统计**；未排期项**永不计入合计**；任何调整只改脚注，禁止只改总数 | 本轮 52.0/52.5/51.5 三个数对不上，直接原因是「已花」与「未排期」混在一列里，且有人只改了总数没改明细 | 无（口径类决策不设反转条件；若要改口径，必须连脚注与分栏表一起改） |
| D19 | 2026-11-10 | **M3-1 与 M3-2 合并为「第一刀」并关闭**（目录分层 / 转发头修复 / 生成式源清单 / 边界门禁 / D15 删除，五项一次落地）；**新增 G15 边界门**（CI 必跑、退出码直用、调用形式写死为 repo 根无参数）；M3-3 / M3-4 / M3-5 未动 | 三份 sha（body_vox `9377D56A…`、body_clean `C5325AD4…`、head_in `F35B9377…`）**逐字节不变**，证明分层与清理没有改变数值行为；实花 0.5 人日（原估 3.5） | 若 M3-3/3-4/3-5 任一改动使三份 sha 变化，则按下一条 D20 流程定位到具体分层动作后再继续 |
| D20 | 2026-11-10 | **结构类改动（容器 / 遍历顺序 / 分配策略）一律按 G16 三步判定**：① 三份 sha 全等 → 逐字节等价，不重跑基线；② sha 变但拓扑四项全等 → 浮点累加顺序变化，可接受但**必须重跑基线并更新 §11.1**；③ 拓扑也变 → 行为改变，按 D19 定位，不许当等价 | sha 只能说明「有差异」，不能说明「差异是什么」。**sha 全等是等价，sha 变而拓扑全等是「不同实现的同一模型」**——两者都是等价，但后者会让后续所有 A/B 永远以旧 sha 为准而恒红，所以必须重跑基线 | 若第 ③ 类（拓扑也变）被判为「可接受的等价」，必须同时改写 §11.1 基线并把理由写进 COMPARISON.md；否则视为违规放行 |

**与本文其余部分的关系**：D1 改变了 §9.2 的并发编排与人日表里 M4-1/M4-5 的依赖列；D2 改变了 §8.1 的阶段表（M3 行的落地方式、M6 行的结论）。两处正文均已同步，**决策记录是权威，正文以它为准**。

**待裁决（尚无结论）**：暂无。原 Q1（`--field-brush` 默认值）、Q2（`--obj-precision` 默认位数）、Q3（是否升 C++17）已分别裁决为 **D3 / D4 / D5**。

**决策的自我约束**：D1~D5 全部写成"默认不动 + 加开关 + 数据说话 + 反转条件明确"。这不是保守，而是因为本项目的验收门（G1~G12）刚刚才被 §11 的真实数据校准过一轮——在门本身还不稳定时改默认值，等于用未校准的尺子量新的东西。
