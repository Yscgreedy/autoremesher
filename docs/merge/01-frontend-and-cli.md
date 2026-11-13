# 01 · 前端与 CLI 表面调研（arch-refactor × master）

> 范围：仅前端（Go Web UI）与 CLI 表面 + 相关 docs。核心搬迁（src/core）、工具与门禁（tools/、G1~G15 门）不在本文判定范围内，只在与本文结论耦合处引用。
> 方法：只读静态检视。master 走工作区读取，arch-refactor（`0ba6c98d`）走 `git show 0ba6c98d:<path> | nl -ba`。**未构建、未运行任何二进制、未改分支/工作区**。
> 行号一律取自上述两种读取方式的实际输出。合入阶段标签：S1(核心搬迁) / S2(工具与门禁) / S3(前端与CLI) / DROP(不带)。

---

## 0. 一页结论

| 问题 | 结论 | 阶段 |
|---|---|---|
| ui-go 与 gui 二选一 | **保留 master 的 `gui/`**，`ui-go/` 整体不带 | DROP |
| ui-go 的独有能力回灌 | **只回灌 2 项**：`ui-go/.gitignore` 的 `!go.mod` 取回规则（P0，修 master 的致命缺陷）、`-max-jobs` 最旧作业淘汰（P1，修内存泄漏）。`/api/fs` 白名单、无鉴权、双方言降级**不回灌** | S3 |
| arch 独有的 3 个 CLI 选项 | `--events-json`、`--report`、`--triangulate` **都不作为独立小提交合入**（理由见 §2.3；`--report` 若确有批处理需求，以新名 `--report-file` 延后另开） | DROP |
| CLI 主体 | 保留 `src/cli/main.cpp`（26 选项）为唯一 CLI，`src/cli/main_cli.cpp`（15 选项）不带 | S3 / DROP |
| arch docs | 只有 `ARCHITECTURE-REFACTOR-PLAN.md`（改写后）与 `BUILD-CORE.md`（改写后）有留用价值；`GUI-LEGACY.md`、`legacy/README.md`、`docs/archive/*.v1`、`ui-go/README.md` 不带；`tools/metrics/README.md` 随工具走 | 见 §3 |

**最刺眼的一条事实（P0，与前端直接相关）**：master 的 `gui/go.mod` **没有进版本库**——根 `.gitignore:20` 的 `*.mod`（Fortran 规则）把它吞了，`git ls-files gui/go.mod` 为空，`git status --porcelain --ignored gui/go.mod` 输出 `!! gui/go.mod`（本地磁盘上有该文件，内容 `module autoremesher-gui` / `go 1.27`）。后果：**fresh clone 的 master 无法 `go build ./cmd/autoremesher-gui`**，而 `gui/README.md:14` 还写着“第三方模块为零、可离线构建”。ui-go 用一条带注释的规则精确解决了这个问题：`ui-go/.gitignore` → `!go.mod` / `!go.sum`（该文件 11 行，注释写明“仓库根的 .gitignore 有 Fortran 的 `*.mod`，会连 go.mod 一起忽略”）。这条必须回灌到 S3。—— **S3**

---

## 1. ui-go（arch）vs gui（master）逐项对比

规模先摆平：arch `ui-go/` 共 7 个文件 1541 行（`main.go` 202 + `job.go` 847 + `server.go` 520 + `web/index.html` 250 + `README.md` 318 + `.gitignore` 11 + `go.mod` 3，全部 stdlib，`go.mod` 仅 3 行、无 `go.sum`）；master `gui/` 共 23 个受版本控制的文件、约 5100 行（另加未入版本库的 `go.mod` 33 字节）。—— 证据：附录 A 的 `git show … | wc -l` 与 `git ls-files gui | grep -c .` → `23`。

### 1.1 进程模型

| 维度 | gui（master） | ui-go（arch） |
|---|---|---|
| 入口 | `gui/cmd/autoremesher-gui/main.go` | `ui-go/main.go`（`package main` 单层） |
| 进程结构 | 单二进制；`--cli/--port/--addr/--no-open/--version`（main.go:26-30），强制 loopback（main.go:55 → 133-142），`--port 0` 自动选端口（main.go:82-86），启动后自动开浏览器（main.go:109-116） | 单二进制；`-addr`(默认 `127.0.0.1:8787`)、`-cli`、`-max-jobs`(32)、`-root`(可重复)（main.go:37-42），**无自动开浏览器**，`0.0.0.0` 允许绑（main.go:98-100 只是把日志 URL 改回 127.0.0.1） |
| 子进程 | 每作业一个 CLI 进程，`exec.Command` + 独立进程组（proc_windows.go:11-21 / proc_other.go:14） | 每作业一个 CLI 进程，`exec.CommandContext(j.ctx)`（job.go:615），**杀的是直接子进程，不是进程树** |
| CLI 查找 | `--cli` → 自身 exe 同级 → `<exe>/dist`,`<exe>/..`,`<exe>/../dist`,`<exe>/../..` → cwd → PATH 的 `autoremesher`（discover.go:41-101；README.md:50-56） | `-cli` → `AUTOREMESHER_CLI` → `.`,`..`,`../..` 下的 `build/core/autoremesher_cli(.exe)` → PATH 的 `autoremesher_cli`（main.go:149-178） |

结论：两者都是“Go 单二进制 + 进程边界 + Web UI”的同一种架构，**没有架构级高下之分**，差别全在工程完备度上。—— **S3**

### 1.2 HTTP / SSE 协议

