# AutoRemesher 架构改造计划（剩余工作）

> **本版起，正文只写「还没做的」。** 已交付的代码、已结案的实验、被否决的变体，一律不在正文描述——**附录 A** 每条留一行索引 + 证据路径；要看过程去看代码、`artifacts/` 与 git 历史。
>
> **保留的是判据，不是历史**：验收门（§5.1）、计时协议（§5.2.1）、优化位置原则（§2.3 / D17）、决策记录（附录 B）原样保留——它们是量剩余工作的尺子。
>
> 快照 2026-11-10 ｜ 工作树 `arch-refactor` ｜ 回归锚点见 §1.2 ｜ 剩余工作量 **37.5 人日**（§9.1；2026-11-10 因 M3-5 切分点修正 +1.0）
>
> 旧版 2,355 行中约 1,300 行是已完成工作的过程记录（§0 现状清单、§5.4~5.10、§10.1、§11 全量、第 13~18 章），本版已删除或压成附录 A 的一行索引。**这两个行数是旧版的一次性快照，不随后续编辑变化**，因此不属于 C4-② 要排除的「易变元数据」。
>
> **本文不记录自身体积**（行数 / 字节）：那是每改一次即失效的易变元数据，读者无法判断它属于哪一刻（C4-②）。需要时按 C4 现测并带上取数时刻。

## 0. 七条口径（先读这七条）

| # | 口径 | 本项目真实反例 |
| --- | --- | --- |
| C1 | **只写没做的**：已完成事项只留索引，不写过程 | 旧版用 120 行记录 M4 实验过程，读者要从里面翻出「这条路已否决」一句话 |
| C2 | **阈值只写推导式**，具体数字一律标注「某次基线快照」 | 把 4,720 / 8,101 这类数字当常量抄进脚本，重建基线后全错 |
| C3 | **环境与产物状态必须随结论一起记录**（T7）：开关**实际取值** + 是否**全新构建** | `option()` 缓存把 OFF 变 ON（7,954 vs 8,101）；中断的增量目录把产物问题误判成算法回归 |
| C4 | **任何数字必须带「字段名 + 用例 + 口径」**，否则不得引用。**会随后续编辑变化的元数据（本文自身行数 / 字节 / wall）只出现在交付报告里，并带取数时刻** | 两处实证：83 / 63 / 360 被写进 `components` 列（其实是 `non_quads`）；head10k 的 0.9522 与 0.9573 被读成矛盾（其实是 `largest` 与 `top-3`，见 §5.1 的 G4-b 取字段规则）。**反面写法**：把本文自身的行数/字节写进本文头部——每改一次即失效，且读者无法判断它是哪一刻的量 |
| C5 | **同一测量只在一处给值**，其余位置写交叉引用、不复制数字。**重复即漂移**：改一处忘记另两处，两个值就都「像实测」 | stderr 体积曾在正文出现两套值：§5.1 的 G11 写 multi12k **6.94 MB**，另三处仍留旧的 **3.9 MB**（差 78%）。根因不是忘了同步，而是抄了三份；**口径写全的人会发现填不出两个差一个量级的值**，所以 C4① 能兜住 C5 的漏 |
| C6 | **正文不写「当前 / 最新」+ 具体值**。这类断言**没有失效机制**：写成「现测 `<值>`（取数时刻）」或只给交叉引用。**判据**：任何「值」若没有配套的失效检测（**mtime / 版本号 / 重跑比对**三者之一），就当它**已过期**处理 | D7 曾写「当前 `5DEFE0ED…`」，而工具早已变为 `9456BA7B…`（8 份 `metrics_meta.json` 全为新值）——**照旧值去比 sha 的人会得出「不可比」的错误结论**。「当前」两个字让一条本该自我校验的规则失效了。**下游还有一处同源**：`artifacts/COMPARISON.md` 头部也内嵌过同一个失效值，而它正是门判定的直接信息来源 |
| C7 | **派生件必须在其输入之后生成**，并记录输入的指纹（工具 sha / 数据 mtime）。**顺序颠倒＝漂移**，因为派生件不会知道自己比输入旧。**推论**：判定派生件是否可信，**只看它的输入指纹，不看它的内容像不像对的** | `artifacts/COMPARISON.md` 的真实时间线（现测 mtime）：**15:51:02** 生成（头部内嵌 `5DEFE0ED…`）→ **16:03:10** 工具被改（sha 变 `9456BA7B…`）→ **16:03:34** baseline 四份 `metrics_meta.json` 重生成。它**从 16:03:10 起就是死的，而它自己不知道**。这不是「忘了同步」，是「汇总件先于数据生成」——任何人手写头部都挡不住这个顺序 |

**全局不变式**（任何改动都不得违反）：

1. **默认路径字节不变**：默认参数下三份 OBJ 的 sha256 必须等于 §1.2 的锚点；要动它必须先过 G16 与 D13。
2. **新行为一律 OFF by default**：CMake option 默认 OFF、env 旋钮不设即旧行为；改默认＝独立提交 + 一轮两点矩阵。
3. **不自动提交**：除明确要求，不产生 git commit / push。

## 1. 起点：现在有什么

### 1.1 资产速查（只写位置与用途）

| 资产 | 位置 | 说明 |
| --- | --- | --- |
| 构建入口 | `CMakeLists.txt` | `autoremesher_core`(STATIC) + `autoremesher_cli`(EXE)；legacy Qt 目标打开即 `FATAL_ERROR` |
| core 分层 | `src/core/{base,geom,numeric,field,cover,param,extract,pipeline}` | 29 文件；规则见 §2.1，门 G15 |
| 源清单单一来源 | `tools/gen_sources.py` → `cmake/core_sources.cmake` | 禁 GLOB；`--check` 退出码 0/1 |
| 边界门 | `tools/check_boundaries.py` | G15，用法 §2.2 |
| 度量工具 | `tools/metrics/mesh_metrics.py` | `measure` / `compare`；G1~G7、G12 的唯一口径 |
| CLI | `src/cli/main_cli.cpp` | 开关实测清单见 §8.2；退出码 0/1/2/3 |
| 可选前端 | `ui-go/` | 纯标准库，`:8787`，SSE；进程边界，不链 core |
| legacy GUI | `legacy/gui-qt/` | 冻结，不进默认 target / CI（D9）；恢复见 §11.4 |
| 基线产物 | `artifacts/{baseline,after}/**/metrics.json` + 三份汇总件 | 门判定的数字来源。**汇总件（`COMPARISON.md` / `baseline/SUMMARY.md` / `after/SUMMARY.md`）是生成物，不要手改**——规则与退出码见 §5.2 第 4 条 |

### 1.2 回归锚点（默认路径三份 sha）

| 用例 | 参数 | quads | non-quads | components | sha256 前缀 |
| --- | --- | --- | --- | --- | --- |
| `body_vox` | `--target-quads 15000 --sharp-edge 60 --adaptivity 0.3` | 8,101 | 83 | 1 | `9377D56ACE10995E…0427` |
| `body_clean` | `--target-quads 15000 --sharp-edge 60 --adaptivity 0.3 --anisotropy 1` | 9,982 | 63 | 1 | `C5325AD47432A8DE…0F53` |
| `head_in` | `--target-quads 10000 --sharp-edge 60 --adaptivity 0.5 --anisotropy 1` | 4,720 | 360 | 70 | `F35B937797976BE4…EEDE` |

**sha 的写法（唯一约定）**：文档里一律**大写**；本表用「**前 16 位 … 末 4 位**」，其余位置引用可缩写为「前 8 位」，**同一份 sha 不许混用两种截断**（大小写、4 位/5 位尾缀混着写，核对时会被当成两个不同的值）。**权威值是小写十六进制**，存在 `metrics_meta.json` / `run.json` 里——同值两种表示，不是两个值。

**判定**：结构类改动后三份 sha 全等 → 逐字节等价，**不必重跑基线**（G16 第①步）。任一份不等，按 G16 第②/③步处理，不许直接放行。

**`components` 与 G4-b 是两个不同字段，不要互相印证**：`components` 是边连通分量个数（1 / 1 / 70）；G4-b 的字段按 D8-3 由 `parts_class` 决定：`single` 用 `components_largest_face_ratio`、`multi` 用 `components_top3_face_ratio`（**是选择，不是退化**）。head_in 实测 `largest` = **0.9522**、`top-3` = **0.9573**，同一份输出的两个不同量，**不构成冲突**。

数值会随基线重建而变（C2）；**这三行是当前快照，不是常量**。输入统一取 `E:\ComfyUI-aki-v3\zcode_work\char3d\remesh\{body_vox,body_clean,head_in}.obj`。

**2026-11-10 复核**：文档清理（删已完成章节 + 三处代码注释改指附录）后重跑三份，quads / non-quads / components / sha 全部与上表一致（components 用 `mesh_metrics measure` 复算：1 / 1 / 70）（head10k 16.7 s、body15k 9.1 s、body_vox 6.9 s，wall 仅参考量）。

### 1.3 门禁的两个调用

~~~powershell
python tools\check_boundaries.py        # 退出码 0 才算过；可用任意层级路径作参数
python tools\gen_sources.py --check     # 源清单与脚本一致
~~~

## 2. 结构约束（已落地，仍需遵守）

### 2.1 依赖规则（可机器判定）
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

### 2.2 自动化边界检查（G15，脚本已存在）

规则由 `tools/check_boundaries.py` 机器判定，**不再把脚本正文抄进文档**：`base < geom < numeric < field < cover < param < extract < pipeline`，禁止反向 include、禁止 Qt 头、禁止白名单外 include。新增文件不需要改文档，只需要过门。

### 2.3 改动准则：优化位置原则（D17）
> **改进必须落在「自由度所在的那个系统」里。**
>
> - 凡是**「给下游求解器喂更好的初值」**的改动 —— 都会被下游的**重新优化**吃掉。实例：M4 取整（A.2-1）、M4-1b 取整调度（A.2-2）、M4-5b 折痕权重（A.2-4），**三次全部如此**。
> - 能被保留的只有**「把新的自由度/约束加进那个重新求解的系统」** —— 实例：M4-1c 的每面 log 尺度（进 `solveQuadCover`）、M4-5c 的折痕约束（进 `applyCurlCorrection` 的 3F 系统）。

