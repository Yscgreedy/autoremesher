# GUI 冻结归档（legacy/gui-qt）

> 状态：**已执行 / GUI 已冻结**
> 分支 `arch-refactor`，基线提交 `4a49f82f`
> 变更范围：`git mv` 64 个文件 → `legacy/gui-qt/`；`CMakeLists.txt` 增加弃用选项门禁；
> 主链产物仍为 `autoremesher_core` + `autoremesher_cli`

## 1. 结论（TL;DR）

1. Qt GUI 的**全部**源码、资源与 qmake 工程已从仓库根/`src/` 迁出到 `legacy/gui-qt/`，共 64 个文件（`git status` 全部为 `R` 重命名，无内容删除）。
2. `src/` 现在只剩 31 个文件：`AutoRemesher/`（12 个 .cpp + 17 个头文件，29 个）、`cli/main_cli.cpp`、`tiny_obj_loader.h`。
3. `CMakeLists.txt` 不再定义任何 Qt target；新增 `AUTOREMESHER_ENABLE_LEGACY_QT_GUI`（默认 OFF，ON 直接 `FATAL_ERROR`）。
4. 删除 `build/core` 后 clean 重建通过（configure 1.1 s + build 10.4 s），`autoremesher_cli.exe` 对 `body_vox.obj` 跑出 **7954 quads**，与参考值 8101 同量级（−1.8%，差异来自工作树中未提交的 M4 改动，见 §6）。
5. `legacy/gui-qt/autoremesher.pro` 只做了**相对路径**修正，**未随任何构建验证**；恢复 GUI 需要 Qt 5.15/6 + qmake。

## 2. 为什么冻结

| 原因 | 具体事实 |
| --- | --- |
| 构建环境无 Qt | 本机（以及本任务的验收环境）只有 MSVC 19.51 + CMake 4.3.1 + Ninja，没有 Qt/qmake；GUI 源码在 CI 里从未被编译过，属于"存在但不可验证"的代码。 |
| 架构目标是与 GUI 解耦 | 改造目标是可嵌入的算法库 + 可脚本化的 CLI + C API；GUI 依赖 `QOpenGLWidget`/`QtAwesome`/`QtWaitingSpinner`，是 core 干净化的最大污染源（例如 `Q_UNUSED`/`emit` 宏与 TBB 冲突，见 `src/AutoRemesher/autoremesher.cpp:38` 的注释）。 |
| 主链可判定性 | 移出后 `src/` 里不再有任何 Qt include（`Select-String -CaseSensitive '#include <Q|QApplication|QWidget|QOpenGL|QtCore|QtGui'` → 0 命中），"core 不依赖 Qt" 从口头约定变成可 grep 的事实。 |
| 保留可追溯性 | 用 `git mv` 而不是删除，历史与 blame 完整保留，随时可恢复。 |

## 3. 被移动文件清单（基线 `4a49f82f` 中的原路径）

### 3.1 GUI 源码：`src/*` → `legacy/gui-qt/src/*`（47 个）

| 组 | 文件 |
| --- | --- |
| 入口与主窗口 | `main.cpp`, `mainwindow.cpp/.h` |
| OpenGL model shader | `modelshadermesh.cpp/.h`, `modelshadermeshbinder.cpp/.h`, `modelshaderprogram.cpp/.h`, `modelshadervertex.h`, `modelshaderwidget.cpp/.h` |
| OpenGL 单色渲染 | `monochromeopenglobject.cpp/.h`, `monochromeopenglprogram.cpp/.h`, `monochromeopenglvertex.h`, `openglbufferutil.h` |
| 网格生成器（GUI 侧） | `rendermeshgenerator.cpp/.h`, `previewmeshgenerator.cpp/.h`, `quadmeshgenerator.cpp/.h` |
| 控件 | `graphicswidget.cpp/.h`, `graphicscontainerwidget.cpp/.h`, `spinnableawesomebutton.cpp/.h`, `intnumberwidget.cpp/.h`, `floatnumberwidget.cpp/.h` |
| 对话框/日志 | `aboutwidget.cpp/.h`, `logbrowser.cpp/.h`, `logbrowserdialog.cpp/.h`, `preferences.cpp/.h` |
| 主题/工具 | `theme.cpp/.h`, `util.cpp/.h`, `version.h` |