| 维度 | gui | ui-go |
|---|---|---|
| 路由风格 | 扁平 + query：`/api/info`、`/api/upload`、`/api/run`、`/api/events?job=`、`/api/cancel`、`/api/model?job=`、静态 `/s/<token>/`（server.go:55-62） | Go 1.22 方法化路由 + path 变量：`GET /{$}`、`GET /api/health`、`GET\|POST /api/jobs`、`GET /api/jobs/{id}/events`、`POST /api/jobs/{id}/cancel`、`GET /api/jobs/{id}/obj`、`GET /api/fs`、`GET /favicon.ico`（server.go:41-51） |
| 提交模型 | **multipart 上传** → 拿到 `inputId` → `POST /api/run`（handlers.go:47-118，上传上限 `512 << 20` = handlers.go:15） | **直接给绝对路径**，不传文件（server.go:126-168；`resolveInput` 要求存在且 `.obj`，server.go:403-422） |
| 返回码 | `POST /api/run` → 200 `{jobId, command}`（handlers.go:114-117） | `POST /api/jobs` → **202** `{id,status,input,output}`（server.go:165-167） |
| SSE 帧 | `data: {json}\n\n`，JSON 必有 `type`：`start/progress/log/report/result/error`，终止帧是 **`{"type":"end","state":...}`**（sse.go:62；README.md:85-89） | `data: {json}\n\n`，`type`：`state/log/progress/phase/done`，终止帧是 **`type:"done"`**（job.go:571-596；README §2） |
| 回放 | 订阅即回放**保留尾部 2000 条**（manager.go:33 `MaxLogTail = 2000`、139-161 Subscribe、169-172 裁剪） | 订阅先发 `state` 再回放**环形 400 条**（job.go:41 `maxJobEvents = 400`、233-241 subscribe；server.go:190-203） |
| 心跳/断开 | 15 s `: ping\n\n`；`r.Context().Done()` 退出，**断开不杀作业**（sse.go:56-74、注释 sse.go:9-12） | 15 s `: ping\n\n`；收到 `done` 即返回（server.go:205-228） |
| 进度节流 | 解析器内 25 次/秒节流（stream.go:38-88 Throttler，README.md:151-152） | 只有“同 percent+同 status 去重”（job.go:680-684），不按时间节流 |
| 统计来源 | CLI `result` 事件为唯一权威，缺失时以退出码判失败（manager.go:459-487） | **三来源兜底**：NDJSON `done` > stdout `Quads:/Non-quads:/Vertices:` > 直接数输出 OBJ（job.go:530-536、641-644、811-836） |

结论：gui 的协议**已经被 `docs/cli-protocol.md` 固化为契约**（cli-protocol.md:40-58），且实现了时间节流；ui-go 的协议是另一套（`percent` 0~100 而非 `value` 0~1，`done` 而非 `end`）。两套协议并存等于让 CLI 同时维护两种机器方言，**没有收益**。—— **S3（保留 gui 协议）/ DROP（ui-go 协议）**

### 1.3 作业管理（并发 / 取消 / 历史 / workspace）

| 维度 | gui | ui-go |
|---|---|---|
| 并发 | **单作业**：`ErrBusy` → HTTP **409**（manager.go:369-371、handlers.go:96-98；README.md:91） | **不排队、不限并发**：`Start` 只登记后 `go m.run(j)`（job.go:434-450），README §6 自述“并发不设限……同时提交多个大模型会争抢 CPU” |
| 取消 | 杀**进程树**：Windows `taskkill /T /F`、POSIX 先 `SIGKILL` 进程组再回退 pid（proc_windows.go:22-32、proc_other.go:17-25）；`Cancel` 幂等（manager.go:254-268） | context cancel → 只杀直接子进程（job.go:615）；`cancel()` 状态转 `canceled` 并在 run 里判 `j.ctx.Err()`（job.go:541-543） |
| 历史 | `m.jobs map` 只增不减（manager.go:379 写入，全文只有 Get/Current/Cancel，**无淘汰**） | `order` + `pruneLocked()`：超过 `maxJobs` 时淘汰**最旧的已完成**作业，仍在运行的保留（job.go:452-477，注释“everything is still running”则 break） |
| workspace | `os.MkdirTemp("", "autoremesher-gui-")`（workspace.go:34-35），输出 `out-<jobID>.obj`（workspace.go:90-91），退出 `RemoveAll`（workspace.go:95-97，调用点 manager.go:500-516 + cmd main.go:65 defer） | **无 workspace**：输出默认写在输入同目录的 `<stem>-remesh.obj`（server.go:425-444），只做“input≠output”校验（server.go:446-453） |
| 上传缓冲 | 512 MiB 上限 + `MultipartForm.RemoveAll` 及时清理（handlers.go:15,52-61） | 无上传，故无此问题 |
| 结果下载 | `GET /api/model?job=` 流式 `io.Copy`，`Content-Disposition` 带 `<stem>_remeshed.obj`（handlers.go:149-180） | `GET /api/jobs/{id}/obj`，**64 MiB 上限**（job.go:43、server.go:246-271） |

判断：
- **workspace + 进程树杀 + 409 串行**是 gui 更强的地方，尤其“取消要连子进程一起杀”在 Windows 上只有 `taskkill /T` 可靠（proc_windows.go:22-23 注释即为此）。
- **ui-go 唯一真正更好的点是 `pruneLocked`**：gui 的 `m.jobs` 只增不减，每个作业还各留 2000 条事件（manager.go:33），一个长时间开着的会话会稳定吃内存。这是可独立回灌的小补丁。—— **S3**

### 1.4 鉴权

- gui：启动生成 16 字节 hex token（server.go:36-43），静态页只在 `/s/<token>/` 下（server.go:56、117-139），`/api/*` 全部过 `guard`（server.go:57-62、95-106），token 可放 `X-AR-Token` 头 / `?token=` / `ar_token` cookie，比较用 `subtle.ConstantTimeCompare`（server.go:74-92）。token 缺失/错误 → 401，静态路径错 → 404（README.md:71）。**页面 HTML 里不含 token**（cli-protocol.md:77）。
- ui-go：**完全没有鉴权**，只在 `-addr` 默认值上绑 loopback，`main.go:37` 的 flag 说明直接写“默认只绑本机，**无鉴权**”；README §6 自述“改成 `0.0.0.0` 就等于把本机 CLI 执行能力暴露给网络”。