**评审任何优化项时先问一句：「它改的是初值，还是自由度？」**

| 改的是 | 结果 | 判据 |
| --- | --- | --- |
| **初值 / 调度 / 权重**（喂给下游求解器的输入） | 被下游重新优化吃掉；指标随操作点漂移（A.2-2 已实证） | 出现在「改参数就能让指标好看，但换个操作点就反转」的形态 |
| **自由度 / 约束**（进联合优化的系统） | 保留；代价是求解规模上升（未知数 +3F 或 +每面尺度） | 出现在「指标改善稳定、但时间与规模同步上涨」的形态 |

**为什么把这条写成原则而不是教训**：三个负结果本身不产生行动，它们只产生「别再试」；但这条原则产生**可执行的判断**——它把「再试一次」和「换作用位置」区分开了，也让 M4-1c / M4-5c 的价值可以被独立评估（它们不是第四次重试，是第一次换位置）。

**反例提醒**：`--field-brush` 与 `band/batch` 这类旋钮**属于「初值/调度」侧**，所以它们的收益必然是操作点相关的（V9 与 A.2-2 都印证）。任何人再用它们做「调参改善质量」的论证，先看这条。

## 3. 契约层（未落地：M3-3 / M3-4）

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

**量化目标（数值以 §5.1 G11 为唯一口径）**：库内 stderr 现在是 MB 级（head10k / multi12k 见 G11），M3-3 之后同一输入应 **≤ 200 行**（进度 + phase report + 汇总）。**正文不再重复体积数字**——C4：同一测量只在一处给值。

---

## 4. 数据模型迁移：面表两刀（M3-5b′ / M3-5c）+ 抽取图一刀（M4-8）

### 4.1 现状字节账（可复算）
~~~text
vector<vector<size_t>> faces(F)
  外层          : F * 24 B        （三个指针）
  每面堆块      : 3*8 = 24 B payload
                  分配器取整 32 B + 头部 8~16 B → 实测按 44 B 计
  合计          : F * 68 B
~~~

按真实产物计（A.1-1 的实测计数）：

| 网格 | V | F | 当前 AoS | CSR(uint32) | 节省 |
| --- | --- | --- | --- | --- | --- |
| `head_quad_20k.obj`（以四边为主） | 14,788 | 14,730 | 14,730 × 92 B = **1.35 MB** | 59 KB + 236 KB = **295 KB** | **4.6×** |
| 等距三角网格（`targetQuads*2` = 40,000 面，`main.cpp:1078`） | ~20,000 | 40,000 | 40,000 × 68 B = **2.72 MB** | 160 KB + 480 KB = **640 KB** | **4.25×** |
| `SurfaceMesh::m_cornersAroundVertex` | 20,000 | C = 120,000 | 480 KB + **5.28 MB** | 80 KB + **480 KB** | **10.3×** |
| UV `vector<vector<Vector2>>` | — | 40,000 | **3.7 MB / 份** | `Vector2[C]` = **1.92 MB** | **1.9×** |

**量级**：完整流水线里 AoS 面表 + 角点邻接 + UV 四份 + 抽取图，粗算 40~60 MB 全是"小的、散的、带分配器头部的"块。CSR 化的直接收益不是省几十 MB，而是**去掉 F 次 malloc/free 与不可预测的指针追逐**——这是岛内分块并行的前提。

UV 那份 3.7 MB × 4 = 14.7 MB 是纯浪费（P2-1），与 CSR 是两件事，可分别做。

### 4.2 已落地的容器（只记判据，不记做法）

`src/core/base/csr.h|csr.cpp` 提供 `CsrView`（指针对视图：`begin/end/size/empty/operator[]/front/back/data`）与 `Csr`（`offsets` + `indices`，`reset(rowSizes)` / `row()` / `set()`）；`SurfaceMesh::m_cornersAroundVertex` 已是 `Csr`，两遍构造（计数 → 前缀和 → 按角升序填充）。

**顺序不是风格而是判据**：`simplifyGraph` 的 `delayPairs`、`fixHoles` 的循环起点、`smoothAndProject` 的累加顺序都依赖邻接遍历顺序，所以 CSR 行内必须保持升序才能 byte-identical（G16 第①步）。**后续两个切片沿用同一条：容器可以换，顺序不许换。**

唯一踩过的坑：`const auto& v = mesh.cornersAroundVertex(x)` 绑到临时对象 → 访问违例（exit `-1073741819`）；必须 `const CsrView v = ...` 取值绑定。

### 4.3 M3-5b′ / M3-5c：面表 CSR 两刀（切分点修正；5c 不可拆）

- **为什么分两刀**：面表成员的**外部消费者有 5 处签名**——`MeshSeparator::splitToIslands`、`IsotropicRemesher`（vendored 第三方，接口写死）、`QuadExtractor`（内部 17 处使用）、`Parameterizer`（15 处使用）、`calculateMeshArea`；再加三个必须继续返回 `const vector<vector<size_t>>&` 的公开 getter（CLI 6 个调用点）。**只换成员而不动签名，就必须在每次调用处物化**一份 `vector<vector<size_t>>`（12k 面 ≈ 每面一次堆分配），把 M3-5a 省下的分配原样还回去。结论：**M3-5b 不是自包含的，它的收益以签名迁移为前提。**
- **M3-5b′（0.5 人日）——零边界，只做 3 张表**：`neighbors`（`:358`，本函数内累积）、`isotropicIslandTriangles`、`decimatedIslandTriangles`（`:735/737`，只传给同文件的 `IsotropicPhase` 结构体）。**排除 `trianglesIslands`**：它虽是局部变量，但由 `MeshSeparator::splitToIslands`（geom 模块）作为**出参**填充（`:577-579`），改它的类型＝改 geom 的公开签名，会破坏「零签名改动」这条定位 → 留给 5c。**排除 `mergedTriangles`**：它是 `mergeIslands` 的出参（`:715`），目的地是两个成员表（`:744/748`），改它会在返回处引入一次边界转换 → 同样留给 5c。5b′ 的判断标准是：**声明在 autoremesher.cpp 且只被本文件填充与消费**；「声明位置」不等于「耦合位置」，`trianglesIslands` 就是反例。
- **M3-5c（3.0 人日，原 1.5）——⚠️ 不可拆**：成员表 + 5 处消费者签名 + getter 缓存必须**一刀**做完（含 `QuadExtractor` 17 处、`Parameterizer` 15 处使用点，以及 `autoremesher.cpp` 内 11 处）。理由：这三件事分开做**没有中间态可验收**——只换成员就要物化，只换消费者则无处传入。**这个「不可拆」是设计结论，不是排期偏好**：未来若有人看到 +1.0 人日想把它拆成两半，请先读这一行。
- **允许的物化点只有两个**：公开 getter 与 `IsotropicRemesher`（第三方接口写死）。getter 结果**必须缓存**，不许每次调用重建。**推论**：5c 之后仓库里**仍会常驻一份 AoS 面表**（getter 那份），所以 §4.1 的节省比例要按「**内部路径全 CSR + getter 一份 AoS 缓存**」重算——现表按「全部转 CSR」算出的 4.25× 是**上限**，不是可达值。
- **顺序约束**：行内容保持角点原顺序、行间保持面的插入顺序；不许为省内存做排序或分组——这是 G16 第①步成立的前提，M3-5a 已验证过同一条。
- **验收**：§1.2 三份 sha 全等（G16 第①步）+ `gen_sources --check` + G15 不退；峰值 RSS 记入 V6。
- **回滚**：5b′ 只改 `pipeline/` 内部；5c 另含 `geom/`/`extract/`/`param/` 的签名，回滚粒度是一个提交。**不做运行时开关**（数据结构不适合做 flag）。
- **与 M4-8 的关系（工日去重，写进账不算完）**：**M3-5c 完成时，M4-8 的前半段（抽取图数组化的公共部分）视为已做**——下次重估必须从 M4-8 里扣掉重叠部分，并按 D18 把这次扣减写进 §9.1 的**算式脚注**，不许只改总数。

### 4.4 QuadExtractor 的图结构数组化（**M4-8**，风险最高；原编号 M3-5c 已改义为面表迁移，见 §4.3）
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

## 5. 里程碑与验收门（唯一权威）

### 5.1 验收门总表（唯一权威，阈值集中在这里）

基线 = M0 采集的 `artifacts/baseline/*.json`。**"当前状态"列只写实测，未测就写"待产出"。**