### 3.2 资源与工程文件

| 原路径 | 新路径 | 数量 |
| --- | --- | --- |
| `shaders/*` | `legacy/gui-qt/shaders/*` | 9（model/monochrome 各 4-5 个 vert/frag/geom） |
| `resources/*` | `legacy/gui-qt/resources/*` | 3（`dust3d-vertical.png`, `crossuv.png`, `autoremesher-logo.svg`） |
| `resources.qrc` | `legacy/gui-qt/resources.qrc` | 1（内容有 4 行路径修正） |
| `autoremesher.pro` | `legacy/gui-qt/autoremesher.pro` | 1（内容有路径修正 + 冻结头注释） |
| `autoremesher.rc`, `autoremesher.icns`, `favicon.ico` | `legacy/gui-qt/` | 3 |

合计 **64** 个文件（`git status --short` 中 `R`/`R M` 共 64 条，2 条带内容修改：`.pro` 与 `.qrc`）。

### 3.3 明确**留在** `src/` 的

- `src/AutoRemesher/`（12 个 .cpp + 头文件）：core 算法栈，CLI 与 C API 都依赖。
- `src/cli/main_cli.cpp`：Qt-free 命令行前端（`autoremesher_cli` 的唯一源文件）。
- `src/tiny_obj_loader.h`：CLI 内置 OBJ 解析（`#define TINYOBJLOADER_IMPLEMENTATION`）。

## 4. 路径修正明细

### 4.1 `legacy/gui-qt/autoremesher.pro`

新位置比原位置深两层（`legacy/gui-qt/`），因此所有指向**仓库根**的路径都加上 `../../`：

| 原写法 | 新写法 | 命中次数 |
| --- | --- | --- |
| `thirdparty/...` | `../../thirdparty/...` | 22 行（QtAwesome `include()`、QtWaitingSpinner、eigen、isotropicremesher、meshoptimizer、tbb include 与 `-L.../tbb/build2/Release`） |
| `include/AutoRemesher/...` | `../../include/AutoRemesher/...` | 6 行 |
| `INCLUDEPATH += include` | `INCLUDEPATH += ../../include` | 1 行 |
| `src/AutoRemesher/...` | `../../src/AutoRemesher/...` | 24 行 |
| `src/*.cpp\|*.h`（GUI 自身） | **保持不变** | 46 行（`version.h` 原工程本就没列，移动后仍未列） |

`src/cli/` 不参与 GUI 构建，`.pro` 中没有任何引用（文件头已注明这一点）。
文件头另加了冻结说明：*未随 CMake 验证，恢复需 Qt 5.15/6 与 qmake*。

### 4.2 `legacy/gui-qt/resources.qrc`

qrc 内部路径相对**自己的位置**解析。`resources/` 与 `shaders/` 跟着一起搬，无需改动；
原本引用仓库根文档的 4 行改成上跳两级：

```
<file>ACKNOWLEDGEMENTS.html</file> → <file>../../ACKNOWLEDGEMENTS.html</file>
<file>SUPPORTERS</file>            → <file>../../SUPPORTERS</file>
<file>AUTHORS</file>               → <file>../../AUTHORS</file>
<file>CONTRIBUTORS</file>          → <file>../../CONTRIBUTORS</file>
```

（这 4 个文件仍留在仓库根，没有被移动。）

## 5. CMake 门禁

`CMakeLists.txt` 的改动（三处，均为最小编辑）：

```cmake
option(AUTOREMESHER_ENABLE_LEGACY_QT_GUI
    "Deprecated: the Qt GUI now lives in legacy/gui-qt and needs qmake, not CMake"
    OFF)
if(AUTOREMESHER_ENABLE_LEGACY_QT_GUI)
    message(FATAL_ERROR "Qt GUI 已移入 legacy/gui-qt，主链不再提供 GUI 目标")
endif()
```

- 原来的 `AUTOREMESHER_BUILD_GUI`（只 `message(WARNING)`）被替换；传 `-DAUTOREMESHER_BUILD_GUI=ON` 现在只会得到 CMake 的 "manually-specified variables were not used" 提示，不再有任何 GUI 相关分支。
- 文件头与文件尾各加注释，说明 GUI 在 `legacy/`、主链只有 core + cli。
- 默认构建产物：`autoremesher_core.lib` + `autoremesher_cli.exe`（+ 同目录 `tbb.dll`）。