判断：gui 的 token 方案对一个“能执行本机 CLI”的服务是必需的；ui-go 的“靠绑定地址”在同机多用户/端口转发场景下等于零防护。—— **S3（保留 gui）/ DROP（不引入无鉴权面）**

### 1.5 文件系统浏览（/api/fs）

ui-go 独有，且是它写得最讲究的一段：
- 路由 `GET /api/fs`（server.go:48），处理函数 server.go:291-343：只列 `.obj`（server.go:318），2000 条截断（server.go:331-334），返回 `{dir,parent,entries,roots,truncated}`（server.go:280-286）。
- 路径守卫 `safeDir`：先 `filepath.Abs` → **`EvalSymlinks`** → `os.Stat` 判目录 → `withinRoots`（server.go:350-374）；`isWithin` 用 `filepath.Rel` 判 `..` 前缀（server.go:389-401）；越权统一 403 `errForbiddenPath`（server.go:345-348、300-307）。
- `-root` 可重复，默认“当前目录 + 临时目录”，启动时逐个 `Abs`（main.go:41-42、52-59）。
- 明确划定边界：注释写“deliberately restricted to the configured roots so the page cannot become a filesystem oracle for the whole disk”，且**作业提交另走显式路径**（server.go:288-290）；README §6 也承认“路径守卫仅覆盖 `/api/fs`”。

master 侧**没有** `/api/fs`（server.go:55-62 路由全表可证），走的是“上传 → 临时 workspace”路线。

判断：这不是能力缺口而是**设计选择**。gui 的上传路线上限 512 MiB（handlers.go:15），对一个本机网格工具确实存在“为了看一眼模型先复制 300 MB”的浪费；但补 `/api/fs` 意味着同时要接受“把任意可读路径喂给 CLI”的语义（ui-go 自己就是这么做的，且它的守卫只护浏览接口）。在 `gui/` 已有 token + workspace 的前提下，**先不动**，真要补也应当和“路径模式”一起设计（见 §1.8）。—— **DROP（暂不回灌）**

### 1.6 错误处理

| 场景 | gui | ui-go |
|---|---|---|
| CLI 不存在 | `ErrNoCLI` → **503**（handlers.go:99-101）；`/api/info` 每次调用重新探测（handlers.go:31-37、discover.go:67-101） | start 时 `os.Stat(cliPath)` → **503**（server.go:157-161）；健康端点每次重新 Stat（server.go:94-102） |
| 未知作业 | 404 `unknown job`（sse.go:19-21、handlers.go:133-136） | 404 `job 不存在`（server.go:174-176） |
| 参数越界 | `RangeError` 指名段名，400（params.go 校验段 + handlers.go:105-112；README.md:110） | `normalize()` 逐字段范围检查，400 带中文原因（job.go:98-117） |
| 退出码语义 | 0+result→done，1/2/3 或 error→failed，被杀→cancelled（README.md:188-190、manager.go:459-487） | exit≠0→failed 并把 stderr 尾部 40 行拼进错误（job.go:40-43、547-554） |
| 方言不兼容 | 不适用（只有一种方言） | **两道闸门**：启动探测 `--events-json --version`（job.go:402-430）；运行期若撞 `unknown option` 则自动**不带该开关重跑一次**并置降级（job.go:522-528、705-723）；`/api/health` 暴露 `events_json/degraded/degrade_note/probe_note`（server.go:79-117） |

判断：ui-go 的“降级双方言”在**它自己的世界**里是负责任的工程，但它存在的前提是“CLI 的方言可能不存在”——而 master 的 `--json` 是同一份代码里的既有能力（main.cpp:254-255 解析、511-520 发 `start`、450-456 发 `progress`、463-475 发 `report`、600-607 发 `result`），不存在“旧二进制不认”的问题。**把双方言搬进 master 等于凭空制造一个兼容性问题。** 唯一值得留的是那条**前向兼容诊断思路**（`UI_GO_INJECT_X_FUTURE=1` 往每条 SSE 事件塞 `x_future:1` 验证客户端忽略未知字段，main.go:76-82、server.go:463-490，README §5.8）——这是测试手法，应当写进 gui 的测试说明而不是代码。—— **DROP（双方言）/ S3（仅记录测试手法）**

### 1.7 测试覆盖与构建入口

- gui：**3 个测试文件 16 个 Test 函数** + 一个协议桩和一个构建辅助。
  - `job/manager_test.go:74 TestManagerHappyPath`、`:127 TestManagerSecondJobRejectedAndCancelled`、`:171 TestManagerErrorPath`、`:197 TestManagerUnknownInput`、`:204 TestParamsValidate`、`:240 TestParamsArgs`、`:266 TestDecodeParamsAppliesDefaults`；
  - `job/stream_test.go:13 TestThrottleCapsProgressRate`、`:64 TestParserClampsProgress`、`:77 TestParserFlushesPendingProgressOnResult`、`:101 TestParserHandlesMalformedAndUnknownLines`；
  - `server/server_test.go:117 TestTokenRejection`、`:144 TestInfoAndStaticPage`、`:187 TestRunProducesResultAndModel`、`:247 TestConflictAndCancel`、`:291 TestRunValidation`；
  - 桩与辅助：`gui/internal/testdata/fakecli/main.go`（230 行）、`gui/internal/testutil/build.go`（57 行）、`gui/docs/e2e-smoke.sh`（62 行）。
