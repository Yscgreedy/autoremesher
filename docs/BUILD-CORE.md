# AutoRemesher 算法核心构建（Qt-free）

本文档描述如何在**没有 Qt** 的机器上构建 AutoRemesher 的算法核心与自建 CLI，替代原有的
`autoremesher.pro` / qmake 流程。GUI 不在本构建范围内（`AUTOREMESHER_BUILD_GUI` 默认 OFF）。

- 工作树：`E:\AI\autoremesher\.agents\worktrees\arch-refactor`
- 基线提交：`4a49f82f`（分支 `arch-refactor`）
- 构建系统：CMake 4.3.1 + Ninja 1.13.2 + MSVC 19.51.36260（x64）

## 1. 环境

| 组件 | 版本 / 路径 |
| --- | --- |
| 编译器 | MSVC 19.51.36260（VS 18 Community，`C:\Program Files\Microsoft Visual Studio\18\Community`） |
| CMake | 4.3.1-msvc1 |
| Ninja | 1.13.2 |
| CPU | 32 逻辑核（并行编译） |
| Qt | **无**（本构建不需要） |

每条 `pwsh` 都是新进程，编译前必须重新进入 VS 开发环境：

```powershell
$vs='C:\Program Files\Microsoft Visual Studio\18\Community'
Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
```

> 注意：在 pwsh 里传 `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` 必须写成
> `'-DCMAKE_POLICY_VERSION_MINIMUM=3.5'`（带引号）。不加引号时参数会被拆成 `=3` 和 `.5`，
> CMake 报 `Invalid CMAKE_POLICY_VERSION_MINIMUM value "3"`。

## 2. 构建目标

根目录 `CMakeLists.txt` 定义两个目标：

| 目标 | 类型 | 内容 |
| --- | --- | --- |
| `autoremesher_core` | STATIC | `src/AutoRemesher/*.cpp`（12 个） + `thirdparty/isotropicremesher/{isotropicremesher,isotropichalfedgemesh,axisalignedboundingboxtree}.cpp` + `thirdparty/meshoptimizer/src/{simplifier,indexgenerator}.cpp` |
| `autoremesher_cli` | EXE | `src/cli/main_cli.cpp`（内含 `TINYOBJLOADER_IMPLEMENTATION`） |

源清单与 `autoremesher.pro` 一致（core 侧不含任何 Qt 源文件：全部 12 个 `src/AutoRemesher/*.cpp`
都不 include Qt 头）。

include 目录（PUBLIC，随 `autoremesher_core` 传递）：

```
include/  thirdparty/eigen  thirdparty/isotropicremesher
thirdparty/meshoptimizer/src  src  thirdparty/tbb/include
```

编译宏：`_USE_MATH_DEFINES`、`NOMINMAX`、`NDEBUG`。
MSVC 选项：`/O2 /Ob2 /fp:fast /bigobj /GL- /wd4819`（`/bigobj` 是 Eigen + framefield 的大 TU 必需；
`/GL-` 显式关掉 LTO 以缩短迭代时间；`/wd4819` 只是屏蔽 Eigen 头在 GBK 代码页下的乱码告警）。
刻意**没有**沿用 `autoremesher.pro` 的 `/GL /LTCG /Qpar`：LTO 会让"改一行就等一次全量链接"，
`/Qpar` 对这里手写并行的流水线没有收益。数值差异见第 5 节。

## 3. TBB：实际走的路径

**采用的路径 = A：用 CMake 单独构建 vendored TBB 2017，产出 `tbb.dll` + `tbb.lib`，根工程以 IMPORTED 目标链接，并把 DLL 拷到 exe 同目录。**

TBB 2017 的源码在本机 MSVC 19.51 下**可以**编译通过，无需退回到源码内嵌静态库。

