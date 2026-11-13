# ui-go — AutoRemesher 的可选 Go 前端

一个可选的本地 Web 前端：**不链接重网格核心，不引入 cgo，不引入任何第三方 Go 模块**，只通过进程边界调用 `autoremesher_cli`。
主链（`autoremesher_core` + `autoremesher_cli`）完全 Qt-free；本目录是额外的一层，删掉它不影响核心、CMake 或 CLI。

```
浏览器 --HTTP/SSE--> ui-go (Go, stdlib) --argv/stdout/stderr--> autoremesher_cli --> autoremesher_core
```

## 1. 构建与启动

```bash
cd ui-go
go vet ./...                                  # 只依赖标准库，无 go.sum
go build -o bin/ui-go.exe .                   # 产物只在 bin/，见 .gitignore
# 或直接：go run . -addr 127.0.0.1:8787

# 注意：不带 -o 的 go build ./... 会把 ui.exe 丢在包目录（已被 .gitignore 排除）。
# 仓库根 .gitignore 有 Fortran 的 *.mod 规则，会连 go.mod 一起忽略，
# ui-go/.gitignore 里用 "!go.mod" / "!go.sum" 显式取回。
```

启动（默认监听 `127.0.0.1:8787`，只绑本机）：

```bash
./bin/ui-go.exe
# 等价
./bin/ui-go.exe -addr 127.0.0.1:8787
```

然后打开 <http://127.0.0.1:8787/>。

CLI 查找顺序（`/api/health` 的 `cli_path` 显示最终结果）：

1. `-cli <path>`
2. 环境变量 `AUTOREMESHER_CLI`
3. `./build/core/autoremesher_cli(.exe)`、`../build/core/...`、`../../build/core/...`
4. `PATH` 里的 `autoremesher_cli`

命令行开关：

| 开关 | 默认 | 说明 |
|---|---|---|
| `-addr` | `127.0.0.1:8787` | 监听地址；**默认只绑本机**（无鉴权） |
| `-cli` | 空 | CLI 路径，优先于环境变量与自动查找 |
| `-root` | 当前目录 + 临时目录 | 允许 `/api/fs` 浏览的根目录，可重复 |
| `-max-jobs` | `32` | 保留的作业数上限，超出淘汰最旧的已完成作业 |

