# AutoRemesher 仓库拆解报告

> 对象：E:\AI\autoremesher（master @ 4a49f82f，2026-10-07）
> 方法：源码通读 + 调用链/包含图静态分析 + 构建与 CI 审查；所有行号取自当前工作区实际内容。
> 规模基线：186 次提交、7 位作者、首个提交 2020-06-18、最新 tag 1.2.0；工作区 34.2 MB，其中 thirdparty/ 占 32.6 MB（96%）；第一方代码 20,792 行（含内嵌 tiny_obj_loader.h 3,084 行）。

---

## 0. 一句话定位

AutoRemesher 是一个 **"三角网格 → 四边形网格" 的自动重拓扑桌面工具**：C++14 + Qt5/Qt6 + OpenGL，核心算法是 Instant Meshes / MIQ 系
"**交叉场 → 参数化（quad cover）→ 等值线提取**" 的独立 C++ 重实现，外壳是一个实时预览 5 个中间阶段的 Qt 应用。

它的工程形态可以一句话概括：**一个可独立运行的单体桌面程序，内部按"算法内核 / 流水线编排 / 工作线程 / OpenGL 表现"自然分成了四层，但从未被物理拆成库。**

---

## 1. 分层架构

`@
┌──────────────────────────────────────────────────────────────────────────┐
│ L4 表现层（Qt Widgets + OpenGL 3.3 core）                                 │
│  main.cpp(246)  mainwindow(1215+187)  theme(410)  logbrowser                │
│  modelshader{vertex,mesh,binder,program,widget}  monochromeopengl{program,   │
│  object}  previewmeshgenerator(391)  rendermeshgenerator(207)               │
│  graphicswidget / graphicscontainerwidget / intnumberwidget / floatnumber-  │
│  widget / spinnableawesomebutton / aboutwidget / preferences / util         │
│  shaders/ : 9 个 GLSL（core 330 与 legacy 110 双份）                        │
├──────────────────────────────────────────────────────────────────────────┤
│ L3 任务层（Qt 线程 + 参数聚合）                                            │
│  QuadMeshGenerator(113+152)   ← QThread，包装 AutoRemesher + 进度转发       │
│  RenderMeshGenerator(207)     ← QThread，任意 n 边面 → GPU 顶点/索引        │
│  PreviewMeshGenerator(391)    ← QThread，一次产出 3 份预览网格              │
├──────────────────────────────────────────────────────────────────────────┤
│ L2 编排层（纯 C++，零 Qt 依赖）                                            │
│  AutoRemesher(1059+269)  ← 唯一门面：参数、岛级并行、进度、阶段计时        │
│  MeshSeparator(154)      ← 连通分量拆分 / 有向边→面表                      │
│  IsotropicRemesher(89)   ← 对 thirdparty/isotropicremesher 的适配封装      │
├──────────────────────────────────────────────────────────────────────────┤
│ L1 算法内核（纯 C++，零 Qt 依赖）                                          │
│  SurfaceMesh(159)  FrameField(225)  SingularitySimplifier(468)              │
│  QuadParameterizer(716)  Parameterizer(436)  QuadExtractor(4553)           │
│  ConstrainedLeastSquares(416)  MixedIntegerLeastSquares(541)                │
│  Vector2/Vector3/Double/Radians/PositionKey/Progress                        │
├──────────────────────────────────────────────────────────────────────────┤
│ 支撑：thirdparty/  Eigen 3.5.0(MPL2)  TBB 2017.0(Apache2)  meshoptimizer 1.2 │
│        isotropicremesher(MIT)  QtAwesome  QtWaitingSpinner  tiny_obj_loader  │
│ 构建：autoremesher.pro（qmake，单文件硬编码所有源文件）+ 3 平台 CI          │
└──────────────────────────────────────────────────────────────────────────┘
`@

**依赖方向严格单向**（由 include 图实测）：

`@
mainwindow ─→ quadmeshgenerator ─→ AutoRemesher ─→ Parameterizer ─→ FrameField ─→ ConstrainedLeastSquares ─→ Eigen
     │                          │              ├─→ SingularitySimplifier ─→ SurfaceMesh
     │                          │              ├─→ QuadParameterizer ─→ MixedIntegerLeastSquares ─→ CLS
     │                          │              └─→ QuadExtractor ─→ MeshSeparator / PositionKey
     ├─→ rendermeshgenerator / previewmeshgenerator / modelshader* / theme
     └─→ tiny_obj_loader（唯一实现 TU：mainwindow.cpp:66-67）
`@