## 6. 验收证据（本机实测）

环境：MSVC 19.51（`Enter-VsDevShell -arch=x64`）+ CMake 4.3.1 + Ninja 1.x，TBB 以 `build/tbb/tbb.lib` 预编译库 IMPORTED 链接。

### 6.1 clean 重建

```powershell
Remove-Item -Recurse -Force build/core
cmake -S . -B build/core -G Ninja -DCMAKE_BUILD_TYPE=Release   # 1.1 s
cmake --build build/core                                       # 10.4 s, 20 个 edge, exit 0
```

- 产物：`build/core/autoremesher_cli.exe` 974 848 B、`autoremesher_core.lib` 18 922 308 B、`tbb.dll` 187 392 B（POST_BUILD 复制）。
- `autoremesher_core.dir` 下 17 个 `.obj`（12 core + 5 thirdparty），确认是全新编译而非复用缓存：`CMakeCache.txt` 16:28:12 写入，`autoremesher_cli.exe` 16:28:23 链接完成。
- 紧随其后的 `cmake --build build/core` → `ninja: no work to do.`（构建系统自洽）。

### 6.2 CLI 实跑

```powershell
build\core\autoremesher_cli.exe -i E:\ComfyUI-aki-v3\zcode_work\char3d\remesh\body_vox.obj ^
  -o artifacts\task6-gui-legacy\body_vox_out.obj --report artifacts\task6-gui-legacy\body_vox_report.txt ^
  --target-quads 15000 --sharp-edge 60 --adaptivity 0.3 --anisotropy 1
```

| 项 | 实测值 |
| --- | --- |
| 输入 | `body_vox.obj`，4 220 294 B |
| 退出码 | 0 |
| Quads | **7954** |
| Non-quads | 74 |
| Vertices | 8084 |
| 内部耗时 | 9.58 s（wall 10.05 s） |
| 阶段 | isotropic 1.13 s / parameterize 8.30 s / merge islands 0.006 s |
| 参考值 | 8101 → Δ = −147（−1.8%） |

**关于差异**：本工作树携带 Lead 尚未提交的 M4 改动，测量时刻（2026-11-10 16:29）的 `git status` 有 8 个 `M`：`src/AutoRemesher/autoremesher.{cpp,h}`、`positionkey.{cpp,h}`、`quadextractor.{cpp,h}`、`framefield.cpp`、`mixedintegerleastsquares.cpp`。clean 重建编译的就是这份代码；测量之后这些文件仍在被继续修改，所以 7954 只是一个时间点快照值。GUI 的 64 个文件从来不参与 `autoremesher_core`（`CMakeLists.txt` 的 source list 只 `glob src/AutoRemesher/*.cpp` + 5 个 thirdparty 文件），因此 7954 与 8101 的差额只可能来自 M4 改动或参考值采集时的参数/输入差异，**不是** GUI 移除造成的。量级一致（同为 8k 级），满足验收门。

### 6.3 src/ 已无 Qt 源

```powershell
Get-ChildItem -Recurse src -File -Include *.cpp,*.h |
  Select-String -CaseSensitive -Pattern '#include <Q|QApplication|QWidget|QOpenGL|QtCore|QtGui'
# → 0 命中
```

`src/` 最终树（31 个文件）：`AutoRemesher/`（29）、`cli/main_cli.cpp`、`tiny_obj_loader.h`。
仓库根也不再残留 `resources.qrc`/`autoremesher.pro`/`autoremesher.rc`/`*.icns`/`favicon.ico`/`shaders/`/`resources/`。

## 7. 恢复步骤（**未验证**）

前提：Qt 5.15.x 或 Qt 6.5+（含 `openglwidgets` 模块）、对应版本的 qmake、可用的 C++14 编译器、TBB。