## 2. HTTP API

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/` | 内嵌单页（`go:embed`，无外链、无 CDN） |
| GET | `/api/health` | `ok` / `version` / `cli_path` / `cli_found` / `events_json` / `degraded` / `degrade_note` / `probe_note` / `jobs` / `uptime_seconds` / `roots` |
| POST | `/api/jobs` | 提交作业，返回 `{id,status,input,output}`（202） |
| GET | `/api/jobs` | 作业列表：状态、参数、耗时、quads / non_quads / vertices |
| GET | `/api/jobs/{id}/events` | SSE：`progress` / `log` / `phase` / `done`（先回放历史，再推实时） |
| POST | `/api/jobs/{id}/cancel` | 杀子进程，状态转 `canceled` |
| GET | `/api/jobs/{id}/obj` | 输出 OBJ 文本（上限 64 MiB） |
| GET | `/api/fs?dir=...` | 只列 `.obj`；越权路径返回 403 |

### POST /api/jobs 请求体

```json
{
  "input":  "C:/models/foo.obj",
  "output": "C:/models/foo-remesh.obj",
  "target_quads": 50000,
  "edge_scaling": 1.0,
  "sharp_edge": 90.0,
  "smooth_normal": 0.0,
  "adaptivity": 1.0,
  "anisotropy": 1.0,
  "extra_args": ["--keep-all-islands"]
}
```

* `output` 省略时为 `<输入名>-remesh.obj`，与输入同目录。
* `extra_args` 透传给 CLI；其中 `-i/-o/--report/--events-json` 会被剥离，防止调用方改写输出路径或搞乱事件流。
* 值域按 `autoremesher_cli --help` 的范围校验，越界返回 400 并指出字段。

### SSE 事件

每条 `data:` 一行 JSON：

```json
{"type":"state","status":"running"}
{"type":"log","seq":2,"time":"16:36:56.488","line":"Loaded 114 vertices, 224 triangles"}
{"type":"progress","seq":3,"percent":0,"status":"Computing voxel size"}
{"type":"phase","seq":40,"lines":["Remeshing uniformly: 61 ms"]}
{"type":"done","status":"done","quads":248,"non_quads":0,"vertices":250,"exit":0,"elapsed_ms":62}
```

* `progress` 一定带 `percent`（0 也是有效值）。
* `done` 是终止事件，`status` 为 `done` / `failed` / `canceled`，带四项统计。
* 晚到的订阅者会先收到 `state` 与最近 400 条事件回放；作业已结束则回放后立即关闭连接。

## 3. 子进程契约与降级

参数末尾追加 `--events-json`，CLI 把 NDJSON 事件写到 **stderr**，每行一个对象：

| type | 字段 |
|---|---|
| `progress` | `percent`、`status` |
| `phase` | `lines`（每阶段耗时报告） |
| `done` | `quads`、`non_quads`、`vertices`、`exit` |
| `error` | `message` |

stdout 的报告块（`Quads:` / `Non-quads:` / `Vertices:`）是第二来源；两者都缺时，服务端直接数输出 OBJ 的面（第三来源）。

**开关状态（已在本机验证）**：本 checkout 的 `build/core/autoremesher_cli.exe` 已实现并验证该开关——
cube 用例输出 62 条 `progress` + 1 条 `phase`（`lines` 55 行）+ 1 条 `done`
（`quads=164 / non_quads=0 / vertices=166 / exit=0`），`schema_version=1`，逐行可 `ConvertFrom-Json`。
因此正常路径无需降级；下面两道闸门保留，用于「二进制比源码旧」或「探针与运行之间被替换」的情况。

**降级有两道闸门：**

1. **启动探测**：`autoremesher_cli --events-json --version`。认识该开关的二进制会解析参数后打印版本并以 0 退出；旧二进制先撞上 unknown option，退出码 1。探测失败即全程使用文本方言，`/api/health` 中 `events_json=false`、`degraded=true`、`probe_note` 给出原因。
2. **运行期回退**：若探测通过但实际运行报 `unknown option --events-json`（例如探测之后二进制被替换），本次作业**自动重跑一次**且不带该开关，日志写明原因，进程级标记转为降级，`degrade_note` 变为「CLI 运行期拒绝 --events-json，已降级为文本进度解析」。

文本方言解析规则（与 CLI 输出一一对应）：

* `N% done. status` -> `progress`（手工解析，未用正则）；
* 以两个空格开头的行 = 阶段报告，聚合为一条 `phase` 事件；
* 其余 stderr 行 -> `log`；
* stdout 的 `Quads:/Non-quads:/Vertices:` -> 统计。

## 4. 页面

单页、无外部资源：

* 参数表单，默认值与 CLI 一致；
* 目录浏览（只列 `.obj`，点击填入输入框）；
* 作业表（状态、quad/non、顶点、耗时，`↓` 标记降级作业）；
* SSE 进度条 + 阶段名 + 日志面板；
* 结果统计行；
* canvas 线框预览：自研 OBJ 解析（`v` / `f`，支持 n 边形），左键拖动旋转、滚轮缩放，超过 6 万条边截断绘制。JS 全部内联，约 150 行。
## 5. 冒烟测试（本机实测）

环境：Windows 11 / go1.27.0 / `build/core/autoremesher_cli.exe`
（该二进制**已实现并验证** `--events-json`：cube 用例 62 条 progress + 1 条 phase（55 行）+ 1 条 done，
`quads=164 / non_quads=0 / vertices=166 / exit=0`，`schema_version=1`）。
临时目录记为 `$TMP`（实测为 `C:/Users/<user>/AppData/Local/Temp/ui-go-smoke`）。

### 5.1 构建

```text
$ cd ui-go && go vet ./... && go build -o bin/ui-go.exe .
（无输出 = 通过，exit 0）
$ ls bin/
ui-go.exe
```

源码目录保持干净：`ui-go/` 下只有 `.gitignore go.mod *.go README.md web/`，
二进制只在 `ui-go/bin/`（被 `.gitignore` 排除），交付不含任何可执行文件。

### 5.2 生成测试模型（224 面球）

```python
# 16 段 x 7 环 UV 球，极点用扇形三角，避免退化三角形
python gen_sphere.py $TMP/sphere.obj
# wrote $TMP/sphere.obj verts 114 faces 224
```

### 5.3 起服务并提交作业

```bash
$TMP/ui-go.exe 或 ui-go/bin/ui-go.exe -addr 127.0.0.1:8787 &   # 后台

curl -sS -X POST -H "Content-Type: application/json" \
  --data-binary @job.json http://127.0.0.1:8787/api/jobs
# {"id":"164059-b3f201e15757", ..., "status":"running"}