| 门 | 指标（`mesh_metrics` 字段） | 阈值 | 数据来源 | 当前状态 |
| --- | --- | --- | --- | --- |
| G1 | `quad_ratio_pp` 变化 | 下降 ≤ **1.0 pp** | `compare --quad-tol 1.0` | 基线（A.1-1）：head10k 92.91% / body15k 99.37% / multi12k 92.18% / scaled10k 92.57% |
| G2 | `valence4_ratio` 不降、`irregular_vertices` 不增（原 G7 的内容） | — | `measure` | multi12k 基线 65.61% → after 62.73%（**回归项**，keep-all 的接缝代价）；head10k after 未单独采（本轮未采） |
| G3 | `boundary_loops` | 不增；补洞类里程碑要求**减少** | `measure` | 基线：head10k **6** / body15k **1** / multi12k **4** / scaled10k **2**；输入 head_clean **2** |
| **G4-0** | 用例标注：`scale_class`（unit/small/large）+ `parts_class`（single/multi） | **未标注的用例不得进入 G4 判定**（D8-3）；G4-b 按 `parts_class` 选 largest（single）或 top-k（multi） | 用例表 / run.json | 现有四用例：head10k unit/single、body15k unit/single、multi12k unit/**multi**、scaled10k **small**/single |
| G4-a | `components_small_face_count`（≤3 面壳） | **≤ 基线**；豁免/容忍上限写作 **≤ 基线 × 1.10（含等号，D8-2）**。**D8-1 前置失效**：基线处于退化态时不适用 | `measure` | head10k 48 / body15k 0 / multi12k 100 / scaled10k 2（A.1-2） |
| G4-b | `components_top3_face_ratio`（主结构；`parts_class=single` 时用 `components_largest_face_ratio`） | **≥ baseline − 2 pp**。**多部件禁用 largest**（multi12k largest 0.4779 天然失义）。**D8-1 前置失效**：退化态基线不适用 | `measure` | 基线（**按 D8-3 选定的那个字段**）：head10k **0.9522**（largest，single）/ body15k **1.0000**（两者相同）/ multi12k **0.9355**（top-3，multi；largest 0.4779 失义）/ scaled10k **0.9922**（largest，small+single）。另一口径对照：head10k top-3 0.9573、scaled10k top-3 0.9973（A.1-2 的表是 top-3 口径，**不得与上列混用**） |
| G4-c | 尺度不变性（两尺度 `largest` 之差） | **≤ 0.02**。**独立用例**（scaled10k 对 head10k），且是 **D8-1 退化态下的主判据** | 两次 `measure` 相减 | 基线 **4.01 pp（不通过）** / after **0.11 pp（通过）** —— 本轮最灵敏的一条 |
| G5 | `edges_non_manifold` | **不增** | `measure` | 基线：head10k **124** / body15k **8** / multi12k **160** / scaled10k **52**（A.1-1） |
| G6 | `covered_ratio` | 不降 | 同上 | 基线：head10k **61.11%** / body15k **95.52%** / multi12k **47.03%** / scaled10k **28.19%** |
| G7 | `ref_dev_p95` | ≤ 基线 × **1.02**（rel，**容差 2%**） | `measure --ref INPUT` | 基线（rel）：head10k **2.192e-3** / body15k **8.181e-4** / multi12k **1.843e-3** / scaled10k **2.985e-3** |
| G8 | 单模型 wall | 重构类 ≤ 基线 × **1.10**；提速类（M5）≤ × **0.85**。**必须按 §5.2 的计时协议测**（串行 / 交替 A/B / 3 次中位数），否则不作为判定依据 | 专用基准脚本（§5.2） | 基线：head10k 36.34 s、body15k 12.586 s、multi12k 87.329 s、scaled10k 33.49 s；**本文所有 wall 仅作参考量**（噪声可达 82%，见 §5.2） |
| G9 | 输出 OBJ SHA256 | **同输入连续两次相同**（**判确定性，不判与基线相等**——与基线相等由 §1.2 锚点 + G16① 负责） | `Get-FileHash` | 已通过；**具体值只在 §1.2 给（C5）**，本行不复制 |
| G10 | 构建 + 边界检查 | clean ≤ 60 s；`check_boundaries` 0 错误 | CMake/Ninja | 构建 **10.1 s**（通过）；边界门 G15 见下两行 |
| G11 | 库内 stderr 体积 | ≤ 200 行 / 次 | 重定向计数 | 基线实测：head10k **3.56 MB** / multi12k **6.94 MB** / scaled10k 3.44 MB / body15k 57 KB（A.1-1）；M3-3 的量化目标 |
| G12 | `degenerate_faces` | 不增 | `measure` | multi12k 基线 60 → after 45；scaled10k 0 → 19（随修复恢复真实分片，见 A.1-1 豁免规则） |
| **G13**（**待触发条款，非门**） | 输出保真：OBJ 写出精度（D4） | **未触发 = 无阈值**。触发条件：引入 `--obj-precision` 后才生效——届时默认必须保持 6 位，且必须同时重建 baseline 与 after 两套 JSON。**未触发前**「默认精度没变」由不变式 1 + §1.2 锚点 + G16① 覆盖（G9 只判确定性「两次相同」，**不判与基线相等**）。**分类规则长期有效**：输出保真不混进 G1/G5/G7 | `Get-FileHash` | 当前硬编码 `std::setprecision(6)`（`main_cli.cpp:90`），开关未实现 → **无独立观测量，故不立门**（2026-11-10 降级） |
| G14 | CLI 事件契约（§11.3 原文五条） | 抽样 ≥20 事件按 v1 解析全过；未知字段注入不崩；`done` 含 quads/non_quads/vertices/exit；`phase` 行数与 `phaseReport()` ±0 | `--events-json` + ui-go | 第 **1/3/4** 条已验（§5.2.1 T7 的 `run.json`：62 progress + 1 phase(55 行) + 1 done）；第 2 条待 ui-go 冒烟 |
| **G15** | 模块边界（`tools/check_boundaries.py`） | **退出码 0**：29 文件 0 违规（向上 include / Qt 头 / 非白名单 include）。调用形式 `python tools/check_boundaries.py [任意层级路径，缺省当前目录]`（`resolve_root()` 支持自身/父/祖父三级）；**只有找不到 `src/core` 才 exit 2**，那是目录结构问题不是参数问题 | `python tools/check_boundaries.py` | A.1-3 已验：repo 根 / `src` / `src/core` 三种调用均 exit 0；`base/double.h` 注入 QuadExtractor 后 1 违规 exit 1，还原后回 0 |
| **G16** | 结构等价（容器 / 遍历顺序 / 分配策略类改动） | 三步判定（**不许跳步**）：① 三份 sha 全等 → **逐字节等价**，不用重跑基线；② sha 变但拓扑四项（`components`/`edges_non_manifold`/`boundary_loops`/`valence_histogram`）全等 → **浮点累加顺序变化**，可接受，但**必须重跑基线并更新 §5.1 门表**；③ 拓扑四项也变 → **行为改变**，按 D19 定位到具体动作 | `Get-FileHash` + `mesh_metrics measure` | **第一个正例**：M3-5 第一切片（CSR/SurfaceMesh），三份 sha 全等（A.1-4） |

**门的增删规则**：门号一经分配**永不重排**（G14~G16 与全文引用绑死在编号上）。退出一个门只能写成「**降级为待触发条款**」并原地保留编号、写清触发条件；新增门只能追加新号。**G13 是本规则的第一个适用者**。

**门编号的 canonical 映射（全文以此为准）**：G1 quad_ratio ｜ G2 valence4/irregular ｜ G3 boundary_loops ｜ G4-0~c components ｜ **G5 edges_non_manifold** ｜ **G6 covered_ratio** ｜ **G7 ref_dev_p95（容差 2%）** ｜ G8 wall ｜ G9 sha ｜ G10 build ｜ G11 stderr ｜ G12 degenerate ｜ G13 输出保真（**待触发条款**）｜ G14 事件契约 ｜ **G15 模块边界** ｜ **G16 结构等价**。**历史文本里出现的 G2=nm / G5=dev / G7=valence4 属旧编号，已在 §5.1 统一纠正**（D11 之后）。

**G4-b 的取字段规则（D8-3 的落地写法）**：判定字段由 `parts_class` 决定——`single` 取 `components_largest_face_ratio`，`multi` 取 `components_top3_face_ratio`。上表基线值已按此列出，括号里的另一口径**仅作对照**：head10k 两者差 **0.51 pp**（0.9522 / 0.9573），multi12k 差 **45.8 pp**（0.4779 / 0.9355）。**用错字段不是精度问题，是判据换了对象**；**方向也要写清**：把 top-3 用在单部件用例上会系统性偏严（0.5 pp），后果是「好结果被判不通过」——**假阴性比假阳性更难发现，因为没人会去质疑一个没过门的候选**——multi12k 用 largest 会得到「主结构塌 47%」的假结论（D8-3 的立条理由）。

**G4 豁免规则（唯一一条，合并版）**：若 G4 未过，但**同时**满足 `coverage` 上升 **且** `dev_p95` 下降，则判为「形态变化」→ 豁免并记录；`small_face_count` 的容忍上限为 **≤ 基线 × 1.10（含等号，D8-2）**。**前置失效（D8-1，已裁决）**：基线处于退化态（bbox 尺度误差 >5% 或 coverage <35%）时，G4-a/G4-b 计数判据**不适用**，由 G4-c 与 G1/G5/G7 承担，且**禁止以「碎片增加」作为回归证据**（scaled10k 是首个适用者，见 A.1-2）。

**阈值总原则（写死在表下，防漂移）**：**阈值一律由基线实测推导，文档只记录推导式（如 `max(0.85, baseline − 2pp)`、`|Δlargest| ≤ 0.02`、`≤ baseline`），不记录某次的具体数值。** 本文出现的所有数字都是「某次基线的快照」；基线换了（新模型、新工具版本、task-4 重建目录）就必须重算，禁止把任何一次实测值当常数抄进文档、脚本或代码。