- ui-go：**0 个 `*_test.go`**（`git ls-tree -r --name-only 0ba6c98d -- ui-go | grep '_test.go' | wc -l` → `0`）。它的质量证据是 `ui-go/README.md` 里手写的冒烟记录（构建、`curl` 提交、SSE 前 10 行、取消、两个桩 CLI 的降级、未知字段注入）——**证据很扎实，但不是可重复的门禁**。
- 构建入口：gui `go build -o dist/autoremesher-gui.exe ./cmd/autoremesher-gui`（README.md:16-20），质量门 `gofmt -l .` / `go vet ./...` / `go test ./...`（README.md:22-28）；ui-go `go vet ./...` + `go build -o bin/ui-go.exe .`（README §1），产物落 `bin/`（`.gitignore` 有 `bin/`、`*.exe`、`*.test`、`*.out`）。
- **两边都没进 CI**：master `.github/workflows/development.yml:15-29` 只有一个 `clang-format-check` 作业，没有 `go build/go test`，也没有 cmake 构建。

结论：测试覆盖上 **gui 完胜**（16 个可重复断言 vs 0）；构建入口上 **ui-go 的 `.gitignore` 更正确**（显式取回 `go.mod`）。建议在 S3 顺手补一个 `gofmt -l` + `go vet` + `go test` 的 CI 作业（两分支都缺，不是合并冲突项）。—— **S3**

### 1.8 「保留哪一个 / 回灌什么」的明确结论

**保留 `gui/`，`ui-go/` 不带。** 依据（按权重）：
1. gui 有 16 个可重复测试 + 协议桩，ui-go 有 0 个（§1.7）；
2. gui 有 token 鉴权，ui-go 无鉴权（§1.4）；
3. gui 取消杀进程树，ui-go 只杀直接子进程（§1.3）；
4. gui 有 workspace 隔离与退出清理，ui-go 直接写输入目录（§1.3）；
5. gui 的 SSE 协议已被 `docs/cli-protocol.md` 固化为契约，ui-go 是第二套（§1.2）；
6. ui-go 的功能是 gui 的真子集——它没有上传、没有 info 端点，唯一“多出来”的是 `/api/fs`（§1.5）。
—— **S3（保留 gui）/ DROP（ui-go 整体）**

**值得回灌 ui-go 的清单（按优先级）**：

| # | 回灌项 | 证据 | 为什么值得 | 阶段 |
|---|---|---|---|---|
| 1 | `!go.mod` / `!go.sum` 取回规则 + 顶层 `bin/`、`*.exe`、`*.test`、`*.out` 忽略（照 `ui-go/.gitignore` 11 行改 gui） | `git ls-files gui/go.mod` 空；`.gitignore:20 *.mod`；`git status --ignored` → `!! gui/go.mod`；对比 `ui-go/.gitignore` 首两段注释 | 修的是 **master 现存缺陷**：fresh clone 无法构建 GUI | S3（P0） |
| 2 | `-max-jobs` + `pruneLocked` 淘汰最旧已完成作业 | ui-go job.go:39-44、452-477；gui manager.go:379 只写不删 | gui 的 `m.jobs` 与每作业 2000 条历史会无界增长 | S3（P1） |
| 3 | `safeDir/withinRoots/isWithin` 这组路径守卫函数（**只当工具函数留用，不急着开 `/api/fs`**） | ui-go server.go:350-401（含 `EvalSymlinks` 解链接） | 若将来给 gui 加“直接指定路径”模式，这是现成的正确实现 | S3（可选） |
| 4 | “未知字段前向兼容”测试手法（`UI_GO_INJECT_X_FUTURE`） | ui-go main.go:76-82、server.go:463-490、README §5.8 | 一行诊断开关就能证明“客户端忽略未知字段、服务端不断流”，写进 gui 测试说明即可 | S3（文档级） |

**明确不回灌**：无鉴权的默认姿势、SSE 第二套帧契约（`percent`/`done`/`phase`）、双方言降级、把输出写到输入目录、`/api/fs` 直接对外（至少要等“路径模式”整体设计）。—— **DROP**

---

## 2. CLI 选项矩阵

### 2.1 两份 CLI 的定位差异

- master `src/cli/main.cpp`（614 行）：目标名 **`autoremesher`**（`CMakeLists.txt:77 add_executable(autoremesher ...)`），是“唯一入口”，`docs/cli-protocol.md` 是它的契约；`--json` 走 **stdout**，`--progress` 走 **stderr**（cli-protocol.md:33-34）。
- arch `src/cli/main_cli.cpp`（548 行）：目标名 **`autoremesher_cli`**（arch `CMakeLists.txt:191`），刻意复刻“Qt 版 headless 路径”的行为（文件头注释 main_cli.cpp:25-30），因此默认值是 Qt 那一套（`target-quads` 默认 **50000**、`sharp-edge` **90**、`adaptivity` **1.0**、`anisotropy` **1.0**，main_cli.cpp:65-72），与 master 的默认值（20000/60/0.5/0.0，main.cpp:104-109）**不一致**。

> 这一条本身就足以否决“整体换 CLI”：默认值不同 = 同一份输入两份结果，`docs/cli-protocol.md` 的默认值表（:13-36）与 `gui/internal/job/params.go` 的 `DefaultParams()` 都会失效。—— **S3**

### 2.2 选项矩阵（26 vs 15）

master 的 26 个长选项取自 `src/cli/main.cpp:191-261`；arch 的 15 个取自 `src/cli/main_cli.cpp:161-226`。

**共有 12 个**：`--help --version --input --output --target-quads --edge-scaling --sharp-edge --smooth-normal --adaptivity --anisotropy --keep-all-islands --relative-tolerance`。

**arch 独有 3 个**：`--report`(184-188)、`--triangulate`(223-224)、`--events-json`(225-226)。

**master 独有 14 个**：`--model-type`(221)、`--exact-target-quads`(232)、`--absolute-tolerance`(234)、`--keep-island-fraction`(238)、`--weld-patches`(242)、`--graded-holes`(244)、`--legacy-holes`(246)、`--projected-caps`(248)、`--local-remesh`(250)、`--threads`(252)、`--json`(254)、`--progress`(256)、`--verbose`(258)、`--quiet`(260)。

