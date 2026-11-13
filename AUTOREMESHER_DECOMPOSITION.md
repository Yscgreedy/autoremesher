> **Historical snapshot.** This report describes the repository *before* the Qt
> removal: the `src/*.cpp` widgets, `shaders/`, `resources.qrc` and `autoremesher.pro`
> it mentions no longer exist. The L1/L2 core it analyses (`src/AutoRemesher`) is
> unchanged and is now the `autoremesher_core` library behind the CLI and the Go GUI.

# AutoRemesher 仓库拆解报告

> 对象：E:\AI\autoremesher（master @ 4a49f82f，2026-10-07）
> 方法：源码通读 + 调用链/包含图静态分析 + 构建与 CI 审查；所有行号取自当前工作区实际内容。
> 规模基线：186 次提交、7 位作者、首个提交 2020-06-18、最新 tag 1.2.0；工作区 34.2 MB，其中 thirdparty/ 占 32.6 MB（96%）；第一方代码 20,792 行（含内嵌 tiny_obj_loader.h 3,084 行）。
