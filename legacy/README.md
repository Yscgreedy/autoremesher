# legacy/ — 冻结的历史组件

本目录存放**已退出构建主链**的历史组件。这里的东西不参与任何自动化构建，
由验收门、CI、工具脚本都不依赖；保留的唯一目的是可追溯与（未验证的）恢复。

| 子目录 | 内容 | 状态 |
| --- | --- | --- |
| `gui-qt/` | 原 Qt/OpenGL GUI 的全部源码、资源、shader 与 qmake 工程 | **冻结**，未随 CMake 验证 |

当前构建主链只有两个产物（见仓库根 `CMakeLists.txt`）：

```
autoremesher_core   STATIC   src/AutoRemesher/*.cpp + thirdparty/{isotropicremesher,meshoptimizer}
autoremesher_cli    EXE      src/cli/main_cli.cpp
```

## 规则

1. 不要从 `legacy/` 反向 include 或 link 到主链 target。
2. 不要在 `legacy/` 下新增功能；冻结即只读。
3. 主链若需要 `legacy/` 中某个算法片段，先把它搬回 `src/`（或新模块）并补 CMake target。
4. 恢复 GUI 的步骤与风险见 [../docs/GUI-LEGACY.md](../docs/GUI-LEGACY.md)。

## gui-qt 快速索引

- 工程文件：`gui-qt/autoremesher.pro`（qmake，**未验证**）
- 源码：`gui-qt/src/*.cpp|*.h`（47 个文件：main.cpp + 主窗口 + OpenGL 渲染 + 控件）
- 资源：`gui-qt/resources.qrc`、`gui-qt/resources/`（3 个）、`gui-qt/shaders/`（9 个）
- 平台图标：`gui-qt/autoremesher.rc`、`gui-qt/autoremesher.icns`、`gui-qt/favicon.ico`