语义同名但实现不同、**不可互换**的一项要单独记：
- `--keep-all-islands`：master 映射成 `keepIslandFraction = 1e-9`（main.cpp:240-241），arch 调 `setKeepAllIslands(true)`（main_cli.cpp:219-220 → `src/core/pipeline/autoremesher.h:102`）。master 的核心侧**没有** `setKeepAllIslands`，只有 `setIslandAreaFraction`（`src/AutoRemesher/autoremesher.h:122`；grep `setKeepAllIslands` 在 `src/AutoRemesher/autoremesher.h` 零命中）。所以 **main_cli.cpp 不能原样搬到 master 上编译**。—— **DROP**

### 2.3 三个 arch 独有选项：master 有等价物吗？值不值得合？

#### (a) `--events-json` —— master 已有等价能力，**不带**

- arch 实现：`HeadlessParams.eventsJson`（main_cli.cpp:78-81）→ 选项（:225-226）→ `ProgressPrinter.json` 走 `emitEvent` 输出 `{"schema_version":1,"type":...,"percent":...}` 到 **stderr**（main_cli.cpp:358-362、364-396）→ 结束时补 `phase`（:453-466）与 `done`（:501-507）。
- master 等价物：`--json`（main.cpp:254-255）→ stdout 上 `start`（:511-520）/ `progress`（:450-456，`value` 0~1，带时间节流 :438-449）/ `report`（:463-475，与 `--verbose` 联动 :591-598）/ `result`（:600-607）/ `error`（:477-480），规则写死在 `docs/cli-protocol.md:40-58`。
- 结论：**不是“缺能力”，是“第二种方言”**。数据完全重合（进度、阶段报告、四元组统计），只是字段名（`value` vs `percent`）、通道（stdout vs stderr）、终止类型（`result` vs `done`）不同。合入它唯一受益者是 `ui-go`，而 ui-go 本身判定不带（§1.8）。**合入 = 凭空多维护一套协议 + 再写一份 `gui/internal/job/stream.go` 的解析分支**。
- 依赖：**不依赖任何未合入的核心代码**。它需要的 `setProgressHandler`/`setTag`/`phaseReport()` 在 master 上都有（`src/AutoRemesher/autoremesher.h:66`、`:71`、`:227`）。真要搬，只搬 main_cli.cpp 的 `emitEvent`(358-362) + `ProgressPrinter`(364-396) + 三处调用点(453-466/501-507) 即可。—— **DROP**

#### (b) `--report` —— master 明确移除过，**不带**

- arch 实现：`reportPath`（main_cli.cpp:64）→ 选项（:184-188）→ 收尾写文件（:518-545），布局注释自称“Byte-for-byte the layout of the report main.cpp writes”（:524），即 **Qt 版 `legacy/gui-qt/src/main.cpp` 的 headless 报告**（该文件 :104-107 定义 `--report`，:193-213 写出 `AutoRemesher Report` 与分隔线）。
- master 现状：**没有** `--report`（main.cpp:191-261 全表无此选项），且 `README.md:132` 明写：**“The old `--report FILE` spelling is gone; use `--verbose`, which prints the same phase report (and emits it as a `report` JSON event under `--json`).”**
- 严格说 master README 这句有瑕疵：`--verbose` 输出的是**逐阶段耗时行**（main.cpp:591-598 → `phaseReport()`），arch `--report` 写的是**统计摘要文件**（quads/non-quads/vertices/total time，main_cli.cpp:525-539）。两者内容不同，所以“`--verbose` 已覆盖 `--report`”只在“想要阶段报告”这一半成立；“想要一个可归档的统计文件”这半边确实是空的。
- 结论：**作为独立小提交不合入**——它会直接推翻 `README.md:132` 已经写死的决策，且当前没有任何消费者（`gui/` 只解析 stdout 的 `result` 事件；`bench/` 自己算统计，不经 CLI）。若将来批处理/CI 真的需要统计落盘，**另开新名 `--report-file`**（约 40 行，见依赖清单），避免与 `--json` 的 `report` 事件类型混淆（cli-protocol.md:48）。
- 依赖：**零核心依赖**，只需 main_cli.cpp 的 `formatNumber`（:87-92）与 `HeadlessParams` 里 master `Options` 已全部具备的字段（inputPath/outputPath/targetQuads/edgeScaling/sharpEdgeDegrees/smoothNormalDegrees/adaptivity/anisotropy）。—— **DROP（可选改名延后）**

#### (c) `--triangulate` —— master 已是“永远三角化”，**不带**

- arch：默认走“历史规则”——每 3 个索引当三角面（main_cli.cpp:273-281，注释“Kept as the default for parity”），`--triangulate` 才做扇形三角化（:282-295）。也就是说 **arch 的默认行为会把 n-gon 读错**，这个开关是用来修的。
- master：加载器**始终**做扇形三角化 `for (i=2; i<face.size(); ++i) triangles->push_back({face[0], face[i-1], face[i]})`（main.cpp:354-356），并在 usage 与文档两处声明（main.cpp:132 “polygons are triangulated”、cli-protocol.md:15 “polygons are triangulated”）。
- 结论：master 里 `--triangulate` 要么是 no-op，要么要求先回退到“错误默认值”——两种情况都不成立。**没有等价物也不需要等价物**。—— **DROP**

### 2.4 合入判据小结

三个选项的共同点：**都是 CLI 局部改动，零未合入核心依赖**（唯一会绊住“整文件搬运”的是 `setKeepAllIslands`，见 §2.2；而三个选项本身都不碰它）。因此技术上“能搬”，但都不值得：`--events-json` 与 `--json` 功能重叠、`--report` 与 README 既定决策冲突、`--triangulate` 是修一个 master 没有的 bug。建议把这三个选项作为**否决项归档**，在合并说明里各写一行原因，避免下次再被拿出来讨论。—— **DROP（三项统一）**

---

## 3. docs 清单差异表