curl -sS http://127.0.0.1:8787/api/jobs      # 轮询至 status != running
```

轮询结果：

```json
{"id":"164059-b3f201e15757","status":"done","elapsed_ms":56,
 "quads":248,"non_quads":0,"vertices":250,"exit":0,
 "degraded":false,"output_exists":true,"percent":100}
```

输出文件存在（248 个四边面 / 0 个非四边面 / 250 顶点），`quads > 0` 成立。

### 5.4 SSE 实际事件序列（前 10 行，`GET /api/jobs/{id}/events`）

```text
data: {"type":"state","seq":0,"time":"","status":"done"}
data: {"type":"log","seq":1,"time":"16:36:56.469","line":"启动: autoremesher_cli.exe"}
data: {"type":"log","seq":2,"time":"16:36:56.488","line":"Loaded 114 vertices, 224 triangles"}
data: {"type":"progress","seq":3,"time":"16:36:56.488","percent":0,"status":"Computing voxel size"}
data: {"type":"progress","seq":4,"time":"16:36:56.488","percent":1,"status":"Splitting mesh into islands"}
data: {"type":"progress","seq":5,"time":"16:36:56.488","percent":2,"status":"Building island contexts"}
data: {"type":"progress","seq":6,"time":"16:36:56.488","percent":3,"status":"Remeshing uniformly"}
data: {"type":"progress","seq":7,"time":"16:36:56.488","percent":3,"status":"Building bounding volume tree"}
data: {"type":"progress","seq":8,"time":"16:36:56.488","percent":5,"status":"Splitting long edges"}
data: {"type":"progress","seq":9,"time":"16:36:56.488","percent":6,"status":"Splitting long edges"}
```

（之后为 `phase` 与 `done`，即上表的 248/0/250。）

### 5.5 取消

```text
POST /api/jobs/{id}/cancel -> {"id":"...","status":"canceling"}
最终 {"status":"canceled","exit":1,"error":"已取消","elapsed_ms":180,"output_exists":false}
SSE 尾：{"type":"done","status":"canceled",...,"exit":1,"elapsed_ms":180,"error":"已取消"}
```

子进程确实被杀（无输出文件、退出码 1）。

### 5.6 降级路径（桩 CLI 验证，不污染真实二进制）

两个桩程序放在临时目录、独立模块，不属于仓库：

* `stub-cli.exe`：任何 `--events-json` 都报 unknown option（模拟旧二进制）；
* `stub2-cli.exe`：`--events-json --version` 正常返回，但真实运行时拒绝该开关（模拟「探测通过、运行失败」）。

| 场景 | `/api/health` | 作业 |
|---|---|---|
| 8788 / stub-cli | `events_json=false`，`degrade_note` = 「CLI 不支持 --events-json，将解析文本进度（探测失败: exit status 1）」 | `done`，`quads=2`，`degraded=true` |
| 8789 / stub2-cli | 启动时 `events_json=true`；运行后转 `false`，`degrade_note` = 「CLI 运行期拒绝 --events-json，已降级为文本进度解析」 | 第一次尝试报错 → 自动重跑 → `done`，`degraded=true` |

8789 的实际事件序列（展示自动降级）：

```text
data: {"type":"state","seq":0,"status":"done"}
data: {"type":"log","seq":1,"line":"启动: stub2-cli.exe"}
data: {"type":"log","seq":2,"line":"Error: unknown option --events-json"}
data: {"type":"log","seq":3,"line":"CLI 不认识 --events-json，自动降级并重跑一次（不带该开关）"}
data: {"type":"log","seq":4,"line":"Loaded 8 vertices, 4 triangles"}
```

### 5.7 其他校验

| 检查 | 结果 |
|---|---|
| `GET /api/fs?dir=$TMP` | 列出 `sphere.obj`、`sphere-remesh.obj` |
| `GET /api/fs?dir=C:/Windows/System32` | 403 `{"error":"路径不在允许浏览的根目录内"}` |
| `POST /api/jobs` 不存在的 input | 400 `{"error":"input 不存在：..."}` |
| `GET /api/jobs/{id}/obj` | 11522 字节（248 面 OBJ） |
| `node --check` 页面内联 JS | exit 0（无语法错误，JS 恰好 150 行） |
| `GET /` | 200，13806 字节，内嵌页面（无外链） |

### 5.8 前向兼容：未知字段注入（G14 第 2 条）

未知字段既可能出现在**输入**（CLI 发来的 NDJSON），也可能出现在**输出**（本服务转发的 SSE）。两个方向都做了实测。

#### (a) 输入侧：CLI 发来不认识的字段

用桩 `stub3-cli.exe` 发 `schema_version=2` 的事件，并携带 `x_future` / `nested` / `unknown_list` 等本服务未建模的字段：

```text
{"schema_version":2,"type":"progress","percent":5,"status":"Stub stage","x_future":1,"nested":{"a":[1,2]}}
{"schema_version":2,"type":"phase","lines":["stub phase: 1 ms"],"x_future":"yes"}
{"schema_version":2,"type":"done","quads":1,"non_quads":0,"vertices":4,"exit":0,"x_future":42}
```

结果：`status=done quads=1 vertices=4`，未知字段被丢弃、已知字段照常解析，后续事件继续消费。

#### (b) 输出侧：转发时插入 `x_future`

测试开关：环境变量 `UI_GO_INJECT_X_FUTURE=1`（启动日志打印「诊断: 每个 SSE 事件将附带未知字段 x_future=1」）。

```bash
$env:UI_GO_INJECT_X_FUTURE = 1 ; ./bin/ui-go.exe -addr 127.0.0.1:8787
```

实测（真实 CLI，248 四边面作业）：

```text
data: {"seq":0,"status":"done","time":"","type":"state","x_future":1}
data: {"percent":0,"seq":3,"status":"Computing voxel size","time":"...","type":"progress","x_future":1}
data: {"elapsed_ms":56,"exit":0,"non_quads":0,"quads":248,"seq":140,"status":"done",...,"x_future":1}
```

| 检查 | 结果 |
|---|---|
| data 行总数 | 141 |
| JSON 解析失败 | 0 |
| 带 `x_future` 的行 | 141（全部） |
| 作业终态 | `done`，`quads=248 / non_quads=0 / vertices=250` |

结论：注入未知字段后服务端不报错、SSE 连接不断、`done` 统计正确；页面用 `JSON.parse` 且只读取已知键，未知字段天然被忽略。

## 6. 已知限制

* **单机、无鉴权**：默认只绑 `127.0.0.1`；改成 `0.0.0.0` 就等于把本机 CLI 执行能力暴露给网络，风险自负。
* **只支持 `.obj`**：输入校验、目录浏览、导出都只认 `.obj`；没有 mtl / 贴图 / 场景概念。
* **并发不设限**：每个作业独占一个 CLI 进程，服务端不排队；同时提交多个大模型会争抢 CPU。
* **取消是硬杀**：`Kill()` 子进程，不留中间清理；被取消的作业不产生输出文件。
* **统计三来源**：NDJSON `done` > stdout 报告 > 数 OBJ 面。
* **SSE 只保留 400 条事件**：更早的历史被环形缓冲淘汰；`elapsed_ms` 由服务端计时，不是 CLI 自报。
* **路径守卫仅覆盖 `/api/fs`**：作业提交接受任意可读绝对路径，`-root` 只约束浏览接口。
* **预览有上限**：canvas 超过 6 万条边截断；`/api/jobs/{id}/obj` 上限 64 MiB。
* **无持久化**：作业表在内存里，重启即清空。
* **`UI_GO_INJECT_X_FUTURE` 是诊断开关**：只用于前向兼容测试，正常部署不要打开（会让每条事件多一个字段）。

## 7. 与 CMake / qmake 的关系

* 本目录**不参与** CMake 构建：`CMakeLists.txt` 只生产 `autoremesher_core`（静态库）与 `autoremesher_cli`（可执行文件）。ui-go 用 `go build` 独立构建。
* 与 qmake（`autoremesher.pro`）无耦合：Qt GUI 正在被移入 `legacy/gui-qt`，ui-go 是它的可选替代，不共享代码。
* 唯一契约是 CLI 的命令行与 `--events-json` 的 NDJSON schema；CLI 换路径用 `-cli` 或 `AUTOREMESHER_CLI` 指过去即可，无需重新编译。

## 8. 文件清单

```text
ui-go/
├── .gitignore            排除 bin/ 与 *.exe；用 !go.mod 抵消仓库根的 *.mod 规则
├── go.mod                module autoremesher/ui, go 1.22, 无依赖、无 go.sum
├── main.go               启动、CLI 解析/查找、-root、诊断开关、优雅退出、go:embed
├── job.go                作业状态机、子进程执行、NDJSON/文本双方言、降级、统计兜底
├── server.go             路由、SSE（含未知字段注入）、路径守卫、JSON 辅助
├── web/index.html        单页（表单 + 作业表 + SSE + canvas 线框）
└── README.md
```