1. 取回目录（`git mv`，不要复制粘贴，避免与 `.pro` 的 `../..` 路径假设脱节）。
2. 修正 `.pro` 中相对路径（按 §4.1 的规则反向操作：`../../thirdparty/`→`thirdparty/`，`../../src/`→`src/`，`../../include`→`include`）。
3. 确认第三方 Qt 依赖存在：`thirdparty/QtAwesome/QtAwesome/QtAwesome.pri`、`thirdparty/QtWaitingSpinner/waitingspinnerwidget.{cpp,h}`（本 checkout 两者都在）。
4. 处理 TBB：`autoremesher.pro` 的 MSVC 分支写死 `-Lthirdparty/tbb/build2/Release -ltbb`，**本 checkout 不存在该目录**；应改为指向 `build/tbb`（`tbb.lib` 96 106 B、`tbb.dll` 187 392 B）或系统 oneTBB。
5. 构建：
   ```powershell
   qmake autoremesher.pro
   nmake           # MSVC；或 make -j / mingw32-make
   ```
6. 冒烟：GUI 启动 → 导入 `body_vox.obj` → target quads 15000 / sharp edge 60 / adaptivity 0.3 / anisotropy 1 → 与 §6.2 的 7954 quads 对比（GUI 走 `MainWindow::runHeadless` 与 CLI 同一条链路，差异应只在导出/统计口径）。
7. 若要把 GUI 重新并入 CMake：需要新的 `find_package(Qt6 COMPONENTS Core Widgets OpenGL OpenGLWidgets)` target 与 `qt_add_resources()`，并先删掉 `AUTOREMESHER_ENABLE_LEGACY_QT_GUI` 的 `FATAL_ERROR`。

## 8. 已知风险

| # | 风险 | 说明与影响 |
| --- | --- | --- |
| 1 | **`.pro` 完全未验证** | 本机无 Qt/qmake，`.pro` 的路径修正只做了静态推理，没有跑过一次 `qmake`/`nmake`。恢复时第一件事应是 `qmake -d` 观察路径解析。 |
| 2 | **qrc 资源路径** | 4 个根文档条目已改成 `../../`，但 `rcc` 未执行过；若恢复后 GUI 启动即缺图标/文案，先查这 4 行与 `resources/`、`shaders/` 的解析基准。 |
| 3 | **Qt6 兼容** | `.pro` 已按 Qt6 处理 `QOpenGLWidget`（`greaterThan(QT_MAJOR_VERSION,5): QT += openglwidgets`）与可选 `winextras`；`main.cpp:78-81` 的 `AA_EnableHighDpiScaling`/`AA_UseHighDpiPixmaps` 已用 `#if QT_VERSION < 6` 包住。仍未验证的点：`QtAwesome`（vendored，Qt5 时代的 icon font 用法）、`QtWaitingSpinner`、`QNativeGestureEvent`/`QScreen` 在 Qt6 的行为、`QMatrix3x3` 传值语义。Qt6 下首次编译大概率有 `deprecated` 甚至 `error` 需要逐个修。 |
| 4 | **`object_parallel_to_source`** | `.pro` 开了它，而源码路径现在有 `../../src/AutoRemesher/` 这种跨工程目录的条目；qmake 会按源码路径镜像对象目录，可能生成 `../..` 之外的对象树或触发同名冲突。恢复时若构建目录被污染，先去掉该 CONFIG 行。 |
| 5 | **TBB 链接路径失效** | §7 第 4 点：`thirdparty/tbb/build2/Release` 在本 checkout 不存在，MSVC 分支直接 `-ltbb` 会失败。 |
| 6 | **GUI 侧网格生成器已冻结** | `rendermeshgenerator`/`previewmeshgenerator`/`quadmeshgenerator` 是 GUI 的展示层封装，被移走后 core 侧若有接口变化（M4/M5），GUI 恢复时这三者需要跟着改，属于**跨里程碑的返工点**。 |
| 7 | **没有被 CI 覆盖** | `.github/workflows/*` 与 `ci/lint.sh` 现在完全不碰 `legacy/`；`legacy/` 里的代码会静默腐烂（这正是"冻结"的含义，不是缺陷）。 |

## 9. 回滚

全部变更都是 `git mv` + 两个文件的路径替换 + `CMakeLists.txt` 三处编辑，未提交：

```powershell
git checkout -- CMakeLists.txt            # 或 git restore --staged --worktree
git reset --hard 4a49f82f                 # 彻底回到基线（会同时丢弃其他并行改动，慎用）
```

恢复单个文件用 `git mv legacy/gui-qt/src/mainwindow.cpp src/mainwindow.cpp` 即可，历史不丢。