master 侧现存 7 份（`docs/cli-protocol.md`、4 份根目录分析、`gui/README.md`、`bench/README.md`）；arch 侧 7 份（`docs/ARCHITECTURE-REFACTOR-PLAN.md` 1096 行、`docs/BUILD-CORE.md` 180、`docs/GUI-LEGACY.md` 190、`docs/archive/...v1` 2357、`legacy/README.md` 29、`ui-go/README.md` 318、`tools/metrics/README.md` 305）。

### 3.1 master 侧（作为目标态，逐份确认）

| 文档 | 判断 | 说明与证据 | 阶段 |
|---|---|---|---|
| `docs/cli-protocol.md`（77 行） | **可直接采用** | 它就是 master CLI + gui 的机器契约（:40-58 JSON-lines 规则、:13-36 选项表、:60-77 GUI 行为与 token 说明），与 `src/cli/main.cpp`、`gui/internal/server/server.go` 一一对得上。需要同步的只有 S3 的两处补丁（go.mod 追踪、作业淘汰），那属于 README 而非本文件 | S3 |
| `gui/README.md`（190 行） | **需改写** | 两处与事实不符：①`:14` 宣称“Go 1.27, standard library only … builds offline”，但 `gui/go.mod` 未进版本库（见 §0），fresh clone 直接构建失败；②`:131` 的 Layout 把 `go.mod` 列为 `gui/` 下的一项，读者会以为它被跟踪。改写动作：Layout 处标注“`go.mod` 需随 `.gitignore` 修正一并入库”，Tests 段补“CI 尚未覆盖 gui”（`.github/workflows/development.yml:15-29` 只有 clang-format） | S3 |
| `AUTOREMESHER_DECOMPOSITION.md`（10 行） | **可直接采用** | 它已是一份“带历史声明的快照”（:1-4 明确“Historical snapshot … 提到的 `src/*.cpp` 控件、`shaders/`、`resources.qrc`、`autoremesher.pro` 已不存在；它分析的 L1/L2 core（`src/AutoRemesher`）未变，现在是 `autoremesher_core`”）。这句声明正是 arch 侧 `docs/archive/*.v1` 想达到的效果，且与 master 现状一致 | S1 |
| `GPU_AND_QUALITY_ASSESSMENT.md`（884 行） | **可直接采用** | 全部是 master 核心的实测与路线（阶段耗时画像、线程伸缩、MILS 阈值 A/B、P0-1~P1-2 实施记录），不引用 arch 侧路径/目标名；是 S1 质量门的依据 | S1 |
| `OPTIMIZATION_LOG.md`（290 行） | **可直接采用** | 记录 master 侧性能提交（增量奇异点电荷、AABB 树、哈希容器、高面数目标瓶颈、割缝并行实测），与 master 的 6 个性能提交一一对应；arch 侧无对应内容 | S1 |
| `RETOPOLOGY_ALGORITHMS.md`（353 行） | **可直接采用** | 算法族谱与阶段表（A~G），描述 master `src/AutoRemesher` 的流水线；若 S1 真把 core 分层搬走，只需更新“文件→阶段”映射表，主体不动 | S1 |
| `bench/README.md`（182 行） | **可直接采用** | 对应 `bench/main.cpp` + 三个 build 脚本，这些文件只在 master 侧存在（tree diff 的 `ONLY master` 段含整个 `bench/`）；开关说明与 master CLI 一致 | S2 |

### 3.2 arch 侧（逐份裁决）

| 文档 | 判断 | 说明与证据 | 阶段 |
|---|---|---|---|
| `docs/ARCHITECTURE-REFACTOR-PLAN.md`（1096 行） | **需改写（内容有价值，名号全错）** | 它的“资产速查”表把当前态写成：构建入口 `autoremesher_core`(STATIC)+**`autoremesher_cli`**(EXE)、core 分层 `src/core/{base,geom,...}` 29 文件、可选前端 **`ui-go/`**、legacy GUI `legacy/gui-qt/`（:37-44）。四项里三项与 master 现状冲突（master：`autoremesher` 可执行、`src/AutoRemesher`、`gui/`、无 legacy）。其实质内容（§2.1 依赖规则、§3 契约层 Result/Progress/Ownership/Log、§4 面表 CSR 两刀、§5 验收门总表、§6 逐条清单）值得留作 S1 路线图，但必须先做一次“名号再基线”：`autoremesher_cli`→`autoremesher`、`src/core/**`→实际落地目录、删掉 `ui-go`/`legacy` 两行 | S1 |
| `docs/BUILD-CORE.md`（180 行） | **需改写（三处硬错误，且自身已过期）** | ①目标表把 CLI 写成 `autoremesher_cli`（:39）与产物 `build/core/autoremesher_cli.exe`（:95、:142），master 是 `autoremesher`（`CMakeLists.txt:77`）与 `build\cli\dist\autoremesher.exe`（README.md:55）；②`:38` 写 core 源是“`src/AutoRemesher/*.cpp`（12 个）”，而 arch 自己已把 core 移到 `src/core/**` 并由 `cmake/core_sources.cmake` 生成列表（arch `CMakeLists.txt:161`），**该文档在 arch 自己分支上就已过期**；③CLI 用法表（:109-118）列着 `--report` 与默认 50000/90/1.0，master 均已不同（§2.1、README.md:132）。改写后（目标名、产物路径、选项表、默认值）可作构建手册；它的 §3「TBB 实际走的路径」与实测耗时（:100-103）是 master 侧缺失内容，值得留 | S2 |
| `docs/GUI-LEGACY.md`（190 行） | **不带** | 全篇是“把 64 个文件 `git mv` 到 `legacy/gui-qt/`”的搬家单（:5、:10-14、:27-47）。master 的选择是**删除** Qt 源码并在 README 里留“Migrating from the Qt GUI”段（README.md:124-133，其中 :128 写“`resources.qrc` 和 `autoremesher.pro` 已被删除；`git log` 保留历史”）。合并后 `legacy/gui-qt/` 不存在，这份文档每一行都没有指称对象；其 §7「恢复步骤（**未验证**）」更是负资产 | DROP |
| `docs/archive/ARCHITECTURE-REFACTOR-PLAN.v1-2026-11-10.md`（2357 行） | **不带** | 是同一份计划 v1 的归档（标题 :2 “v1 归档，2026-11-10 清理前”），其中 §1.5 还专门“对 `AUTOREMESHER_DECOMPOSITION.md` 的独立核验”。master 侧已有自带历史声明的 10 行快照（§3.1），再带 2357 行归档等于把“未落地的计划”连同旧结论一起搬进主线。若 S1 采纳计划，改写主文档即可，归档留在 arch 分支历史里就够 | DROP |
| `legacy/README.md`（29 行） | **不带** | 与 `legacy/gui-qt/` 同生共死：:14 的表格直接写 `autoremesher_cli EXE src/cli/main_cli.cpp`，与 master 的目标名和 CLI 源文件都不一致 | DROP |
| `ui-go/README.md`（318 行） | **不带（但摘 2 条设计记录进 gui 文档）** | 它是一份**质量很高**的前端设计记录（API 表、SSE 样例、降级闸门、8 段实测记录、前向兼容实验）。但它描述的服务本身判定不带（§1.8），留着会与 `gui/README.md` 形成两份互相矛盾的前端自述（端点名、SSE 帧、端口 8787 vs 随机、无鉴权 vs token）。**摘出两条**写进 `gui/README.md`：①“未知字段前向兼容”诊断手法（README §5.8）；②“最旧已完成作业淘汰”的取舍理由（§6 已知限制） | DROP |
| `tools/metrics/README.md`（305 行） | **可直接采用（前提：`tools/metrics/` 随 S2 一起合入）** | 它是度量工具说明书：`mesh_metrics.py measure/compare`（:26-46）、指标定义（:57-92）、`--ref` 对比（:93-126）、自检 `test_metrics.py` 60/60（:204）、G4 判据校准（:264-282）。工具本体（`mesh_metrics.py`、`emit_comparison.py`、`test_metrics.py`、`selfcheck/*.json`、`tools/check_boundaries.py`、`tools/check_plan_numbers.py`、`tools/gen_sources.py`）**只在 arch 侧存在**（tree diff 的 `ONLY arch` 段含 `cmake/core_sources.cmake`、`include/AutoRemesher/Csr`、`tools/*`）。因此文档必须与工具同批走 S2；落地时复核 §4 自检数值（对 arch 工作区跑的，master 核心可能不同） | S2 |