L1/L2 **没有一处 #include <Q…>**，只依赖 STL + Eigen + TBB。这是仓库里最有价值的既有资产：算法内核事实上已经是可复用的库，只是缺一个可安装的构建目标。

---

## 2. 模块清单

| 层 | 模块 | 行数 | 职责 | 一句话评价 |
|---|---|---|---|---|
| L1 | QuadExtractor | 4553 | UV 整数等值线 → 连接图 → 环搜索出四边形 → 21 步拓扑修缮 | **全仓最大单体**，改动风险最高 |
| L1 | QuadParameterizer | 716 | 交叉场刷树、裁缝（seam）、各向异性、quad cover 整数 UV 求解 | 算法最重、串行段最长 |
| L1 | MixedIntegerLeastSquares | 541 | 4-pass 格基消元 + 整数变量逐轮固定 | 取整阈值实现有缺陷（见 §8-P1） |
| L1 | SingularitySimplifier | 468 | 奇异点索引配对、指数沿路径搬运、局部重平滑 | 每对候选 4 次全网格扫描 |
| L1 | Parameterizer | 436 | 单岛编排：法向 → 尺度场 → 场 → 奇异点 → 各向异性 → cover | 有一整段被注释的死代码 |
| L1 | ConstrainedLeastSquares | 416 | 带等式约束的稀疏最小二乘（消元 + 拉格朗日双路径） | 正规方程法，条件数平方 |
| L1 | FrameField | 225 | 4-对称周期场全局平滑（曲率张量引导） | 无 cotan 权重 |
| L1 | SurfaceMesh | 159 | corner（半边）拓扑基座 | 每顶点一个 vector，非流形处理靠"降级为边界" |
| L1 | MeshSeparator | 154 | 岛拆分 + 有向边→面打包表 | 打包键 + 并行排序，设计聪明 |
| L2 | AutoRemesher | 1059+269 | 门面 + 岛级并行 + 加权进度 + 阶段计时报告 | 单文件承载 4 类职责，应再分 |
| L2 | IsotropicRemesher | 89 | 第三方重网格器的类型/回调适配 | 干净 |
| L3 | QuadMeshGenerator | 113+152 | Qt 线程壳，C 函数指针回调 → 信号 | 干净 |
| L3 | PreviewMeshGenerator | 391 | 3 份预览（decimated / isotropic / param） | 半径硬编码、UV 用"修复前"版本 |
| L3 | RenderMeshGenerator | 207 | n 边面 → 归一化顶点 + IBO 扇出 + 边去重 | 索引化设计正确 |
| L4 | MainWindow | 1215+187 | 唯一编排者：文件、参数、5 个预览、进度、导出、CLI 复用 | 状态机靠 4 个 bool + 1 个队列，已到可维护上限 |
| L4 | ModelShader* | 379+394+289+157+46 | 顶点格式 / 网格数据 / VAO-VBO 绑定 / GLSL 程序 / 相机交互 | #pragma pack(1) 与属性偏移手工对齐 |
| L4 | theme | 410 | 调色板、字号、QtAwesome 单例、紧凑样式表 | 含一批 Dust3D 遗留常量 |
| 支撑 | tiny_obj_loader.h | 3084 | OBJ 解析（单头，v1.3.0） | 只取顶点 + 三角索引，丢弃 uv/normal/mtl |

---

## 3. 主流水线拆解（AutoRemesher::remesh()，autoremesher.cpp:548-1057）

### 3.1 阶段表