**为什么阈值行必须写「字段名 + 用例 + 方向」（C4 的闭环）**：本轮实证了一件反直觉的事——字段口径写错时，**A/B 指标对比抓不到它**。`cotan` 换成 `largest` 后仍是四权重第一（0.9596），**排序没变**，所以任何以「谁更好」为判据的对比都会给出「没问题」；暴露它的只有门槛的绝对值（0.9638 vs 0.9596）。再叠上方向：用 top-3 判单部件是**偏严** 0.5 pp，后果是假阴性——**没人会去质疑一个没过门的候选**。所以：① 阈值行必须带字段名；② 字段选择必须带用例的 `parts_class`；③ 口径错误必须先问方向（偏严/偏松）；④ 每次重跑基线要复核这四项，**不能只看 `compare` 的退出码**（退出码对口径错误完全免疫）。

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
3. **指标计算工具本身有版本（D7 + C6）**：`measure` 会把 `mesh_metrics_sha256` 写进同目录的 `metrics_meta.json`（**权威值，小写十六进制**）。**正文不内嵌该值**（C5/C6），改为在每个引用点声明自检：**8 份 meta 一致 且 等于 `tools/metrics/mesh_metrics.py` 当前的 sha256**，三条不满足即视为不可比（详见 `artifacts/COMPARISON.md` 头部）。换工具版本 → 必须重建 baseline 与 after 两套 JSON；旧数据只能作历史参考。
4. **artifacts 下的三份汇总件是生成物**：`COMPARISON.md`、`baseline/SUMMARY.md`、`after/SUMMARY.md` 由 `tools/metrics/emit_comparison.py` 生成（**无单文件模式**：一次校验/一次生成都覆盖全批，理由见 C7——同一输入指纹的派生件构成一个批次，指纹一变全批失效）。**不要手改这三份**；要改格式就改生成器。（**当前状态（2026-11-10 已验收）**：`tools/metrics/emit_comparison.py` 已交付；`--check` 幂等、退出码 `0/1/2/3` 各有实测负例（分别是：指纹分歧 → 2、手改一格 → 1、记录与实测冲突 → 3）。三份文件的字段**全部可从 `metrics.json` / `run.json` / 输入 OBJ 推导**（含 island 数：生成时对输入 OBJ 的三角形按共享顶点做并查集现算，与 `MeshSeparator::splitToIslands` 同义）。**过渡期条款「手改必须同批改完三份」已失效**——现在改内容只能改生成器或改输入数据。**代价**：`--check` 与生成都需读三份输入 OBJ 才能现算 island 数（实测约 4 s，量级参考；秒数变化不改变「多读三份输入」这个事实）。**生成文件里不放机器验不了的内容**：因果判断一律进 `artifacts/AB-NOTES.md`（只引用门号与推导式，不复述快照值，C2）。）
5. **三份汇总件的退出码语义（`tools/metrics/emit_comparison.py --check`）**：`0` 一致 ｜ `1` 文件与重算结果不一致（**过期或被手改**）｜ `2` **前置不可生成**（8 份 `metrics_meta.json` 指纹不一致 / 与工具当前 sha 不符 / 缺 baseline 或 after 目录）｜ `3` **记录与实际冲突**（`run.json` 里记的布尔与实测 `out.obj` 不符，或 `run2/` 两次实际不同）。`1` 与 `3` 的区别是判据对象不同：**`1` 说「文件旧了」，`3` 说「这组数据不可信」**。因此生成器必须**自己 hash `out.obj`**，不得只读 `run.json` 里记的布尔值——否则这道判据会「绿着通过而实际没测到」

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
| **T7 门禁前置条件（复合条款）** | **(a) 记录实际配置**：A/B 一律显式 `-D` 传开关或用全新 build 目录；`run.json` 记录 `CMakeCache.txt` 的实际取值 + `AUTOREMESHER_*` env 旋钮的取值，而不是命令行意图。**(b) 全新构建**：门禁用例必须在**全新 build 目录或 clean 之后**跑——增量目录在编译中断后会留下不一致产物，会把「构建产物问题」误判成「算法回归」（§5.2.1（T7(b)） 的 Release 必崩 / ASan 正常就是这一例） | 二者是同一类问题：**环境与产物状态没被记录，结论就不可复现**。`option()` 的默认值低于 `CMakeCache.txt` 优先级（§5.2.1 的 7,954 vs 8,101）；中断的增量目录同理（§5.2.1（T7(b)） 的 `build/m3` vs `build/m3b`） |

**用例划分**（依据 COMPARISON.md 的观测）：

- **岛级并行**（M5 之前的收益来源）：用 head10k（8,979 islands）与 multi12k（17,958 islands）——岛多，外层并行才吃满。
- **岛内并行**（M5 的目标）：用 body15k（1 个 island、10,045 面）——它在岛级并行里 `cores=1.00`，这个"劣势"正是岛内并行的测试条件。
- 所以 G8 不写"单模型 wall"，写**两个用例各一条**：`head10k`"岛级" + `body15k`"岛内"。

### 5.3 里程碑总表（只剩未完成项）

| Ms | 名称 | 剩余动作 | 人日 | 前置 |
| --- | --- | --- | --- | --- |
| M3-3 | 契约层：Result / StatusCode | 错误模型贯穿 + 库内 `cerr` 清理（§3.1、§6.3 P2-4） | 3.0 | M3-1 ✅ |
| M3-4 | 契约层：ProgressSink / PhaseEvent | `phaseReport()` 结构化，CLI 文本逐字节兼容（§3.2、§7.2） | 2.0 | M3-1 ✅ |
| M3-5b′ | 内部临时面表 CSR | 只换无外部消费者的 3 张临时表（**与 §4.3 同源，改那里时这里必须跟**） | 0.5 | M3-5a ✅ |
| M3-5c | 成员面表 + 消费者签名 + getter 缓存（**不可拆**） | 5 处签名 + 32 处内部使用点，一刀做完（§4.3） | 3.0 | M3-5b′ |
| M4 | P1 缺陷 + 接缝 + 默认切换 | P1-2 / P1-5 / P1-7 + P0-1b + 三个开关的转默认判定 | 13.0 | 部分依赖 M3-5c |
| M5 | 抽取器拆分 + 岛内并行 | QuadExtractor 拆 TU、两处并行、P2-3 清理（§7.1） | 8.0 | M4 |
| M6 | 兼容收尾 | GUI 目标接回（需 Qt）、旧接口删除、全量回测 | 8.0 | 全部 |

**M0/M1/M2 与 M3-1、M3-5a、GUI 迁出、ui-go 已交付**——不再排期，索引在附录 A。

### 5.4 依赖关系（M3 之后，仍然有效）
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

## 6. 逐条改造清单（只剩未完成条目）

格式：**ID｜优先级｜位置** → 现状 / 改法 / 影响面 / 验证 / **状态**。

### 6.1 P0：静默产出错误结果

#### P0-1 removeIsolatedFaces 只留最大岛

- **位置**：`quadextractor.cpp:1439-1449`（函数）、调用点 `:195` 与 `:199`
- **现状**：`splitToIslands` 后取 `std::max_element` 按面数，**把结果整体赋给 `m_remeshedPolygons`，其余岛无条件丢弃**。调用点 `:195` 无条件先调一次，所以多部件模型输出只剩最大部件。函数无条件 `return true`，即使只有一个岛也会白跑 `rebuildHalfEdges()+fixHoles()`。**实测危害**：`head_quad_10k.obj` 输出含 **70 个边连通分量**（主壳 4,899 面 + 69 个 2~15 面漂浮块，§5.1），而输入 `head_in.obj` 有 **8,979 个壳**——这条规则在清洗前输入上会把几何削掉绝大部分。
- **改法**：
  1. 保留面数 ≥ `max(4, largest * minRatio)` 的所有岛（`minRatio` 默认 0.02），或保留全部岛只删"面积 < ε×中位面积"的碎片。
  2. 返回值改为"是否真的删了东西"（比较删除前后岛数/面数）。
  3. 被删岛数/面数进 `RemeshOutput.stats` + `Status.detail`。
  4. CLI 暴露 `--island-policy {largest,all,none}` + `--island-min-ratio`。
- **影响面**：多部件模型输出变大（正确方向）；单部件模型行为不变但少一次 rebuild；`fixHoles` / `smoothAndProject` 输入面数上升 → 看 G8。
- **验证**（A.1-1 已实测）：multi12k 开 keep-all → quads 6,660→**7,035（+5.63%）**、components 142→**186**、non_manifold 160→**1,028（×6.42）**、valence4 65.61%→62.73%，而 dev_p95 仅 −0.31%、coverage 仅 −0.23 pp。**结论：机制生效，但接缝代价大于收益，不能转默认。**
- **状态**：**M2 已落地开关；默认切换被阻断——前置门槛（non_manifold ≤160、components ≤142）未达成，需先做 P0-1b / M4-10 的 patch 焊接。**

#### P0-1b（M4-10）patch 边界焊接与重复顶点归并

- **位置**：`quadextractor.cpp:193-205`（`extract()` 里 keep-all 之后的重建段）、`quadextractor.cpp:1439-1449`（`removeIsolatedFaces`）、结果合并 `autoremesher.cpp:922-942`
- **背景**：P0-1 的开关（`--keep-all-islands`）把多 patch 保留下来了，**但 patch 之间没有焊接**。A.1-1 的实测代价：non_manifold **160 → 1,028（×6.42）**、components **142 → 186（+44）**、valence4 **65.61% → 62.73%**，而 dev_p95 仅 −0.31%、coverage −0.23 pp——**+375 quads 没有换来任何覆盖率净收益**。所以现在不是"要不要保留多 patch"的问题，而是"保留之后必须把这些 patch 缝回一张连续曲面"。
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
- **影响面**：**全流水线数值最敏感处**。判重变细 → 交点变多 → 抽取图变细，可能改变四边形数量与拓扑。两个方向都要验：极小尺度看「塌陷」，极大尺度看「溢出」——A.1-2 的 1e5 用例证明基线的长坐标是靠 32 位 `long` wrap **侥幸可用**，M2 改 `long long` + 饱和后行为才确定。**大尺度的错比小尺度更难发现**（基线输出看起来完全正常）。
- **验证**（A.1-1 已实测，证据充分）：scaled10k 开 `--relative-tolerance` 后 quads 2,030→**4,830（×2.38）**、coverage 28.19%→**60.80%（+32.62 pp）**、dev_p95 2.985e-3→**2.360e-3（−20.95%）**，而**最硬的一条是 bbox**：基线输出包围盒对角 0.00117208，比理想值 0.00133052 **小 11.99%**，修复后偏差 **0.09%**。基线 `exit=0`、输出非空——教科书级的"静默退化"。
- **状态**：**M2 机制与证据均已就位**；剩余 = 归一化 + 64 位打包键（M4）+ 三尺度交叉用例（1e-3 / 1 / 1e3，见 A.1-2 缺口）。**副作用非缺陷**：nm 52→136、components 6→70、degenerate 0→19、boundary_loops 2→4 是"从塌陷态恢复真实分片"，按 G4 豁免规则判通过。

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

#### P1-2 CLS 仿射替换链无环检测