**docs 冲突总述**：master 侧 4 份根目录分析（`AUTOREMESHER_DECOMPOSITION` / `GPU…` / `OPTIMIZATION_LOG` / `RETOPOLOGY_ALGORITHMS`）在 arch 分支上**一份都没有**（tree diff `ONLY master` 首四行即是它们），arch 的 4 份 docs 在 master 上也没有——**两份文档集不重叠、不冲突，是“并存择用”关系**，不构成合并阻塞。—— **S1/S2**

---

## 4. 结论区：前端与 CLI 资产最终取舍表

| 资产 | 建议 | 理由（含证据） | 阶段 |
|---|---|---|---|
| `src/cli/main.cpp`（master，26 选项，目标 `autoremesher`） | **保留为唯一 CLI** | 覆盖 core 全部能力（`--threads`/`--model-type`/`--exact-target-quads`/补洞三档/`--local-remesh`），默认值与 `docs/cli-protocol.md` + `gui/internal/job/params.go` 对齐（main.cpp:104-109） | S3 |
| `src/cli/main_cli.cpp`（arch，15 选项，目标 `autoremesher_cli`） | **不带** | 缺 14 个选项（§2.2）；默认值取 Qt 那一套（target 50000/sharp 90/adaptivity 1.0，main_cli.cpp:65-72）；调用 master 不存在的 `setKeepAllIslands`（`src/core/pipeline/autoremesher.h:102` vs master `autoremesher.h:122`） | DROP |
| `--events-json` | **不带** | 与 `--json` 数据重合，通道/字段/终止类型全不同（main.cpp:450-607 vs main_cli.cpp:358-507）；唯一消费者 ui-go 不带 | DROP |
| `--report` | **不带**（若需批处理统计文件，另开 `--report-file`，约 40 行，零核心依赖） | master `README.md:132` 已声明移除；`--verbose` 只覆盖“阶段报告”半边，统计文件半边确实空缺但当前无消费者；`--report` 名与 JSON `report` 事件撞名（cli-protocol.md:48） | DROP |
| `--triangulate` | **不带** | master 加载器始终扇形三角化（main.cpp:354-356；cli-protocol.md:15），arch 该开关是修它自己的错误默认值（main_cli.cpp:273-281） | DROP |
| `gui/`（master Go 前端，23 个受版本控制文件 ≈5100 行） | **保留唯一前端** | 16 个测试 + 协议桩（§1.7）、token 鉴权（server.go:36-106）、进程树取消（proc_windows.go:22-32）、workspace 清理（workspace.go:95-97）、WebGL 预览（`gui/web/viewer.js` 403 行 + `app.js` 584 行） | S3 |
| `ui-go/`（arch，7 文件 1541 行） | **整体不带** | 0 测试、无鉴权、取消只杀直接子进程（job.go:615）、无上传、SSE 第二套契约；功能是 gui 的子集（§1.8） | DROP |
| ui-go 的 `.gitignore`（`!go.mod`/`!go.sum` + `bin/`/`*.exe`/`*.test`/`*.out`） | **回灌到 `gui/`（P0）** | master `gui/go.mod` 被根 `.gitignore:20 *.mod` 吞掉、未入库（`git ls-files gui/go.mod` 空、`git status --ignored` → `!! gui/go.mod`），fresh clone 无法构建；ui-go 的 11 行规则精确修此问题 | S3 |
| ui-go 的 `-max-jobs` + `pruneLocked` | **回灌到 `gui/internal/job`（P1）** | gui `m.jobs` 只写不删（manager.go:379），每作业另留 2000 条事件（manager.go:33）；ui-go job.go:452-477 是现成实现 | S3 |
| ui-go 的 `safeDir/withinRoots/isWithin` 路径守卫 | **留作工具函数备用，不接 `/api/fs` 路由** | 实现正确（`EvalSymlinks` + `Rel` 判 `..`，server.go:350-401），但 gui 走“上传 + 临时 workspace”（handlers.go:15,47-81），开任意路径浏览会引入新面 | S3（可选） |
| ui-go 的双方言 / 无鉴权默认 / 输出写输入目录 | **不回灌** | 双协议是维护成本；无鉴权等于把 CLI 执行能力暴露给本机任意进程（ui-go README §6 自述）；输出落输入目录会让 gui 的 workspace 语义失效 | DROP |
| `docs/cli-protocol.md` | **保留（契约权威）** | 与 master CLI/GUI 实现一一对应（§3.1） | S3 |
| `gui/README.md` | **保留 + 改写** | 修 `:14` 的“可离线构建”与 `:131` 的 Layout（go.mod 未入库）；补“CI 未覆盖 gui” | S3 |
| `bench/README.md` | **保留** | 对应文件只在 master 侧（tree diff） | S2 |
| `docs/ARCHITECTURE-REFACTOR-PLAN.md` | **条件采用（必须改写名号）** | 内容（契约层/CSR/验收门）有价值，但 :37-44 的路径与目标名与 master 冲突 | S1 |
| `docs/BUILD-CORE.md` | **改写后采用** | 目标名/产物路径/选项表三处硬错误（:39、:95、:109-118），且 :38 的源清单在 arch 自己分支上已过期 | S2 |
| `docs/GUI-LEGACY.md`、`legacy/README.md` | **不带** | 与 master“删除 Qt 源码”的既定选择冲突（README.md:124-133） | DROP |
| `docs/archive/*v1-2026-11-10.md` | **不带** | 2357 行历史归档；master 已有 10 行“历史快照”声明（AUTOREMESHER_DECOMPOSITION.md:1-4） | DROP |
| `ui-go/README.md` | **不带（摘 2 条设计记录）** | 描述的服务不带；摘“未知字段前向兼容测试”与“作业淘汰取舍”进 gui 文档 | DROP |
| `tools/metrics/README.md` | **随 `tools/metrics/` 与 `tools/check_*.py` 一起采用** | 文档与工具必须同批；工具本体只在 arch 侧 | S2 |
| （新增，非合并项）CI 增加 `gofmt -l` / `go vet` / `go test` 作业 | **建议在 S3 补** | 两分支都没有 Go 门禁（master `.github/workflows/development.yml:15-29` 仅 clang-format） | S3 |