```powershell
cmake -S thirdparty/tbb -B build/tbb -G Ninja -DCMAKE_BUILD_TYPE=Release `
      '-DCMAKE_POLICY_VERSION_MINIMUM=3.5' `
      -DTBB_BUILD_SHARED=ON -DTBB_BUILD_STATIC=OFF `
      -DTBB_BUILD_TBBMALLOC=OFF -DTBB_BUILD_TBBMALLOC_PROXY=OFF `
      -DTBB_BUILD_TESTS=OFF
cmake --build build/tbb
```

产物：`build/tbb/tbb.dll`（187 KB）、`build/tbb/tbb.lib`（96 KB，导入库）。
注意 `cmake_minimum_required(VERSION 2.8)` 在 CMake 4 下直接报错，`-DCMAKE_POLICY_VERSION_MINIMUM=3.5`
是必需的（口令见第 1 节的引号注意事项）。

**备选路径 B（已实现但本次未启用）**：`-DAUTOREMESHER_TBB_FROM_SOURCE=ON`，或干脆删掉
`build/tbb/tbb.lib`，根 CMake 就会把 `thirdparty/tbb/src/tbb/*.cpp` + `src/old/*.cpp`（剔除 test*）
+ `src/rml/client/rml_tbb.cpp` + `src/tbb/intel64-masm/*.asm` 编成静态库 `tbb_static`，
并自动 `enable_language(ASM_MASM)`。该路径的编译宏为 `__TBB_BUILD=1 USE_WINTHREAD DO_ITT_NOTIFY
_WIN32_WINNT=0x0600`，编译选项 `/GS- /wd4267 /wd4800 /wd4146 /wd4244 /wd4018 /wd4996 /bigobj`
（TBB 2017 前缀了更严格的默认 MSVC 检查，这些是它需要的宽容项；本次没有遇到需要额外
加 `/permissive` 或 `-DTBB_USE_ASSERT=0` 的情况）。

## 4. 构建与运行

```powershell
# 进入 VS 环境（见第 1 节），然后：
cmake -S . -B build/core -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/core
```

产物：

- `build/core/autoremesher_core.lib`
- `build/core/autoremesher_cli.exe`（958 KB）
- `build/core/tbb.dll`（POST_BUILD 自动拷贝）

实测耗时（32 核，增量构建目录已存在）：

| 动作 | 时间 |
| --- | --- |
| TBB 2017 全量构建（`--clean-first`，43 个 ninja 步骤） | 2.8 s |
| 根工程全量构建（`--clean-first`，18 个源文件 → 20 个 ninja 步骤） | **10.1 s** |

### CLI 用法

参数与 `src/main.cpp` 的 headless 模式完全一致：

```
-i, --input <file.obj>     -o, --output <output.obj>   --report <report.txt>
--target-quads <count>     (默认 50000)
--edge-scaling <factor>    (默认 1.0)
--sharp-edge <degrees>     (默认 90.0)
--smooth-normal <degrees>  (默认 0.0)
--adaptivity <value>       (默认 1.0)
--anisotropy <value>       (默认 1.0)
-h/--help   -v/--version
```

行为对齐点：

- OBJ 读入：`tinyobj::LoadObj` 只取 `v` 与三角面 indices，与 `MainWindow::loadObj`
  （`src/mainwindow.cpp:490-557`）逐行对应。
- 参数落库：与 `setHeadlessParams` 一样把 double 收窄成 float 再喂给 `AutoRemesher`；
  `targetTriangleCount = targetQuads * 2`，model type 固定 `Organic`（headless 没有该开关）。
- OBJ 写出：`# AutoRemesher 1.2.0` / `# https://autoremesher.dust3d.org/` 头 + `v` + `f`（1-based），
  浮点按 6 位有效数字（= `QTextStream` 默认精度，与 `saveMeshToFile` 一致）。
- 报告：`--report` 文件与 `src/main.cpp:198-213` 布局逐字相同；stdout 打印同格式摘要；
  进度 `N% done. <status>` 与 `phaseReport` 逐行输出到 **stderr**（对应 `qDebug`/`fprintf(stdout)` 的
  原行为，这里移到 stderr 是为了让 stdout 只剩下摘要）。