- **位置**：`constrainedleastsquares.cpp:155-170`（`resolve` 的 `while`）、`:211-220`（单根/双根替换建立）；同模式在 `mixedintegerleastsquares.cpp:310-313`（`finalizeM1`）、`:376-379`
- **现状**：`resolve` 沿 `m_substitutions[cursor].root` 走，条件 `!fixed && root != cursor`，**没有访问计数、没有深度上限**。当前构造方式理论上不产生环，但只要有人放宽 `:214-217` 的双根合并（例如允许已固定节点重新变成根），立刻无限循环——而这类改动正是重构期最常见的。
- **改法**：① 深度上限 `m_variableCount` + "回到起点"判定，命中返回 `SolverFailed` 并走拉格朗日回退；② 用 **union-find + 按秩合并** 取代父指针全表 resolve（当前最坏 O(n²)，改后 O(α(n))）；③ `finalizeM1` 两处 `while` 同样加计数。
- **影响面**：正常输入数值不变（根相同），只在退化输入下把"挂死"变"返回错误"。
- **验证**：单测构造 A→B、B→A，断言 n 步内返回错误；6 模型 G1~G12 逐位不变。
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
| P2-3 | `quadextractor.cpp:1401、1415、1422、1425、1436` | `searchBoundaries` **逐顶点**打 cerr，`fixHoles` 调两次 → **万级行**（体积见 §5.1 G11，此处不重复编号） | 全删，只留一条"N 个边界环，最大 X"的 Debug 汇总 | stderr ≤200 行/次（G11）；wall 改善 |
| P2-4 | 全局 58 处 | 58 处库内 `std::cerr` | 走 §3.4 `Logger`，调试类降 Debug 且默认关 | `check_boundaries` R4 = 0 |
| P2-8 | `mainwindow.cpp:615-645` | `saveMesh` 成功后不复位 `m_saved`，标题一直挂修改标记 | 成功后置位 + `updateTitle()` | 手工（需 Qt） |
| P2-9 | `mainwindow.cpp:1167`、`:928` | 对象已 `moveToThread`（`:914` 同类），却在 GUI 线程 `delete` | 改 `deleteLater()`，或 `thread::finished` 之后 delete | 需 Qt；待验证 |
| P2-10 | `mainwindow.cpp:1033-1045`；`main_cli.cpp:78-83` | OBJ 写出 6 位有效数字（`QTextStream` 默认），对 10² 量级模型是 1e-4 绝对量化 | 默认提 `%.9g`，加 `--obj-precision` 开关；**默认变更必须与 G9 一起验** | 顶点坐标相对误差 < 1e-7；`ref_dev_p95` 改善 |
**状态**：上表除 P2-8 / P2-9（需 Qt，归 M6-1）外**全部未做**；P2-5 / P2-6 / P2-7 已随 M3-1 落地并从表中移除（索引 A.1-3）。

### 6.4 已关闭的条目（只留 ID，过程见附录 A）

**P1-1 / P1-3 / P1-6 关闭**：分别对应 A.2-1（M4 两开关否决）、A.2-3（场算法归属），处置已写进 D11 / D12 / D14 / D16。

**P2-5 / P2-6 / P2-7 已落地**（M3-1，A.1-3），已从 §6.3 的表中移除。

**P2-10 半落地**：`--obj-precision` 开关**未实现**，OBJ 精度仍是硬编码 `std::setprecision(6)`（`main_cli.cpp:90`）；D4 的「默认 6 位」因此被默认满足。**G13 已因此从门降为待触发条款**（§5.1）：未引入开关前它没有独立观测量，「默认精度没变」由不变式 1 + §1.2 锚点 + G16① 承担。

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
- **MILS 迭代轮数与调度参数**（P1-1 / A.2-2）：`plannedRoundCount()` 的解析值、实际轮数、`band` / `batch` 的取值（含 env 覆盖）——M4-1b 证明「轮数用尽 = 静默丢岛」，所以这三个数必须进事件流，否则下一次同样的悬崖还是看不见
- 被放弃补洞的边界环数（P1-5）
- 被丢弃的岛数与面数（P0-1）
- `failedIslands` / `totalIslands`（§3.1）
- 被过滤的越界三角形数（P1-7）

**日志分级的落地**：`LogLevel::Debug` 默认关（`--verbose` 打开）。这样默认路径下的 stderr 体积（当前量级见 §5.1 G11）会降到接近 0，需要时仍可拿到。

---

## 8. 兼容期与回滚

### 8.1 qmake 与 CMake 并存策略

| 阶段 | qmake（`autoremesher.pro`） | CMake | 说明 |
| --- | --- | --- | --- |
| M1/M2（现在） | 仍是"官方"GUI 构建 | 只构建 core + CLI | 两者源清单必须同步；目录重构会打破这一点 |
| **M3（D9 之后）** | **已移入 `legacy/gui-qt/`，主链不再引用** | core + CLI + 边界检查 | 原 D2 的 `gen_sources.py` 双输出方案**作废**（core 源清单本就只有 CMake 一份；`legacy/gui-qt/autoremesher.pro` 保留其历史 SOURCES 列表，不再要求同步） |
| M6 | 不适用（已冻结） | core/cli/测试的唯一构建系统 | 如需恢复 GUI，走 §11.4 的四步，**不是**恢复 qmake 并存 |

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
3. 恢复条件：Qt 侧 CI 恢复绿色并跑通 GUI 构建后，按 §11.4 的四步恢复；恢复时 qmake 随 `legacy/gui-qt/` 一起搬回，**不恢复「两套构建系统并存」的状态**。

### 8.2 行为开关（实测清单，不是计划清单）

| 开关 | 存在形式 | 默认 | 期限 / 备注 |
| --- | --- | --- | --- |
| `--keep-all-islands` | CLI | 关 | 转默认待 P0-1b 达标（§6.1）；M6 删开关 |
| `--relative-tolerance` | CLI | 关 | 转默认随 M4-7；M6 删 |
| `--triangulate` | CLI | 关 | 先做 V4；转默认随 M4-7；M6 删 |
| `--events-json` | CLI | 关 | 长期（§11.1 契约） |
| `AUTOREMESHER_FIELD_BRUSH` | env（`quadparameterizer.cpp:59`） | `legacy` | `off` 仅诊断，保留（D14） |
| `AUTOREMESHER_CREASE_WEIGHT` | env（`framefield.cpp:77`） | `length` | 四模式保留为对照下限（D16） |
| `AUTOREMESHER_MILS_BAND` / `_BATCH` / `_DEBUG` | env（`mixedintegerleastsquares.cpp:42/51/60`） | 解析值 | 长期（诊断口，M4-1b 留下的有用部分） |
| `AUTOREMESHER_M4_PROGRESSIVE_ROUNDING` / `_M4_CREASE_AVERAGE` | CMake option | **OFF** | 线已关（D11/D12/D16）；M6 决定删除 |

**计划里提过、但代码里不存在的开关**（不要在文档/脚本里当成已存在）：`--obj-precision`、`--island-min-ratio`、`--island-policy`、`--max-hole-edges`、`--field-brush`（只有同名 env）、`--verbose`、`--events-file`。

**开关纪律**：每个开关必须有一个「何时删除」的里程碑；没有删除期限的开关就是永久分叉。

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

## 9. 剩余工作量与执行顺序

### 9.1 剩余人日（只统计未完成项）

| 任务 | 人日 | 依赖 | 备注 |
| --- | --- | --- | --- |
| M3-3 Result/Status + `cerr` 清理 | 3.0 | M3-1 ✅ | 与 M3-4 部分并行 |
| M3-4 ProgressSink + PhaseEvent | 2.0 | M3-1 ✅ | 同上 |
| M3-5b′ 内部临时面表 CSR（无外部消费者） | 0.5 | M3-5a ✅ | 串行 |
| M3-5c 成员面表 + 消费者签名 + getter 缓存 | 3.0 | M3-5b′ | 风险最高；与 M4-8 前半重叠 |
| M4-2 P1-2 CLS union-find + 环检测 | 1.5 | — | 可并行 |
| M4-4 P1-5 条带补洞 + 参数化 | 2.0 | — | 可并行 |
| M4-6 P1-7 越界索引过滤 | 0.5 | — | 可并行 |
| M4-7 P0-1 / P0-2 / P1-8 默认值切换 + 归一化键 | 1.0 | M2 A/B ✅ | 可并行 |
| M4-8 抽取图数组化（完整化） | 4.0 | M3-5c | 串行，+2 人日风险溢价 |
| M4-9 P2-1 UV 四份拷贝消除 | 1.0 | — | 可并行 |
| M4-10 P0-1b patch 焊接与顶点归并 | 3.0 | P0-1 的 A/B ✅ | 改 `extract()` 核心重建段，不并行 |
| M5-1 QuadExtractor 拆 TU + `extract()` 编排化 | 2.5 | M4-8 | 串行 |
| M5-2 `holdSingularLines` 锥级并行 | 2.0 | M5-1 | 串行 |
| M5-3 `extractMesh` 分区并行 + `computeSeam` 队列化 | 3.0 | M5-1 | 串行 |
| M5-4 P2-3 逐顶点日志删除 + 性能复测 | 0.5 | M3-3 | 可并行 |
| M6-1 GUI 目标接回 + P0-3 / P0-4(Qt) / P2-8 / P2-9 | 4.0 | 需 Qt 机器 | 本机不可验 |
| M6-2 旧接口/旧路径删除 + R9 规则 | 1.5 | M5 | 串行 |
| M6-3 qmake 决策落地 + 源清单单点化收尾 | 1.0 | M6-2 | 串行 |
| M6-4 COMPARISON.md + 全量回测 | 1.5 | 全部 | 串行 |
| **合计（剩余）** | **37.5** | | 见下方算式 |

~~~text
M3  8.5 = 3.0 + 2.0 + 0.5 + 3.0
M4 13.0 = 1.5 + 2.0 + 0.5 + 1.0 + 4.0 + 1.0 + 3.0
M5  8.0 = 2.5 + 2.0 + 3.0 + 0.5
M6  8.0 = 4.0 + 1.5 + 1.0 + 1.5
────────────────────────────────
合计 37.5（= 全量 49.5 − 已交付 12.0；本次重估 +1.0 见 §4.3，台账见 A.3）
~~~

**未排期 backlog（不计入合计，D18）**：`autoremesher_capi` + cgo 2.0 ／ M4-1c 尺度耦合 4~6 ／ M4-5c 折痕约束 1.5~2 ＝ **7.5~10**。

**风险溢价**：M4-8 的 `simplifyGraph` 顺序敏感性是本计划最大的单点风险（+2 人日），故 37.5 的悲观值取 39.5。

### 9.2 关键路径