| # | 行 | 阶段 | 并行 | 产物 | 备注 |
|---|---|---|---|---|---|
| 0 | 552-564 | 入参校验 | — | — | 空网格 / targetTriangleCount==0 早期返回 false |
| 1 | 570-573 | 由面积反推体素边长 | — | m_voxelSize | sqrt(area/target/(0.866*0.5))，正三角形面积系数 |
| 2 | 575-580 | 拆连通岛 | 串行 BFS | trianglesIslands | MeshSeparator；边表打包 + parallel_sort |
| 3 | 604-636 | 构建岛上下文（顶点压实） | TBB per island | IslandContext[] | 每岛独立索引空间 |
| 4 | 641-750 | 等距重网格（含预降采样 + 曲率自适应场） | TBB per island | m_isotropic*, m_decimated* | 单岛内：decimateIfTooDense → 目标边长场 → IsotropicRemesher |
| 5 | 753-880 | 参数化 + 四边形提取 | TBB per island | 每岛 unique_ptr<Parameterizer/QuadExtractor> | 岛内串行；异常被 catch 后**跳过该岛** |
| 6 | 882-944 | 合并岛（预览数据 + 四边形） | 串行 | m_remeshedVertices/Quads, m_isotropic* | 顶点偏移累加 |
| 7 | 946-1044 | 阶段计时报告 | — | m_phaseReport | stderr + GUI 日志窗 |

### 3.2 岛内三阶段的进度预算（硬编码）

`@cpp
parallelPhaseBegin = 0.03, parallelPhaseEnd = 0.95        // autoremesher.cpp:78-79
islandResampleEnd = 0.17, islandParameterizeEnd = 0.50    // :84-85，其余 0.50→1.0 给提取
`@

进度是**按岛三角数加权的和**（:646-650），每次 updateProgress 增量更新 m_progressSum 并做 permille 去抖（:498-503），状态文案取"最慢的岛"（:507-513）。设计正确，代价是 updateProgress 里有一把全局 mutex（:482）——所有岛的所有步进都过这把锁。

### 3.3 算法内核链路（每岛）

`@
三角网格（等边长）
 └ Parameterizer::parameterize                 parameterizer.cpp:294
    ├ 顶点法向（面积加权）                     :319-342
    ├ 曲率自适应面尺度场                       :353 → computeFaceScalingField :223
    ├ SurfaceMesh 拓扑（corner 邻接）          :359
    ├ FrameField::create（2F 未知数稀疏解×5）   :370 → framefield.cpp:86
    ├ SingularitySimplifier::simplify（原地改场）:380-386
    ├ 各向异性场 → su/sv per face              :393-397 → computeFaceAnisotropyField :146
    └ QuadParameterizer::parameterize          :407 → quadparameterizer.cpp:643
       ├ 场平滑（guidance 为空才跑）/ 生成树刷场
       ├ 每角点 90° 旋转 + 特征边 U/V 约束
       ├ applyCurlCorrection（3F 未知数 CLS）
       ├ computeSeam（BFS 内球 + 悬垂割缝修剪，O(C²)）
       ├ solveQuadCover（4C 变量，整数平移 2 步长）
       │   └ MixedIntegerLeastSquares：4-pass 消元 → kernel 投影 → 逐轮固定 → CLS 求解
       └ buildResultUv（|x−round(x)|<0.01 吸附）
UV（每三角 3 个 Vector2）+ 奇异点索引
 └ QuadExtractor::extract                      quadextractor.cpp:48，21 阶段
    ├ 整数 isoline 求交 + PositionKey 去重      :759-932
    ├ holdSingularLines（饥饿锥补线）           :934-1257
    ├ 图清理（塌短边/三角/端点，折 2 度点）      :291-484
    ├ 环搜索抽面（7 层嵌套，面尺寸 4→5→6→7→3）  :486-757
    ├ fixHoles / removeNonManifoldFaces         :1295-1478
    ├ smoothAndProject(5)（AABB 树投影回原网格）:1541-1668
    └ 12 个专项修缮 pass（六/七边拆分、三价塌陷、五边合并…）
四边形网格（3..7 边多边形，vertex 已压实 + 重映射）
`@

---

## 4. 关键接口契约（拆解时最该先固化的一层）

| 契约 | 现状 | 影响 |
|---|---|---|
| 网格表示 | vector<Vector3> + vector<vector<size_t>>（每面变长） | 无索引宽度约定、无 CSR，遍历有指针追逐；size_t 在 64 位是 8 字节 |
| 输入指针 | Parameterizer/QuadExtractor 构造传 const vector*（不持有） | 生命周期靠调用方，无所有权语义 |
| UV 所有权 | takeTriangleUvs() 移交 unique_ptr | 与 originalTriangleUvs() 返回的**同内容拷贝**并存（parameterizer.cpp:415-416） |
| 进度回调 | std::function<void(float,const char*)>；门面层用 C 函数指针 + void* tag | 两套风格并存；必须在工作线程调用，GUI 侧靠信号跨线程 |
| 失败语义 | remesh() 可能返回 false；岛内 parameterize/extract 失败只 stderr + 跳过 | 无错误码/无 partial-result 描述；extract() **恒返回 true**（quadextractor.cpp:288） |
| 中间产物 | 门面暴露 isotropic / decimated / UV / 连接 / 奇异点 6 组 getter | 只为预览服务，却让 L2 表面接口膨胀到 16 个 getter |