---

## 附录 A：取证命令（可复现）

```bash
# 1) ui-go 规模
for f in ui-go/main.go ui-go/job.go ui-go/server.go ui-go/web/index.html ui-go/README.md ui-go/.gitignore ui-go/go.mod; do
  printf '%-28s %s\n' "$f" "$(git show 0ba6c98d:$f | wc -l)"; done
# 2) gui 受版本控制文件数
git ls-files gui | grep -c .            # → 23
# 3) go.mod 未被跟踪（关键缺陷）
git ls-files gui/go.mod                 # → 空
git status --porcelain --ignored=matching gui/go.mod   # → !! gui/go.mod
grep -nE '\.mod$' .gitignore            # → 20:*.mod
# 4) 文档规模
git show 0ba6c98d:docs/ARCHITECTURE-REFACTOR-PLAN.md | wc -l   # → 1096
# 5) 树差异（谁有 bench/、tools/、gui/、legacy/）
git ls-tree -r --name-only master | sort > /tmp/m.txt
git ls-tree -r --name-only 0ba6c98d | sort > /tmp/a.txt
comm -23 /tmp/m.txt /tmp/a.txt | grep -vE '^legacy/' | head -40
comm -13 /tmp/m.txt /tmp/a.txt | head -40
# 6) ui-go 无测试
git ls-tree -r --name-only 0ba6c98d -- ui-go | grep '_test.go' | wc -l   # → 0
# 7) CLI 选项面
grep -nE '"--[a-z-]+"' src/cli/main.cpp
git show 0ba6c98d:src/cli/main_cli.cpp | nl -ba | grep -E '"--[a-z-]+"'
# 8) 目标名
grep -nE 'add_executable|add_library' CMakeLists.txt                       # → 47/77
git show 0ba6c98d:CMakeLists.txt | grep -nE 'add_executable|add_library'   # → 161/191
```

## 附录 B：本次调研的边界与未验证项

1. **未构建、未运行**：`gui/` 与 `ui-go/` 都没有编译，`go vet`/`go test` 未执行；“能否构建”的判断基于“`go.mod` 是否入库”这一静态事实（`git ls-files` 输出）。
2. **`gui/go.mod` 的 `go 1.27`**：文件内容为 `module autoremesher-gui` + `go 1.27`，但它不在版本库里，属于工作区现状；`ui-go/go.mod` 是 `go 1.22`（被跟踪）。合并时必须先决定用哪个版本行。
3. **arch 核心侧接口差异只核了与本文件结论相关的两处**（`setKeepAllIslands` vs `setIslandAreaFraction`、`phaseReport`/`setProgressHandler` 双方都有）；完整核心 API 差异由核心搬迁（S1）负责。
4. **`tools/metrics/README.md` 的自检数值**（60/60、§4 各表）是对 arch 工作区跑出来的，合入后需在 master 核心上重跑复核；本文只判“文档与工具同批”，不背书数值。
5. **未验证「master 的 gui 二进制可运行」**：本次只读，不执行任何构建产物；§1 中关于行为差异的判断均来自源码与文档，未做运行时对拍。