~~~text
M3-3/M3-4(3+2) ──┐
M3-5b′(0.5) → M3-5c(3) → M4-8(4) → M5-1(2.5) → M5-3(3) → M6-2(1.5) → M6-4(1.5)
                        ↑ M4-10(3) 必须串在 M4 内（改 extract 核心重建段）
~~~

**关键路径 ≈ 16.0 人日**（M3-5b′ → M3-5c → M4-8 → M5-1 → M5-3 → M6-2 → M6-4）。已交付的 M3-1 / M3-5a 不再在路径上。

**3 人编排**：

- A（串行链，不可拆）：M3-5b′ → M3-5c → M4-8 → M5-1 → M5-3
- B（可交错）：M3-3 → M3-4 → M5-4
- C（可并行）：M4-2 / M4-4 / M4-6 / M4-7 / M4-9 → 然后 M4-10（独占，改核心重建段）

**并行期的硬约束**：每条改动都要跑 G1/G5/G7 并留一行记录；回归先回滚开关再看代码；`artifacts/baseline/` 只由基线维护者写，实验只写 `artifacts/m4-*` 子目录。

## 10. 待验证清单（只剩未关闭的）

| # | 待验证项 | 阻塞谁 | 归属 |
| --- | --- | --- | --- |
| V4 | n 边形输入 × `--triangulate` 的面集合正确性 | P1-8 的默认切换（M4-7） | verify-runner |
| V6 | CSR 化前后峰值 RSS（head_vox、multi_part） | M3-5b′ / M3-5c 的收益证明（**5c 之后仍常驻一份 AoS 供 getter，RSS 期望值要扣除它**，见 §4.3） | 待测 |
| V7 | 岛内并行后的 `Cores kept busy` 与 wall | M5 的 G8 | 待测 |
| V8 | P0-3 / P0-4(Qt) / P2-8 / P2-9 的行为验证 | M6-1 | **需 Qt，本机不可验** |
| V11 | `.pro` 与 CMake 源清单单点化 | — | ✅ **已由 D9 关闭**：`.pro` 只保留历史快照，不再要求同步（§8.1） |
| V14 | G8 的 T1~T7 协议完整跑一轮 A/B/A/B/A/B | 性能门目前只有参考值 | 待执行 |
| V16 | ui-go 在非 Windows（Linux/macOS）的构建与冒烟 | 跨平台承诺 | task-7 |
| V17 | schema 版本兼容：v1 消费 v2 事件 / v2 消费 v1 事件 | 事件契约（§11.1）的长期可用性 | task-7 + CI |

已关闭：V1 / V2 / V3 / V5 / V9 / V10 / V12 / V13 / V15 / V18（证据索引在附录 A；数字出处见 A.1-1 / A.1-2）。

### 10.1 引用数字的出处

| 数字 | 出处 |
| --- | --- |
| stderr：head10k **3.56 MB**、multi12k **6.94 MB**、scaled10k **3.44 MB**、body15k **57 KB** | `artifacts/*/*/stderr.txt` 实测体积（本次用 Get-Item 复核） |
| 全部 A/B 指标（quads / ratio / nm / bl / comp / dev_p95 / coverage / degenerate / valence4 / wall） | **`artifacts/COMPARISON.md` 主表（§5.1 门表 / A.1-1 已全量落表）** |
| head_quad_10k：93.54% / 124 / 70 壳 / 22 退化 / 5 环 / 价4 69.12% | task-2 实测，`tools/metrics/selfcheck/*.json`（F15） |
| head_quad_20k：97.10% / 188 / 1 壳 / 2 退化 / 价4 86.48% | 同上 |
| body_quad_15k：98.97% / 12 / 1 壳 / 0 退化 / 价4 94.96% | 同上 |
| head_in：8,979 壳 / 14,520 环 / 54,014 边界边；head_clean：1 壳 / 4 边界边 / 2 环 | 同上 |
| mesh_metrics 60 条断言、`compare` 退出码 0/1/2、`--samples 50000` 使对比 23.7 s → 7.1 s | task-2 交付说明（F10） |

## 11. GUI 侧：legacy 冻结 + 可选 Go 前端

### 11.1 进程契约：`--events-json` 的 NDJSON schema v1

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

### 11.2 进阶路径：`autoremesher_capi` + cgo（未排期）
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
- **无收益**：当前 UI 一次只跑一个作业；CLI 进程启动成本 `0.03~0.04 s`（§10.1 实测 cube 全流程 0.036 s）；事件量约 200 条/次（§11.3 的 G14 抽样基数即来自这个量级）。进程边界在这个量级上不构成瓶颈。

### 11.3 事件契约门（G14 原文）与剩余承诺
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

- **剩余承诺**：V16（非 Windows 构建）、V17（schema 跨版本兼容）未做；legacy 侧的四处修复（P0-3 / P0-4 Qt / P2-8 / P2-9）随 M6-1，需 Qt 机器。

### 11.4 风险与回滚
| 风险 | 说明 | 缓解 / 回滚 |
| --- | --- | --- |
| **Qt 恢复成本** | 恢复步骤要点：① 从 `legacy/gui-qt/` 移回 `src/gui/`；② `.pro` 的 include 路径重新指回；③ CMake 的 `AUTOREMESHER_BUILD_GUI=ON` 实现为真构建（不是 warning）；④ P0-3 / P0-4 / P2-8 / P2-9 的四处修复重做或移植。估 **2~3 人日**，且**必须有一台装有 Qt 的机器** | 冻结 ≠ 删除：源码、qrc、shaders、.pro 全部原样保留；恢复是"搬回来 + 修四处"，不是重写 |
| **Go 前端跨平台打包** | 无 cgo 的好处：单一静态二进制、`GOOS/GOARCH` 交叉编译、**不需要 Qt/TBB 运行时**、不需要装 MSVC | 代价：事件 JSON 解析开销与进程启动开销（§11.2 已量化，可接受） |
| **事件 schema 漂移** | CLI 与 ui-go 各自演进，容易在不升版的情况下改语义 | 四条兼容规则（§11.1）+ G14 的未知字段注入 + V17 的跨版本兼容用例 |

## 附录 A：已交付与已结案（一行一条，过程不在此）

> **引用规则**：条目编号只到 `A.<组>-<序号>` 一级（如 `A.2-4`）。要指向条目内部的某张表或某段理由，写成「**A.2-4 + 描述**」（例如「A.2-4 的四行实测表」），**不造 `A.2-4.x` 子编号**——子编号每加一张表都要多维护一套锚点，而它指向的内容本来就能一句话说清。

### A.1 已交付

| ID | 内容 | 证据 |
| --- | --- | --- |
| A.1-1 | M0 基线 + M2 A/B：`--keep-all-islands`（P0-1）、`--relative-tolerance`（P0-2）的开关与实测 | `artifacts/{baseline,after}/`、`COMPARISON.md` |
| A.1-2 | 尺度交叉验证（1 / 1e-3 / 1e5）与 G4 三项定稿（D6 / D8） | `artifacts/inputs/head_scaled_*`、COMPARISON |
| A.1-3 | M3-1：8 模块分层、转发头统一、`gen_sources.py`、`check_boundaries.py`（G15）、D15 删除 | `src/core/`、`tools/`、`cmake/core_sources.cmake` |
| A.1-4 | M3-5a：`Csr` / `CsrView` + `SurfaceMesh` 邻接 CSR（G16 第一个正例，三份 sha 全等） | `src/core/base/csr.*`、`src/core/geom/surfacemesh.cpp` |
| A.1-5 | legacy GUI 冻结迁出（64 文件）+ ui-go 前端（7 文件，NDJSON 消费者） | `legacy/gui-qt/`、`ui-go/`、`docs/GUI-LEGACY.md` |
| A.1-6 | Qt-free CLI + NDJSON v1 事件流 + 退出码 0/1/2/3 | `src/cli/main_cli.cpp` |

### A.2 已结案裁决（负结果，一等交付物）

| ID | 结论 | 一句话理由 |
| --- | --- | --- |
| A.2-1 | **M4 的逐步取整与折痕平均都不转默认**（D11）：开关留 OFF | 三个变体全挂 G6（覆盖率）；折痕平均另挂 G4-b 与 G5 |
| A.2-2 | **M4-1b 取整调度参数化被否决**（D12）；只保留自适应轮数上限 | 调度扫描 coverage 全线下滑，`nm` 的「−32%」只在单一操作点成立 |
| A.2-3 | **V9 场算法归属**：`brush` 是承重墙，`smoothCrossField` 是死代码并已删（D14） | 强制 `always` 在 A/B 两点皆负收益；关 `brush` 后 134 s / 216 s 无有效输出 |
| A.2-4 | **M4-5b 四种折痕权重全否**（D16）；`cotan` 定为 M4-5c 的对照下限 | A 点 `nm` 108 vs 124，B 点 `nm` 8→28——方向相反，无一点两点全过 |

> **A.2-4 的四个实测值**（`artifacts/m4-5b/A_*_m.json`，A 点 = `head_in` @10000，`--samples 20000`；基线 `largest` 0.9522 / top-3 0.9573 / nm 124 / small 48）：
>
> | 权重 | quads | components | **largest** | top-3 | small | nm |
> | --- | --- | --- | --- | --- | --- | --- |
> | `length`（默认） | 4,959 | 119 | 0.9295 | 0.9352 | 87 | 136 |
> | `invlen2` | 4,645 | 77 | 0.9533 | 0.9583 | 57 | 136 |
> | `cotan` | 4,705 | 64 | **0.9596** | 0.9638 | 45 | **108** |
> | `cotanlen` | 4,862 | 66 | 0.9591 | 0.9635 | 46 | 132 |
>
> **按 D8-3 的正式判据取 `largest`**：`cotan` 0.9596 是四者最高，比基线 0.9522 高 0.74 pp。B 点（`body_clean` @15000）四者 `largest` = top-3 = 1.0000，不区分字段，区分度全在 `nm`（8 → 28/20）。

**这三个负结果共同推出 D17（优化位置原则）**：给下游求解器喂更好的初值，会被下游重新优化吃掉；只有把新的自由度/约束加进那个被重新求解的系统里才留得住。

### A.3 人日台账（D18 口径）