---

## 5. 并发、进度与计时模型

- **两级并行**：外层 tbb::parallel_for 以"岛"为粒度（autoremesher.cpp:738、876）；内层在单岛内用 TBB 做数据并行（法向、曲率、场初始化等）。**岛内求解器全串行**（Eigen 单线程）。
- **串行部分就是性能上限**：quadparameterizer 的 cover 求解、singularitysimplifier 的配对、SurfaceMesh 构造、MeshSeparator 的 BFS 都不并行。模型若只有一个连通岛（常见），外层并行完全退化为单线程。
- **可观测性做得比多数同类项目好**：phaseReport 同时打印"累加时间"与"墙钟时间"，并算 Cores kept busy = accumulated / wall（autoremesher.cpp:1030-1040）；accumulateStageTime 按 order = stageOrder + fraction 排序输出（:534-546），使多岛交错时报告仍按流水线顺序阅读。
- **三线程 + 嵌套并行的风险**：GUI 线程、3 个 QThread（quad/render/preview）、TBB 线程池同时存在；QuadMeshGenerator 在 GUI 线程被 delete（mainwindow.cpp:1167）但对象已 moveToThread，属 Qt 亲和性边界用法。

---

## 6. 数据模型与性能热点

| 热点 | 位置 | 量级 |
|---|---|---|
| QuadExtractor 环搜索 | quadextractor.cpp:573-753 | 7 层嵌套 + 每候选 3 次 set 查询，最坏 O(V·d⁶) |
| holdSingularLines | :1060、:1166 | 每个饥饿锥遍历全部连接的 std::map，O(S·E) 串行 |
| computeSeam 割缝修剪 | quadparameterizer.cpp:406-425 | while(changed) × 全角点扫描，最坏 O(C²) |
| SingularitySimplifier::cancelPair | :335-336、350、384-385 | 单对候选内 4 次全网格 vertexCharges()，配对阶段 O(k²·C) |
| 场平滑每轮整份拷贝 | quadparameterizer.cpp:152 | 40 轮 × 2F double |
| 岛结果整份拷贝 | mainwindow.cpp:1196-1197、quadmeshgenerator.cpp:98-100 | 结果网格峰值内存 ×2~3 |
| OBJ 写出精度 | mainwindow.cpp:1033-1045 | QTextStream 默认 6 位有效数字，大坐标几何被量化 |
| PositionKey 量化 | positionkey.cpp:26、39-41 | 固定 1e-5 绝对精度、long 在 Windows 为 32 位，坐标 >21474 溢出 |

---

## 7. 构建 / 依赖 / CI（工程化拆解）