- 退出码：`0` 成功，`1` 参数错误（缺 `--input`/`--output`、未知选项），
  `2` 输入载入失败，`3` 重网格无结果/写文件失败。（Qt 版 headless 一律返回 `app.exec()` 的值，
  即永远 0，所以这里的退出码是新增语义。）

## 5. 验证：body_vox.obj

输入 `E:\ComfyUI-aki-v3\zcode_work\char3d\remesh\body_vox.obj`（4.2 MB），参数
`--target-quads 15000 --edge-scaling 1 --sharp-edge 60 --smooth-normal 0 --adaptivity 0.3 --anisotropy 1`
（与 `out/report_body_15k.txt` 的调用一致）。

```powershell
build/core/autoremesher_cli.exe ^
  --input 'E:\ComfyUI-aki-v3\zcode_work\char3d\remesh\body_vox.obj' ^
  --output 'build/core/verify/body_q15k.obj' ^
  --report 'build/core/verify/report_body_15k.txt' ^
  --target-quads 15000 --sharp-edge 60 --adaptivity 0.3 --anisotropy 1
```

| 指标 | Qt 参考（`out/report_body_15k.txt`） | 本 CLI run1 | 本 CLI run2 |
| --- | --- | --- | --- |
| Quads | 8129 | **8101** (-0.34%) | 8101 |
| Non-quads | 85 | **83** | 83 |
| Vertices | 8272 | **8244** (-0.34%) | 8244 |
| 总耗时 | 7.502 s | 8.216 s（进程 wall 8.65 s） | 7.145 s（wall 7.66 s） |
| 输出 OBJ 大小 | 462011 B（同名产物） | 460395 B | 460395 B |

- 两次运行输出 OBJ 的 SHA256 完全相同（`9377D56A...C0427`），即**同一构建下结果可复现**。
- 耗时 7.1–8.2 s，落在任务要求的 4–20 s 区间；quad 数与既有 report 同一量级，
  差值来自编译器/浮点路径差异，不是逻辑改动（core 源码零修改）。
- `stderr` 里的 phase report 完整（Isotropic 1.16 s / Parameterize 6.91 s / 总 8.12 s），
  其中 `Cores kept busy across the parallel phase: 1.00`——该输入是单 island，
  并行度受"island 粒度"限制，这与 Qt 版行为一致，不是本次构建引入的退化。

## 6. 已知差异 / 注意事项

1. **GUI 未构建**：`src/*.cpp` 里除 `src/AutoRemesher/` 之外的 Qt 源文件不在本构建内，
   `resources.qrc`、`autoremesher.rc`、`shaders/` 也没有接进来。`AUTOREMESHER_BUILD_GUI=ON`
   只发一条 warning。
2. **数值抖动**：参考报告由另一个工具链（macOS/Qt，`-O3 -flto`）产生；本地是 MSVC `/O2 /fp:fast`。
   quad 数差 28（0.34%）、顶点差 28，属于浮点舍入改变化简/合并边界的正常范围。
   若要逐位复现，需要 `/fp:precise` + 相同编译器/库版本，本环境不具备。
3. **未启用 LTO**：`autoremesher.pro` 的 `/GL /LTCG` 被显式关掉以缩短迭代时间。
   需要发布级优化时加 `-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` 自行开。
4. **TBB 是 DLL**：`build/core/tbb.dll` 必须与 exe 同目录（POST_BUILD 已自动拷贝）。
   换目录运行请一并拷贝；若想静态链接，用第 3 节路径 B。
5. **退出码语义是本 CLI 新增的**（0/1/2/3），Qt headless 永远返回 0。
6. **报告文件路径原样回显**：`Input file:`/`Output file:` 写的是命令行传入的字符串，
   与 Qt 版一致（绝对/相对路径都会原样出现）。
7. **单 island 输入不并行**：见第 5 节说明，属既有设计。
8. `src/cli/` 与根 `CMakeLists.txt` 是新增文件；`build/` 已被 `.gitignore` 覆盖，不会污染工作树。