~~~text
全量估算 49.5（2026-11-10 重估：原 M3-5b 1.0→0.5 更名 5b′、原 M3-5c 1.5→3.0 改义为「面表成员+消费者签名」；原 5c 的「抽取图数组化」并入 M4-8）
  已交付 12.0 = M0 1.0 + M2 A/B 1.5 + M3-1/2 0.5 + M3-5a 0.5
                + M4-1 1.5 + M4-3 1.5 + M4-5 1.0 + M4-1b 0.5 + M4-5b 0.5 + legacy 0.5 + ui-go 3.0
  剩余   37.5 = 见 §9.1 的分里程碑算式
────────────────────────────────
合计   49.5
~~~

**口径**：「已交付」与「未排期」必须分栏；未排期项（7.5~10）永不计入合计；调整时只改上面这几行。

### A.4 已删死代码原文存档：`smoothCrossField`

> D15 要求「不用 `#if 0`，但原文必须可查」。代码里现在是 5 行墓碑；以下是删除前的实现原文，仅供查阅，**不得复活**（复活条件见 D14：必须在 A/B 两点都不劣于 legacy）。
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

## 附录 B：决策记录（权威，正文以它为准）

记录已裁决事项，供后续评审追溯。每条都写明**理由**与**反转条件**——没有反转条件的决策不是决策，是习惯。

> **三条阅读约定**：① 已被取代的：**D2 → D9**（GUI 移出主链，`.pro` 只留历史快照）、**D2-a 的 `.pri` 输出作废**（只剩 `cmake/core_sources.cmake`）；② **D11 / D12 / D14 / D16** 已关闭 M4 的两条线，其「反转条件」仍然有效；③ **D3 / D4 里的 `--field-brush`、`--obj-precision` CLI 开关在代码里并不存在**（只有同名 env、精度硬编码 6 位）——这两条按「默认值不得擅自改动」执行，不按「开关已提供」执行。