- **构建**：单文件 autoremesher.pro（298 行），约 60 组 SOURCES/HEADERS **逐文件硬编码**；CONFIG += release 写死（:9）；所有平台都用 -O3/-LTO，Linux 额外 -march=x86-64-v2，MSVC 用 /O2 /GL /Qpar /fp:fast。**无 CMakeLists、无 install 语义、无库目标**。
- **转发头机制**：include/AutoRemesher/* 18 个无扩展名文件各转发一行，使 #include <AutoRemesher/Vector3> 可用。其中 12 个用 "../src/…"（相对自身是错的），靠编译器 "quoted include 回退搜 -I" 才成立；QuadRemesher 指向不存在的 quadremesher.h（死头）。
- **依赖四分叉**：TBB 在 macOS 走 Homebrew /opt/homebrew、Linux -ltbb、MSVC 用 vendored **TBB 2017**、MinGW 用系统 **oneTBB**（-ltbb12）；头文件靠 __has_include(<oneapi/tbb/…>) 双路兼容（autoremesher.cpp:44-69）。三平台跑的不是同一套 TBB。
- **版本注入链**：.pro 变量 → PROJECT_DEFINED_* 宏 → version.h → 代码。version.h **无默认值**，脱离 qmake 直接编译会失败。
- **CI**：development.yml 只跑 clang-format（macOS）；release.yml（534 行）三平台构建 AppImage / zip / dmg，**冒烟测试只断言"输出文件存在"**，不校验四边形数/报告内容，且三平台都漏传 --anisotropy；无 gh-release，tag 只产出 workflow artifact。
- **合规**：项目 MIT；ACKNOWLEDGEMENTS.html（192 KB，打进 qrc 并在 About 展示）仍保留 CoMISo(GPLv3)、libQEx、CGAL、Geogram 等**已被移除的 GPL 组件全文**，与实际二进制（Eigen MPL2 / TBB Apache2 / 其余 MIT）不符，属误导性声明 + 死文本。

---

## 8. 风险清单

### P0（会静默产出错误结果）

1. **removeIsolatedFaces 只保留最大岛**（quadextractor.cpp:1439-1449）——不判断岛数，多部件模型（含独立小零件）的其余部分被无条件丢弃。
2. **PositionKey 固定 1e-5 绝对量化 + 向零截断**（positionkey.cpp:26、39-41）——小尺度模型（<1e-3 单位）所有交点被压成极少数整数点，连接图直接塌陷；大尺度模型 long 溢出（Windows 32 位 long）。
3. **源/结果网格靠"结果是否为空"区分**（mainwindow.cpp:932）——第一次重网格成功后，源网格不再被识别，之后入队的源网格被当成结果处理。

### P1（算法质量 / 稳定性）

4. **MixedIntegerLeastSquares 取整阈值退化**（mixedintegerleastsquares.cpp:456-469）：threshold = max(d+0.001, 1.0)，而 d ∈ [0,0.5]，阈值恒为 1.0 → **首轮就把全部整数变量固定**，"逐次取整 + 重解"退化为一次四舍五入，maximumIterations=100 形同虚设。
5. **ConstrainedLeastSquares 仿射替换链无环检测**（constrainedleastsquares.cpp:214-217 + :159）——若出现 a→b 后又 b→a，resolve 会死循环。
6. **FrameField 一个面多条锐边时后写覆盖**（framefield.cpp:101-110）——面被锁定却只服从最后一条边的方向；传输能量权重恒为 1（:179-190），场结果依赖三角剖分密度。
7. **两套场算法叠加语义不明**：QuadParameterizer 只有在 guidance == nullptr 时才自己平滑（quadparameterizer.cpp:680-681），而 Parameterizer 总是传非空场，所以 smoothCrossField 在本流水线里**永不执行**，但 brushFieldAlongSpanningTree（:682）无条件覆盖 FrameField 的结果。
8. **SurfaceMesh 放过越界顶点索引**（surfacemesh.cpp:51-52）——只跳过入链、保留面；下游 cornerVertex() 会返回越界索引造成越界读。
9. **extract() 恒返回 true**（quadextractor.cpp:288）→ autoremesher.cpp:855 的失败分支不可达，失败无上报通道。
10. **边界环 >65 直接放弃补洞**（quadextractor.cpp:1300-1303）+ 非流形启发式整面删除（:1451-1478）——大洞永久敞开，且两者可能反复震荡。

### P2（性能 / 可维护性）

11. 死代码：computeConformalScaling（parameterizer.cpp:42-138，调用点被注释于 :388-389）、Radians/PositionKey 无引用、QuadRemesher 悬空转发头、resources/autoremesher-logo.svg 无引用。
12. UV 双份拷贝（parameterizer.cpp:415-416，3F·16 字节×2）；[Param] 预览用的是"修复前" UV（previewmeshgenerator.cpp:200-203），与真实抽取不一致。
13. GUI 线程 delete 已移入 worker 线程的对象（mainwindow.cpp:1167）；saveMesh 成功后不复位 m_saved（:615-645）。
14. fixHoles/searchBoundaries 逐顶点写 std::cerr（quadextractor.cpp:1415-1428）——每次补洞可能刷屏数万行，与多岛并行交错污染终端。
15. 进度分数、奇异点球半径、颜色、线宽等大量魔数硬编码，且与实测耗时/模型尺度脱钩。

---

## 9. 重构与演进路线（按收益/风险比排序）

| 步 | 动作 | 具体做法 | 收益 |
|---|---|---|---|
| 1 | **把 L1+L2 物理抽成 autoremesher_core 静态库** | 加 CMakeLists.txt（或 .pri）：core = src/AutoRemesher/* + 转发头 + Eigen/TBB/meshoptimizer/isotropicremesher；app = Qt 层链接 core；version.h 加 #ifndef 默认值 | 内核可被插件、CLI、服务端复用；转发头不再依赖编译器回退 |
| 2 | **修 P0 语义** | removeIsolatedFaces 增加 keepAll/面积阈值；PositionKey 改 int64 + 相对包围盒量化；MainWindow 用显式 enum MeshSlot 替代"是否为空"判断 | 多部件、小尺度模型不再静默出错 |
| 3 | **修 P1 算法缺陷** | MILS 阈值改 min(d)+0.05（真正逐轮固定）；CLS 加环检测；FrameField 多锐边加权平均 + cotan 权重；SurfaceMesh 构造期丢弃越界面；extract() 返回真实成功标志 | 四边形质量与鲁棒性提升，失败可观测 |
| 4 | **数据模型换 CSR** | vector<vector<size_t>> → flat indices + offsets；cornersAroundVertex 改 CSR；抽取图改邻接数组 | 去掉 O(F) 次小堆分配与指针追逐，为并行铺路 |
| 5 | **并行化三个串行核** | cover 求解按顶点环/割缝连通块分块；cancelPair 增量维护 vertexCharges；computeSeam 修剪改队列式一轮 O(C) | 单岛大模型（当前最坏情形）获得实际加速 |
| 6 | **建立回归基线** | 用 common-3d-test-models 跑 5~10 个模型，断言 quad 占比、非流形边数、边界环数、包围盒偏差、各阶段耗时区间；输出几何哈希做跨平台差异告警 | 让第 2~5 步可验证，替代"文件存在即通过" |
| 7 | **清理合规与卫生** | 重写 ACKNOWLEDGEMENTS.html 为实际依赖清单 + 许可证全文；随包分发 licenses/；删 thirdparty/{eigen/{doc,test,bench,demos,unsupported},tbb/{doc,examples}}（约 9 MB） | 体积与合规声明一致 |

---

## 10. 阅读/验证路线

1. 先读 src/AutoRemesher/autoremesher.h（269 行，门面 + 参数 + 阶段报告结构）→ remesh()（autoremesher.cpp:548-1057）。
2. 再读 parameterizer.h + parameterize()（parameterizer.cpp:294-434）：这是内核的"目录页"。
3. 想改质量：framefield.cpp（场）→ quadparameterizer.cpp:447-608（cover）→ quadextractor.cpp:486-757（抽面）。
4. 想改性能：先用 phaseReport 定位；单岛模型瓶颈几乎总在 solveQuadCover 与 holdSingularLines。
5. 快速验证命令（读代码后可直接跑）：

`@bash
qmake && make -j$(nproc)          # 或 nmake -f Makefile.Release / 先 cmake 建 TBB
./autoremesher --input armadillo.obj --output out.obj --report r.txt \
  --target-quads 50000 --edge-scaling 1.0 --sharp-edge 90 --smooth-normal 0 \
  --adaptivity 1.0 --anisotropy 1.0
`@

--input 一出现即进入 headless 分支（main.cpp:141），但**仍会构造完整 GUI + 请求 OpenGL 3.3 core**（main.cpp:143-146、172），故无显示环境时需 QT_QPA_PLATFORM=offscreen QT_OPENGL=software（CI 的 Windows 分支即如此，release.yml:330-331）。

---

## 11. 结论

- **算法内核是这个仓库真正的资产**：SurfaceMesh/FrameField/Parameterizer/QuadParameterizer/QuadExtractor 构成一条完整、零 Qt 依赖、带阶段计时的 MIQ 系实现，可独立复用。
- **最大的结构性问题是"没有边界"**：内核从未被打包成库，AutoRemesher 门面同时承担参数、并行、进度、计时；QuadExtractor 单文件 4553 行承载 21 个阶段 + 12 个修缮 pass，是所有改动的风险集中点。
- **最值得优先修的不是性能而是语义**：三个 P0（只留最大岛、绝对量化、空值判据）都会**静默**产出错误结果，且都能用小改动修掉。
- **工程化短板集中于可复现性与验证**：单文件 qmake、版本宏外置、TBB 四分叉、CI 只断言"文件存在"、无单元测试。第 1、6 步（抽库 + 建基线）是其余所有重构的前置条件。