| ID | 日期 | 决策 | 理由 | 反转条件 |
| --- | --- | --- | --- | --- |
| D1 | 2026-11-10 | 基线 JSON（V1）完成即视为 **M0 收口**；此后 **M3（目录+契约+CSR 容器）与 M4-1（MILS 取整阈值）/ M4-5（FrameField 多锐边）并行开工** | 两者技术不互相依赖（MILS 只在 `mixedintegerleastsquares.cpp` 内部、FrameField 只在 `framefield.cpp:101-110`、CSR 容器在 `base/Csr.h`）；且 M4 的质量收益需尽快拿到，用于**校准验收门 G1/G5/G7 的阈值**（原本是按经验定的） | 若并行期出现"M4 改动在 M3 的容器上不可比对"（diff 不可读/指标对不上），或 P2 约束（见 §9.2）触发两次以上，则退回串行 |
| D1-a | 2026-11-10 | 并行期**每条 PR 必须跑 G1/G5/G7** 并在 `artifacts/COMPARISON.md` 留一行 | 并行期的唯一保障是"每个改动都自带指标"，否则回归无法归因 | 无（该约束不可撤销；撤销即等于放弃并行） |
| D1-b | 2026-11-10 | **M5（并行化）严格排在 M3 之后** | M5 要改的邻接容器正是 M3 的交付物，提前并行会让 diff 不可读、指标无法归因 | 无 |
| D2 | 2026-11-10 | qmake **保留为"仅 GUI 薄壳"**，不删 `autoremesher.pro`；core 源清单以 **CMake 为单一来源**，由 `tools/gen_sources.py` 生成 `cmake/core_sources.cmake` 与 `qmake/core_sources.pri` 两份 | 本机 **无 Qt**，qmake 路径无法验证；删除构建入口属**不可回滚动作**。而 .pro 保留的成本仅是"多一份生成物的 include" | Qt 侧 CI 恢复绿色、`autoremesher_gui` 跑通后，再提案"GUI 也迁 CMake 并删除 .pro"。届时需一次完整三平台打包验证 |
| D2-a | 2026-11-10 | 生成物（`core_sources.cmake` / `core_sources.pri`）**进版本库**，CI 增加 `gen_sources.py --check` | 生成物 diff 可读，且避免"忘了跑脚本"导致的构建差异；`--check` 保证产物与脚本一致 | 若生成物 diff 噪音过大（例如排序不稳定），改 `.gitignore` + 构建期生成，但必须同时把 `--check` 挪到 configure 阶段 |
| D3 | 2026-11-10 | `--field-brush` 默认 **legacy**（即保持现状：`smoothCrossField` 永不执行），新增显式开关 `--field-brush`（`legacy` / `always`） | 改默认值等于换算法，会动 G1/G5/G7 的**全部**基线；先有开关与 A/B，再由数据决定是否翻默认 | 若 `--field-brush=always` 在 4 模型上 G1/G5/G7 全不劣于 legacy，可提案翻默认；**翻默认必须由 task-4 新建 baseline 目录**（baseline 只由它写） |
| D4 | 2026-11-10 | `--obj-precision` 默认维持 **6 位有效数字**（与 Qt 版逐字节对齐），新增 `--obj-precision=N`（允许 6 / 9 / 17） | 默认值与 Qt 版对齐是 SHA 层 A/B 的前提；把默认改大会让 baseline 与 after 的字节比较失效 | 任何「默认改大」的动议必须**同时重建 baseline 与 after 两套 JSON**；且这是「输出保真」而非「算法质量」，验收门里单列（G13），不混进 G1/G5/G7 |
| D5 | 2026-11-10 | **M3 期间不升 C++17**，保留 §3.3 的自研 Span | 升标准会牵动 Qt 6 + TBB 2017 的既有 CI 组合，而收益（Span）40 行就够 | 若 M3 后要升，三条前置**缺一不升**：① CMake 与 .pro 同步改标准 ② TBB 2017 / Eigen 3.5 在 MSVC 下无回归 ③ 全量 G1~G12 有 before/after 两套数据 |
| D6 | 2026-11-10 | **G4 主壳占比不硬编码 0.95**，改为双条件 `max(0.85, baseline_ratio − 0.02)`；并把原先两条豁免规则合并为一条 | 硬编码 0.95 会让 multi12k（基线 47.79%）**永远过不了自己的门**，而 head10k 只比 0.95 高 0.22 pp（随时翻车）；阈值必须由基线自身校准 | 阈值随基线更新自动重算——**禁止把某次实测值当常数抄进文档或代码**（防漂移）。若某用例基线 ratio < 0.87，则 0.85 地板生效并自动失去约束力，需在 report 里明示 |
| D7 | 2026-11-10 | **指标必须连工具版本一起记录**：凡引用指标 JSON 处，必须同时记录 `metrics_meta.json` 里的 `mesh_metrics_sha256`（**现测 `9456BA7B…`**，取数 2026-11-10 19:26；工具 `mesh_metrics.py` 的 **mtime = 2026-11-10T08:03:10Z**——**mtime 晚于取数时刻即说明该值已过期，必须重测**，这条使本行具备自我失效机制；`artifacts/` 下 8 份 `metrics_meta.json` **全部为该值**，两套 JSON 一致。旧文本写的 `5DEFE0ED…` 是更早的工具版本、已失效——**照它去比 sha 的人会得出「不可比」的错误结论**） | 指标 JSON 是某个版本工具的输出，不记 sha 就无法复现，也无法判断两份 JSON 是否可比 | 换工具版本 → 必须重建 baseline 与 after 两套 JSON，并在 `COMPARISON.md` 头部更新 sha；旧 sha 的 JSON 只能作历史参考 |
| D8-1 | 2026-11-10 | **前置失效条款**：基线处于退化态（bbox 尺度误差 >5% 或 coverage <35%；scaled10k 实测 11.99% / 28.19%）时，G4-a/G4-b 的计数判据**不适用**，改由 G4-c 与 G1/G5/G7 承担；该用例上**禁止用「碎片增加」当回归证据** | 塌陷态的碎片数是被焊掉的假数字，拿它当分母等于要求修复后继续塌着 | 退化态判据本身可调（5% / 35% 为当前经验值）；基线修复后自动回到常规判据 |
| D8-2 | 2026-11-10 | `small_face_count` 阈值写法固定为 **≤ 基线 × 1.10（含等号）**；multi12k 的 100→110 记为**恰好压线、判通过** | 边界值已经出现一次（multi12k），`<` 与 `≤` 必须唯一解释，否则同一结果两个结论 | 无 |
| D8-3 | 2026-11-10 | **G4-0**：进入 G4 判定的用例必须标注 `scale_class`（unit/small/large）与 `parts_class`（single/multi），**未标注不得判定**；G4-b 按 parts_class 选 largest 或 top-k | 防下一轮拿 multi-part 的 largest 套单部件阈值（multi12k largest 0.4779 会得出「主结构塌 47%」的错误结论） | 无 |
| D9 | 2026-11-10 | **Qt GUI 冻结为 `legacy/gui-qt/`**：不进默认 target / CI / 发布，不接 P0~P2 修复（恢复所需除外）。新前端走进程边界，不链接 core | GUI 在本机**不可构建、不可验证**（H1/H2），四条待验证项（V8）因此永远悬空；而 CLI 已由 M1/M2 解耦为唯一稳定接口，GUI 不再是任何链路的必需品 | 恢复步骤见 §11.4（搬回 + 修四处 + 需 Qt 机器，估 2~3 人日）。**冻结 ≠ 删除**：源码/qrc/shaders/.pro 原样保留 |
| D10 | 2026-11-10 | Go 前端**优先进程 + NDJSON**，不优先 cgo；`autoremesher_capi` + cgo 留作进阶（§11.2） | 零 ABI 风险、离线可构建（无 cgo → 单静态二进制、不需要 Qt/TBB/MSVC 运行时）、崩溃隔离（前端崩了不影响 remesh）。capi 的收益在当前负载下为零：进程启动 0.03~0.04 s、事件量约 200 条/次 | 触发条件（满足任一才上 capi）：长会话 / 多作业并发 / 零拷贝预览（§11.2 已量化） |
| D11 | 2026-11-10 | **M4 两个开关默认 OFF**（M4-1 逐步取整 / M4-5 折痕平均），保留开关供后续 A/B；两项「修好」降级为 backlog **M4-1b（2.5 人日）/ M4-5b（1.5 人日）** | 四个变体**无一项满足全部门**：M4-1 挂 **G6**（覆盖率 −1.76 pp）与 **G7**（dev +4.1%）；M4-5 挂 **G4-b**（top-3 −2.21 pp、分量 70→119、small 48→87）、**G5**（非流形 +9.7%）、**G6**（覆盖率 −1.94 pp）；两者同时挂 G4-b/G5/G6。**三个变体全部挂 G6**（A.2-1） | M4-1b 达标的判据：nm ≤84 且 `dev_p95 ≤ 基线×1.02` 且覆盖率 ≥0.6111；M4-5b 达标的判据：top-3 ≥ baseline−2 pp 且 small ≤ baseline×1.10。达标后才重启「转默认」议题 |
| D12 | 2026-11-10 | **M4-1b 关闭为「已实现、已测量、被否决」**；`AUTOREMESHER_M4_PROGRESSIVE_ROUNDING` 默认保持 OFF；**保留**自适应迭代上限（独立健壮性修复）与 band/batch env 旋钮 + `plannedRoundCount()`（诊断工具）；**M4-1 线不再排期**，M4-1c 仅在有人投 4~6 人日做尺度耦合时启动 | 调度扫描（A.2-2）显示 coverage 全线下滑、quads 全线 −14~−17%、nm 在 target 4000 上反而变差；A.2-2 证明「nm −32%」只在 target 10000 这一个操作点成立；A.2-2 定位根因＝`t` 与尺度/保角缩放**没有联合优化**，不是调参能救的 | M4-1c 启动条件：有人投 4~6 人日 **且** M5 已交付；验收仍用 D11 三条（nm ≤84 / dev ≤×1.02 / coverage ≥0.6111），换操作点须同时换基线 |
| D13 | 2026-11-10 | **M4-1c 的验收改为两点矩阵**（A：head_in @target 10000；B：body_clean @target 15000），每点阈值由该点自己的基线推导，**两点同时满足**才通过；**只在一个点通过 = 不通过**；**不接受单点基线、不固定单一操作点** | A.2-2 已证明同一算法在 target 10000 上 nm −32%、在 target 4000 上 nm 变差——**方向会反转**。固定单点等于鼓励下一轮「挑点验收益」，而挑点会让结论翻转 | 若 M4-1c 换用第三/第四个操作点，必须**扩充矩阵**（不是替换点 A/B）；基线一律由 task-4 重建 |
| D14 | 2026-11-10 | **`smoothCrossField` 标记 dead code**（M3 搬目录时删除）；**`brush` 保留、不许删**（`off` 仅诊断且带 WARNING）；**M4-5b 解除阻塞**并从 1.5 → **1.0 人日**，折痕加权改在 FrameField 内部做、brush 原样保留；关闭 V9 | V9 实测（A.2-3）：`smoothCrossField` 的调用条件 `guidance` 恒非空 → **从未执行**，强制打开（`always`）在 A/B 两点都是负收益（A: nm +9.7%/dev +3.8%/cov −1.59pp；B: nm 8→16 翻倍）；`brush` 关掉后 seam 爆炸、cover 病态（CPU 134 s/216 s 无输出）。**「两套并列算法」的前提被证伪：一套在用 + 一套已死** | 若将来重提 `smoothCrossField`，必须先给出它在 A/B 两点都不劣于 legacy 的数据；`brush` 的删除动议一律拒绝，除非能证明 off 模式下有等效替代且不病态 |
| D15 | 2026-11-10 | **死代码删除协议**：不用 `#if 0`；删除时函数签名缩成 3~4 行墓碑（含 V9 数字与 A.4 索引），实现原文整段移入 A.4 并存档 commit sha | `#if 0` 只是把死代码藏起来，`grep`/重构/折叠里照样产生成本；而完全删掉又会丢掉「它到底做过什么」的可查性。墓碑 + 文档原文 = 索引与可复原性都有，误读成本归零 | 若 A.4 的原文与仓库状态不一致（例如 M3 前又改过该函数），以 `git show <sha>` 为准并更新本节与 sha 标注 |
| D16 | 2026-11-10 | **M4-5b 关闭为「已实现、已测量、被否决」**；`AUTOREMESHER_M4_CREASE_AVERAGE` 默认 OFF，四种权重保留为诊断（默认 `length`）；**`cotan` 的拓扑收益定为 M4-5c 的对照下限**；M4-5 线关闭，仅留 M4-5c 提案（1.5~2 人日，未排期） | 四种权重**没有一个能同时过 A/B**（A.2-4 的四行使实测表），且方向相反：A 上折痕越贴切→拓扑越好但 dev 越差，B 上→非流形边翻倍（8→28）。根因：折痕加权只改 FrameField 初值，`applyCurlCorrection` 重新优化时把差异抹平（A.2-4 段末的根因说明） | M4-5c 必须至少在 **A 点（`head_in` @ target 10000，`parts_class=single`）** 复现 nm ≤108 / `components_largest_face_ratio` ≥ **0.9596** / `small ≤45`（**A 点是 single，按 D8-3 判据取 `largest`**；0.9596 取自 `artifacts/m4-5b/A_cotan_m.json` 的 `components_largest_face_ratio`，同一份 cotan 输出的 top-3 为 0.9638 —— **旧文本里的 `.9638` 是 top-3 口径，只作历史记录，不得用于判定**），**且**同时满足 A 的 G7 ≤×1.02 与 B（`body_clean` @15000）的 G5 ≤8；启动前提 M5 已交付 |
| D17 | 2026-11-10 | **优化位置原则**（§2.5）：改进必须落在「自由度所在的那个系统」里。给下游求解器喂更好初值/调度/权重的改动，会被下游重新优化吃掉（M4 / M4-1b / M4-5b 三次实证）；能被保留的只有把新自由度/约束加进那个重新求解的系统（M4-1c 尺度、M4-5c 折痕约束） | 三个负结果的共同结构是「优化位置错了」，而不是「参数没调对」。写成原则后，评审任何优化项都先问「改的是初值还是自由度」，避免第四次同类重试 | 若出现「初值侧改动却在两个操作点上都稳定改善」的实证，本原则需要放宽并记录反例 |
| D18 | 2026-11-10 | **人日口径**：§9.1 的合计只由**算式脚注**定义（**当次**为 base 47.0 + 4.5 = 51.5；M3 第一刀结算后为 48.5，见 §9.1）；「已花」与「未排期」**必须分栏统计**；未排期项**永不计入合计**；任何调整只改脚注，禁止只改总数 | 本轮 52.0/52.5/51.5 三个数对不上，直接原因是「已花」与「未排期」混在一列里，且有人只改了总数没改明细 | 无（口径类决策不设反转条件；若要改口径，必须连脚注与分栏表一起改） |
| D19 | 2026-11-10 | **M3-1 与 M3-2 合并为「第一刀」并关闭**（目录分层 / 转发头修复 / 生成式源清单 / 边界门禁 / D15 删除，五项一次落地）；**新增 G15 边界门**（CI 必跑、退出码直用、调用形式写死为 repo 根无参数）；M3-3 / M3-4 / M3-5 未动 | 三份 sha（body_vox `9377D56A…`、body_clean `C5325AD4…`、head_in `F35B9377…`）**逐字节不变**，证明分层与清理没有改变数值行为；实花 0.5 人日（原估 3.5） | 若 M3-3/3-4/3-5 任一改动使三份 sha 变化，则按下一条 D20 流程定位到具体分层动作后再继续 |
| D20 | 2026-11-10 | **结构类改动（容器 / 遍历顺序 / 分配策略）一律按 G16 三步判定**：① 三份 sha 全等 → 逐字节等价，不重跑基线；② sha 变但拓扑四项全等 → 浮点累加顺序变化，可接受但**必须重跑基线并更新 §5.1 门表**；③ 拓扑也变 → 行为改变，按 D19 定位，不许当等价 | sha 只能说明「有差异」，不能说明「差异是什么」。**sha 全等是等价，sha 变而拓扑全等是「不同实现的同一模型」**——两者都是等价，但后者会让后续所有 A/B 永远以旧 sha 为准而恒红，所以必须重跑基线 | 若第 ③ 类（拓扑也变）被判为「可接受的等价」，必须同时改写 §5.1 门表 基线并把理由写进 COMPARISON.md；否则视为违规放行 |

**与本文其余部分的关系**：D1 改变了 §9.2 的并发编排与人日表里 M4-1/M4-5 的依赖列；D2 改变了 §8.1 的阶段表（M3 行的落地方式、M6 行的结论）。两处正文均已同步，**决策记录是权威，正文以它为准**。

**待裁决（尚无结论）**：暂无。原 Q1（`--field-brush` 默认值）、Q2（`--obj-precision` 默认位数）、Q3（是否升 C++17）已分别裁决为 **D3 / D4 / D5**。

**决策的自我约束**：D1~D5 全部写成"默认不动 + 加开关 + 数据说话 + 反转条件明确"。这不是保守，而是因为本项目的验收门（G1~G12）刚刚才被 §5.1 的真实数据校准过一轮——在门本身还不稳定时改默认值，等于用未校准的尺子量新的东西。

## 附录 C：与既有文档的关系

| 文档 | 关系 |
| --- | --- |
| `docs/BUILD-CORE.md` | 构建手册与实测记录；本文不重复其内容 |
| `docs/GUI-LEGACY.md` | legacy GUI 的冻结范围与恢复步骤 |
| `artifacts/COMPARISON.md` | 本计划所有验收数字的实际来源 |
| `AUTOREMESHER_DECOMPOSITION.md`（主检出根目录） | 原始拆解输入；其 P0/P1 条目已并入 §6，核验结论见附录 A |

**本文内部的结构**：§1 起点 ｜ §2 结构约束 ｜ §3 契约层 ｜ §4 数据模型 ｜ §5 门 ｜ §6 逐条清单 ｜ §7 并行与观测 ｜ §8 兼容回滚 ｜ §9 工作量 ｜ §10 待验证 ｜ §11 GUI ｜ 附录 A/B/C。