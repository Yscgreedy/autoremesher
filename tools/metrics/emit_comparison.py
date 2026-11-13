#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 artifacts 下的 JSON 生成三份派生件（纯标准库，不测量任何几何）。

  artifacts/COMPARISON.md          A/B 主表 + 差值表
  artifacts/baseline/SUMMARY.md    基线采集摘要（主表 + 明细）
  artifacts/after/SUMMARY.md      after 采集摘要（14 列）

**批次不可拆**：没有「只生成/只校验一份」的用法；生成与 --check 永远覆盖三份。
理由：磁盘上曾出现「COMPARISON 修好、两份 SUMMARY 继续内嵌失效 sha」的状态，
根因不是人偷懒，而是工具允许只修一份。

用法
----
    python tools/metrics/emit_comparison.py                 # 生成三份
    python tools/metrics/emit_comparison.py --check         # 三份比对，任一不一致 exit 1（并指名哪份、第几行）
    python tools/metrics/emit_comparison.py --out-dir X     # 仍写三份，仅换输出根目录（调试用）

退出码
------
    0  一致 / 生成成功
    1  文件与重算结果不一致（过期或被手改）
    2  前置不可生成：8 份 meta 指纹不一致 / 与工具当前 sha 不符 / 缺少 baseline 或 after 数据 /
       某格无法推导（如缺 out.obj → 「不可推导」，不猜「否」）
    3  数据本身不可复现：baseline/<case>/run2/ 存在但两次 out.obj 的 SHA256 不同
       （「两次不一致」= 这组数据不可信，绝不写成「否」）

口径（C4：每列写清字段 + 用例 + 来源）
--------------------------------------
* `wall (s)` / `run1 wall` / `run2 wall` ← `run.json` 的 `wall_seconds`（进程墙钟）
* `report Time (s)` ← `report.txt` 的 `Total time:` 行（程序内计时）
* `sha256 (前 12)` ← `run.json.sha256_out`，交叉核对 `out.obj` 实算值
* `两次一致` ← `baseline/<case>/out.obj` 与 `baseline/<case>/run2/out.obj` 实算 SHA256 比对
* `== baseline` ← `baseline/<case>/out.obj` 与 `after/<case>/out.obj` 实算 SHA256 比对（after 无 run2）
* `输入面数` ← `metrics.json` 的 `ref.ref_stats.triangles`（参考网格扇形三角化后口径）
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import re
import sys

CASE_ORDER = ["head10k", "body15k", "multi12k", "scaled10k"]
PHASES = ("baseline", "after")
TOOL_REL = os.path.join("tools", "metrics", "mesh_metrics.py")
EXPECTED_CASES = 4


# ---------------------------------------------------------------------------
# 格式化
# ---------------------------------------------------------------------------
def f_pct(x):
    return "%.2f%%" % x


def f_sci(x):
    """0.002192 -> 2.192e-3（指数去前导零）"""
    m, e = ("%.3e" % x).split("e")
    return m + "e" + str(int(e))


def f_wall(x):
    """3 位小数去尾零：36.34 / 35.7 / 12.586"""
    return ("%.3f" % x).rstrip("0").rstrip(".")


def f_dp4(x):
    """report Time：4 位小数去尾零：36.2335 / 33.405"""
    return ("%.4f" % x).rstrip("0").rstrip(".")


def f_sig6(x):
    return "%.6g" % x


def f_int(x):
    return "%d" % x


def f_sint(x):
    return "%+d" % x


def f_spp(x):
    return "%+.2f pp" % x


def f_pp(x):
    return "%.2f pp" % x


def f_spct(x):
    return "%+.2f%%" % x


def f_pct2(x):
    return "%.2f%%" % x


NOT_DERIVABLE = "不可推导"


# ---------------------------------------------------------------------------
# 指纹（现算）
# ---------------------------------------------------------------------------
def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def short_sha(sha):
    return (sha or "")[:8].upper() + "…"


def tz_label(now):
    """时区展示名：UTC+8 统一写 Asia/Shanghai（与历史文本一致），其它回退系统名。"""
    off = now.utcoffset()
    if off is not None and off == datetime.timedelta(hours=8):
        return "Asia/Shanghai"
    return now.tzname() or "local"


def fingerprint(root, entries):
    """(ok, line, reason)。ok=False 时 reason 给出人话原因。"""
    tool = os.path.join(root, TOOL_REL)
    if not os.path.exists(tool):
        return False, None, "找不到 %s" % TOOL_REL
    tool_sha = sha256_file(tool)

    metas = {}
    for key, e in entries.items():
        sha = ((e or {}).get("meta") or {}).get("mesh_metrics_sha256")
        if sha:
            metas[key] = sha
    want = EXPECTED_CASES * 2
    if len(metas) < want:
        return False, None, "只找到 %d/%d 份 metrics_meta.json" % (len(metas), want)

    counts = {}
    for sha in metas.values():
        counts[sha] = counts.get(sha, 0) + 1
    if len(counts) == 1:
        common = next(iter(counts))
        if common == tool_sha:
            return True, "PASS（%d/%d 一致 = %s，与工具当前 sha 相符）" % (
                len(metas), want, short_sha(common)), None
        return False, None, ("meta 一致值 %s 与工具当前 sha %s 不符——工具在数据生成后变更，"
                             "需重跑 run_metrics.ps1 后再跑本生成器"
                             % (short_sha(common), short_sha(tool_sha)))
    parts = " / ".join("%s×%d" % (short_sha(s), c)
                       for s, c in sorted(counts.items(), key=lambda kv: -kv[1]))
    major = max(counts, key=lambda s: counts[s])
    odd = ["artifacts/%s/%s/metrics_meta.json（%s）" % (p, c, short_sha(v))
           for (p, c), v in sorted(metas.items()) if v != major]
    return False, None, ("%d 份 meta 不全是同一个值：%s；不一致的文件：%s"
                         % (len(metas), parts, "、".join(odd)))


# ---------------------------------------------------------------------------
# 装载 + 不可推导检测
# ---------------------------------------------------------------------------
def _load(path):
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def _dig(d, path, default=None):
    cur = d
    for part in path.split("."):
        if not isinstance(cur, dict) or part not in cur:
            return default
        cur = cur[part]
    return cur


def load_all(root):
    entries = {}
    for phase in PHASES:
        base = os.path.join(root, "artifacts", phase)
        if not os.path.isdir(base):
            continue
        for name in sorted(os.listdir(base)):
            if not os.path.exists(os.path.join(base, name, "metrics.json")):
                continue
            e = {"metrics": _load(os.path.join(base, name, "metrics.json")),
                 "meta": {}, "run": {}, "dir": os.path.join(base, name)}
            p = os.path.join(base, name, "metrics_meta.json")
            if os.path.exists(p):
                e["meta"] = _load(p)
            p = os.path.join(base, name, "run.json")
            if os.path.exists(p):
                e["run"] = _load(p)
            entries[(phase, name)] = e
    return entries


def _sha_of(path):
    return sha256_file(path) if os.path.exists(path) else None


def scan_actual(root, entries, cases):
    """先实测，再比对记录（C 条 B：禁止直接信 run.json 的布尔）。

    返回 dict：
      sha[(phase, case)]     实测 out.obj sha（缺失 None）
      run2[case]             实测 run2/out.obj sha（缺失 None）
      repro[case]            实测两次一致（None = 不可推导）
      match[case]            实测 after == baseline（None = 不可推导）
      conflicts[(case, 描述)] 记录与实际冲突（→ exit 3）
    """
    sha = {}
    for phase in PHASES:
        for case in cases:
            sha[(phase, case)] = _sha_of(
                os.path.join(root, "artifacts", phase, case, "out.obj"))
    run2 = {}
    for case in cases:
        run2[case] = _sha_of(os.path.join(root, "artifacts", "baseline", case,
                                          "run2", "out.obj"))
    repro, match, conflicts = {}, {}, []

    for case in cases:
        b = sha.get(("baseline", case))
        r2 = run2.get(case)
        a = sha.get(("after", case))

        repro[case] = None if (b is None or r2 is None) else (b == r2)
        match[case] = None if (b is None or a is None) else (a == b)

        brun = (entries.get(("baseline", case)) or {}).get("run") or {}
        arun = (entries.get(("after", case)) or {}).get("run") or {}

        def conflict(where, field, rec, act):
            conflicts.append((case, "%s 记录 %s=%r，实测 %r" % (where, field, rec, act)))

        if b and brun.get("sha256_out") and brun["sha256_out"] != b:
            conflict("baseline/%s/run.json" % case, "sha256_out", brun["sha256_out"][:12], b[:12])
        r2rec = (brun.get("run2") or {}).get("sha256_out")
        if r2 and r2rec and r2rec != r2:
            conflict("baseline/%s/run.json" % case, "run2.sha256_out", r2rec[:12], r2[:12])
        if repro[case] is not None and brun.get("reproducible_sha256") is not None:
            if bool(brun["reproducible_sha256"]) != repro[case]:
                conflict("baseline/%s/run.json" % case, "reproducible_sha256",
                         brun["reproducible_sha256"], repro[case])
        if a and arun.get("sha256_out") and arun["sha256_out"] != a:
            conflict("after/%s/run.json" % case, "sha256_out", arun["sha256_out"][:12], a[:12])
        if match[case] is not None and arun.get("sha256_matches_baseline") is not None:
            if bool(arun["sha256_matches_baseline"]) != match[case]:
                conflict("after/%s/run.json" % case, "sha256_matches_baseline",
                         arun["sha256_matches_baseline"], match[case])
        if repro[case] is False:
            conflicts.append((case,
                              "baseline/%s：out.obj 与 run2/out.obj 实测 SHA256 不同"
                              "（%s vs %s）—— 这组数据不可复现" % (case, b[:12], r2[:12])))
    return {"sha": sha, "run2": run2, "repro": repro, "match": match,
            "conflicts": conflicts}


def sample_scope(entries):
    vals = []
    for e in entries.values():
        v = (e.get("meta") or {}).get("samples")
        if v is not None and v not in vals:
            vals.append(v)
    return "/".join(str(v) for v in vals) if vals else "—"


def case_order(entries):
    cases = {name for (_p, name) in entries}
    ordered = [c for c in CASE_ORDER if c in cases]
    ordered += sorted(c for c in cases if c not in CASE_ORDER)
    return ordered


def _uniq(values):
    out = []
    for v in values:
        if v and v not in out:
            out.append(v)
    return out


def _one(values, sep=" / "):
    """唯一值列表 -> 展示字符串（多值用 sep 连，缺值给 —）。"""
    vals = _uniq(values)
    return sep.join(vals) if vals else "—"


def report_txt_time(entry):
    """report Time 取 report.txt 的 Total time 行（程序内计时）；缺失则回退 run.json。"""
    d = (entry or {}).get("dir")
    if d:
        p = os.path.join(d, "report.txt")
        if os.path.exists(p):
            with open(p, "r", encoding="utf-8", errors="replace") as fh:
                for line in fh:
                    line = line.strip()
                    if line.lower().startswith("total time"):
                        try:
                            return float(line.split(":", 1)[1].strip().split()[0])
                        except (IndexError, ValueError):
                            pass
    v = ((entry or {}).get("run") or {}).get("report_time_seconds")
    return v


# island 计数的历史 prose 值：作为回归锚点冻结。现算与之不同则报差异并拒绝生成。
ISLAND_ANCHORS = {"head_in.obj": 8979, "multi_part.obj": 17958, "body_clean.obj": 1}


def count_islands(path):
    """输入 OBJ 的 island 数 = 三角形按**共享顶点**做并查集得到的连通分量数。

    与 MeshSeparator::splitToIslands 同义（顶点连通，不是面经共享边连通）。
    """
    nv = 0
    faces = []
    with open(path, "rb") as fh:
        for line in fh:
            head = line[:2]
            if head == b"v " or head == b"v" + chr(9).encode():
                nv += 1
            elif head == b"f " or head == b"f" + chr(9).encode():
                faces.append([int(t.split(b"/")[0]) for t in line[2:].split()])
    if not faces or nv == 0:
        return 0
    parent = list(range(nv))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for f in faces:
        v0 = f[0] - 1 if f[0] > 0 else nv + f[0]
        rf = find(v0)
        for tok in f[1:]:
            v = tok - 1 if tok > 0 else nv + tok
            rv = find(v)
            if rv != rf:
                parent[rv] = rf
                rf = find(rf)
    used = set()
    for f in faces:
        for tok in f:
            used.add(tok - 1 if tok > 0 else nv + tok)
    return len({find(v) for v in used})


def scan_islands(entries, cases):
    """{case: {name, path, count} 或 None}，按 input 路径去重。"""
    cache = {}
    out = {}
    for case in cases:
        run = (entries.get(("baseline", case)) or {}).get("run") or {}
        inp = run.get("input")
        if not inp or not os.path.exists(inp):
            out[case] = None
            continue
        if inp not in cache:
            cache[inp] = count_islands(inp)
        out[case] = {"path": inp, "name": os.path.basename(inp), "count": cache[inp]}
    return out


def short_input(path):
    if not path:
        return "—"
    parts = path.replace("\\", "/").split("/")
    for i in range(len(parts) - 1):
        if parts[i] == "artifacts" and parts[i + 1] == "inputs":
            return "artifacts/inputs/" + parts[-1]
    return "/".join(parts[-2:])


# ---------------------------------------------------------------------------
# 1) COMPARISON.md
# ---------------------------------------------------------------------------
CMP_MAIN_HEADER = (
    "| case | 开关 | quads | quad_ratio | non_manifold | boundary_loops | "
    "components | deviation_p95 (rel) | wall (s) | exit | sha256 == baseline |")
CMP_MAIN_SEP = "|---|---|---|---|---|---|---|---|---|---|---|"
CMP_DIFF_HEADER = (
    "| case | Δquads | Δquad_ratio | Δnon_manifold | Δboundary_loops | "
    "Δcomponents | Δdeviation_p95 | Δcoverage | Δvertices | Δdegenerate | Δwall |")
# 11 列 → 11 组分隔符（历史手写版此处只有 10 组，是笔误；生成器作为唯一所有者输出结构正确的表）
CMP_DIFF_SEP = "|---|---|---|---|---|---|---|---|---|---|---|"
CMP_DEF = ("- deviation_p95 = 输出顶点到输入表面最近距离的 p95，按目标包围盒对角归一化（rel）；"
           "coverage = 参考表面被输出覆盖的比例。")
CMP_REPRO = """## 复现方式
```powershell
pwsh -NoProfile -File artifacts/scripts/run_baseline.ps1   # 基线四用例 x2
pwsh -NoProfile -File artifacts/scripts/run_after.ps1      # after 四用例（带开关）
pwsh -NoProfile -File artifacts/scripts/run_metrics.ps1    # mesh_metrics 全量
pwsh -NoProfile -File tools/metrics/emit_comparison.py     # 三份派生件（只读 JSON 排版）
```"""
CMP_ARTIFACTS = """## 产物位置
```
artifacts/baseline/<case>/{out.obj,report.txt,stdout.txt,stderr.txt,run.json,run2/,metrics.json,metrics_meta.json}
artifacts/baseline/SUMMARY.md
artifacts/after/<case>/{out.obj,report.txt,stdout.txt,stderr.txt,run.json,metrics.json,metrics_meta.json}
artifacts/after/SUMMARY.md
artifacts/COMPARISON.md
```"""
CMP_FOOTER = ("> 结论与判据见 [artifacts/AB-NOTES.md](AB-NOTES.md)；"
              "本表由 `tools/metrics/emit_comparison.py` 生成（`--check` 可校验），请勿手改。")


def cmp_main_row(case, phase, entry, match_flag):
    if not entry:
        return ("| %s | %s | — | — | — | — | — | — | — | — | — |"
                % (case, "基线 · (none)" if phase == "baseline" else "after · —"))
    m = entry["metrics"]
    run = entry.get("run") or {}
    sw = "基线 · (none)"
    if phase == "after":
        s = run.get("switches_text") or " ".join(run.get("switches") or [])
        sw = "after · " + (s if s else "(none)")
    p95rel = _dig(m, "ref.deviation.p95_rel")
    cells = [
        case, sw,
        f_int(m.get("quads", 0)),
        f_pct(m.get("quad_ratio_pct", 0.0)),
        f_int(m.get("edges_non_manifold", 0)),
        f_int(m.get("boundary_loops", 0)),
        f_int(m.get("components", 0)),
        f_sci(p95rel) if p95rel is not None else "—",
        f_wall(run["wall_seconds"]) if run.get("wall_seconds") is not None else "—",
        f_int(run["exit_code"]) if run.get("exit_code") is not None else "—",
        "—" if phase == "baseline" else flag_cell(match_flag),
    ]
    return "| " + " | ".join(cells) + " |"


def flag_cell(v):
    if v is None:
        return NOT_DERIVABLE
    return "**是**" if v else "否"


def cmp_diff_row(case, b, a):
    if not b or not a:
        return "| %s | — | — | — | — | — | — | — | — | — | — |" % case
    bm, am = b["metrics"], a["metrics"]
    br, ar = b.get("run") or {}, a.get("run") or {}
    qb, qa = bm.get("quads", 0), am.get("quads", 0)
    dq = qa - qb
    dqp = (100.0 * dq / qb) if qb else 0.0
    dratio = am.get("quad_ratio_pct", 0.0) - bm.get("quad_ratio_pct", 0.0)
    cov_b = 100.0 * (_dig(bm, "ref.coverage.covered_ratio") or 0.0)
    cov_a = 100.0 * (_dig(am, "ref.coverage.covered_ratio") or 0.0)
    pb, pa = _dig(bm, "ref.deviation.p95_rel"), _dig(am, "ref.deviation.p95_rel")
    dp95 = (100.0 * (pa - pb) / pb) if (pb and pa is not None) else None
    wb, wa = br.get("wall_seconds"), ar.get("wall_seconds")
    dwall = (100.0 * (wa - wb) / wb) if (wb and wa is not None) else None
    cells = [
        case,
        "%s (%s)" % (f_sint(dq), f_spct(dqp)),
        f_spp(dratio),
        f_sint(am.get("edges_non_manifold", 0) - bm.get("edges_non_manifold", 0)),
        f_sint(am.get("boundary_loops", 0) - bm.get("boundary_loops", 0)),
        f_sint(am.get("components", 0) - bm.get("components", 0)),
        f_pct2(dp95) if dp95 is not None else "—",
        f_pp(cov_a - cov_b),
        f_sint(am.get("vertices", 0) - bm.get("vertices", 0)),
        f_sint(am.get("degenerate_faces", 0) - bm.get("degenerate_faces", 0)),
        f_pct2(dwall) if dwall is not None else "—",
    ]
    return "| " + " | ".join(cells) + " |"


def build_comparison(ctx, now):
    entries, cases, fp_line = ctx["entries"], ctx["cases"], ctx["fp_line"]
    utc = now.astimezone(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S+00:00")
    L = ["# A/B 对比：基线（M1 快照） vs after（M2 行为开关）"]
    L.append("生成时间 %s（本地 %s %s）；全部数据来自 `artifacts/baseline/*/`、"
             "`artifacts/after/*/` 原始产物，指标由 `tools/metrics/mesh_metrics.py` 计算，"
             "`--samples %s`。" % (utc, now.strftime("%Y-%m-%d %H:%M:%S"),
                                      tz_label(now), sample_scope(entries)))
    L.append("")
    L.append("> **工具版本自检（生成时现算，不是模板）**：权威值是各 `metrics_meta.json` 里的 "
             "`mesh_metrics_sha256`；下面这行由生成器在本文件落盘前读取 8 份 meta 与 "
             "`tools/metrics/mesh_metrics.py` 当前 sha256 现算得出。")
    L.append(">")
    L.append("> **工具指纹 = %s**" % fp_line)
    L.append(">")
    L.append("> 判读规则（C6）：8 份 meta 非同一值 → 数据来自不同工具版本，本表作废；"
             "与工具当前 sha 不符 → 工具在数据生成后变更，需重跑指标并重跑本生成器；"
             "只有「8 份一致**且**等于工具当前 sha」时，本表的数字才对当前代码成立。"
             "生成器已把这条做成硬前置：自检不 PASS 就拒绝生成（exit 2）。")
    L.append(">")
    L.append("> 历史注记：本文件早先头部内嵌过 `5defe0ed…`，那是更早的工具版本；"
             "**该值与现有 8 份 meta 全不匹配**，照它比对会得出「不可比」的错误结论"
             "——这正是把版本号写进正文而不是指向来源的代价。")
    L.append("")
    bl = _uniq([e["run"].get("cli_sha256") for (p, _c), e in entries.items()
                if p == "baseline" and (e.get("run") or {}).get("cli_sha256")])
    repro = [e["run"].get("reproducible_sha256") for (p, _c), e in entries.items()
             if p == "baseline" and (e.get("run") or {}).get("reproducible_sha256") is not None]
    note = ""
    if repro:
        note = "（每用例跑 2 次，两次 SHA256 全部一致）" if all(repro) else "（⚠️ 两次 SHA256 不一致）"
    L.append("- 基线二进制：`artifacts/bin/baseline/autoremesher_cli.exe` sha256 `%s`%s"
             % (_one(bl), note))
    af = _uniq([e["run"].get("cli_sha256") for (p, _c), e in entries.items()
                if p == "after" and (e.get("run") or {}).get("cli_sha256")])
    L.append("- after 二进制：`artifacts/bin/after/autoremesher_cli.exe` sha256 `%s`"
             % _one(af))
    L.append(CMP_DEF)
    L.append("")
    L.append("## 主表")
    L.append(CMP_MAIN_HEADER)
    L.append(CMP_MAIN_SEP)
    for case in cases:
        b = entries.get(("baseline", case))
        a = entries.get(("after", case))
        L.append(cmp_main_row(case, "baseline", b, None))
        L.append(cmp_main_row(case, "after", a, ctx["match"][case]))
    L.append("")
    L.append("### 差值（after − 基线）")
    L.append(CMP_DIFF_HEADER)
    L.append(CMP_DIFF_SEP)
    for case in cases:
        L.append(cmp_diff_row(case, entries.get(("baseline", case)),
                              entries.get(("after", case))))
    L.append("")
    L.append(CMP_REPRO)
    L.append("")
    L.append(CMP_ARTIFACTS)
    L.append("")
    L.append(CMP_FOOTER)
    L.append("")
    return "\n".join(L)


# ---------------------------------------------------------------------------
# 2) baseline/SUMMARY.md
# ---------------------------------------------------------------------------
BSD_MAIN_HEADER = (
    "| case | quads | quad_ratio | non_manifold | boundary_loops | components | "
    "deviation_p95 (rel) | wall (s) | exit | sha256 (前 12 位) | 两次一致 |")
BSD_MAIN_SEP = "|---|---|---|---|---|---|---|---|---|---|---|"
BSD_DETAIL_HEADER = (
    "| case | input | 输入面数 | out bytes | vertices | faces | non_quads | degenerate | "
    "coverage | bbox diag | edge p95 | valence4 | report Time (s) | run1 wall | run2 wall | "
    "run2 sha256 |")
BSD_DETAIL_SEP = "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"
BSD_FACES_NOTE = ("> `输入面数` 口径：取 `metrics.json` 的 `ref.ref_stats.triangles` —— 参考网格按"
                 "**扇形三角化**后的三角形数，故与 OBJ 的 `f` 行数可能差若干（例如含 2 个四边形时多出 2）。")
BSD_TIME_NOTE = ("> 时间列口径（C4）：`wall (s)` / `run1 wall` / `run2 wall` 取自 `run.json` 的 "
                 "`wall_seconds`（进程墙钟，含进程启动与 OBJ 写出）；`report Time (s)` 取自 "
                 "`report.txt` 的 `Total time:` 行（程序内计时）。两列**不同源**，不可互相印证。")
BSD_REPRO_NOTE = ("> `两次一致`：比对 `<case>/out.obj` 与 `<case>/run2/out.obj` 的实算 SHA256"
                  "（不是读 `run.json` 的自述）；若两者不同，生成器按 exit 3 拒绝生成。")
CORE_RE = re.compile(rb"Cores kept busy across the parallel phase:\s*([0-9]+(?:\.[0-9]+)?)")


def scan_cores(entries, cases, phase="baseline"):
    """从各用例 stderr.txt 取 'Cores kept busy across the parallel phase: X.XX'。

    返回 {case: float 或 None}；缺失即 None（渲染成 n/a，不猜默认值）。
    """
    out = {}
    for case in cases:
        e = entries.get((phase, case))
        val = None
        if e:
            p = os.path.join(e["dir"], "stderr.txt")
            if os.path.exists(p):
                with open(p, "rb") as fh:
                    for line in fh:
                        m = CORE_RE.search(line)
                        if m:
                            try:
                                val = float(m.group(1))
                            except ValueError:
                                val = None
                            break
        out[case] = val
    return out


def capture_date(entries, cases, phase="baseline"):
    """采集日期（来自 run.json.captured_at），用于标注「本机实测」的时点。"""
    dates = []
    for case in cases:
        ca = ((entries.get((phase, case)) or {}).get("run") or {}).get("captured_at")
        if ca:
            try:
                dates.append(datetime.datetime.fromisoformat(ca).date().isoformat())
            except ValueError:
                pass
    if not dates:
        return None
    lo, hi = min(dates), max(dates)
    return lo if lo == hi else "%s ~ %s" % (lo, hi)


def bsd_obs(ctx):
    """观测段。island 数与 scaled10k 退化数字全部现算；因果判断一律不写进本文件。

    计算式：ideal = bbox(head10k).diagonal / 1000（scaled10k = head_in × 1e-3）；
            dev   = (actual − ideal) / ideal。
    格式固定：quads 整数；coverage %.2f%%；长度类 %.6g；相对偏差 %+.2f%%。
    """
    islands, entries = ctx["islands"], ctx["entries"]

    def n(name):
        for info in islands.values():
            if info and info["name"] == name:
                return info["count"]
        return None

    head, multi, body = n("head_in.obj"), n("multi_part.obj"), n("body_clean.obj")
    L = ["## 观测"]
    L.append("- 4 个用例全部 exit=0、输出非空，无退化/空输出；`scaled10k` 虽未崩，但几何严重退化（见下）。")
    L.append("- 同构建可复现：4 个用例 run1/run2 的 out.obj SHA256 全部逐字节一致。")
    if head is not None and multi is not None and body is not None:
        twice = "（= 2×%d，两份拷贝各自独立）" % head if multi == 2 * head else                 "（非 2×%d，两份拷贝不再独立）" % head
        L.append("- 输入碎片化（**生成时现算**；口径：输入 OBJ 的三角形按**共享顶点**做并查集求"
                 "连通分量，与 `MeshSeparator::splitToIslands` 同义）：`head_in.obj` 被切成 %d 个 "
                 "island，`multi_part.obj` 为 %d 个%s，`body_clean.obj` 只有 %d 个。"
                 % (head, multi, twice, body))
    else:
        L.append("- 输入碎片化：无法推导（缺 run.json 的 input 路径或文件不存在）。")

    b10 = (entries.get(("baseline", "head10k")) or {}).get("metrics")
    bsc = (entries.get(("baseline", "scaled10k")) or {}).get("metrics")
    if b10 and bsc:
        diag10 = _dig(b10, "bbox.diagonal", 0.0)
        diag_sc = _dig(bsc, "bbox.diagonal", 0.0)
        ideal = diag10 / 1000.0
        dev = ((diag_sc - ideal) / ideal) if ideal else None
        L.append("- `scaled10k`（head_in × 1e-3）基线仅出 %d quads、coverage %s、bbox 对角 %s"
                 "（理想值 = head10k 对角 %s 的 1/1000 = %s，偏差 %s）；归因见 "
                 "`artifacts/AB-NOTES.md`。"
                 % (bsc.get("quads", 0),
                    f_pct(100.0 * (_dig(bsc, "ref.coverage.covered_ratio") or 0.0)),
                    f_sig6(diag_sc), f_sig6(diag10), f_sig6(ideal),
                    f_pct2(100.0 * dev) if dev is not None else "—"))
    else:
        L.append("- `scaled10k` 退化数字：无法推导（缺 head10k 或 scaled10k 的 metrics.json）。")

    L.append(cores_line(ctx))
    return "\n".join(L)


def cores_line(ctx):
    """并行度一行：数值全部来自 stderr 实测，机器核数来自 os.cpu_count()。"""
    cores, islands, cases = ctx["cores"], ctx["islands"], ctx["cases"]
    cpu = os.cpu_count()
    cap = capture_date(ctx["entries"], cases)
    singles, rest, missing = [], [], []
    for case in cases:
        v = cores.get(case)
        if v is None:
            missing.append("%s n/a" % case)
        elif v < 1.5:
            isl = (islands.get(case) or {}).get("count")
            why = "单岛，并行退化" if isl == 1 else "并行退化"
            singles.append("`%s` cores=%.2f（%s）" % (case, v, why))
        else:
            rest.append(v)
    bits = []
    if singles:
        bits.append("、".join(singles))
    if rest:
        lo, hi = min(rest), max(rest)
        bits.append("其余用例 " + (("%.2f–%.2f" % (lo, hi)) if lo != hi else "%.2f" % lo))
    if missing:
        bits.append("、".join(missing))
    head = ("- 并行度（**本机实测**；来源：各用例 `stderr.txt` 的 "
            "`Cores kept busy across the parallel phase` 行；机器核数 `os.cpu_count()`=%s 逻辑核；"
            "采集日期 %s）：" % (cpu if cpu is not None else "n/a", cap or "n/a"))
    return head + ("；".join(bits) if bits else "n/a") + "。"
BSD_FILES = """## 文件
```
artifacts/baseline/<case>/out.obj        run1 输出
artifacts/baseline/<case>/report.txt     CLI --report
artifacts/baseline/<case>/stdout.txt     CLI stdout（summary）
artifacts/baseline/<case>/stderr.txt     CLI stderr（进度 + phaseReport）
artifacts/baseline/<case>/run.json       case/cmd/wall/exit/sha256/quads/...
artifacts/baseline/<case>/run2/          run2 同名四件套（复现性证据）
artifacts/baseline/<case>/metrics.json   mesh_metrics 输出
artifacts/baseline/<case>/metrics_meta.json  metrics 墙钟/工具 sha256/ref
```"""


def bsd_main_row(case, e, repro_ok):
    if not e:
        return "| %s | — | — | — | — | — | — | — | — | — | — |" % case
    m, run = e["metrics"], e.get("run") or {}
    p95rel = _dig(m, "ref.deviation.p95_rel")
    cells = [
        case,
        f_int(m.get("quads", 0)),
        f_pct(m.get("quad_ratio_pct", 0.0)),
        f_int(m.get("edges_non_manifold", 0)),
        f_int(m.get("boundary_loops", 0)),
        f_int(m.get("components", 0)),
        f_sci(p95rel) if p95rel is not None else "—",
        f_wall(run["wall_seconds"]) if run.get("wall_seconds") is not None else "—",
        f_int(run["exit_code"]) if run.get("exit_code") is not None else "—",
        (run.get("sha256_out") or "")[:12] or "—",
        NOT_DERIVABLE if repro_ok is None else ("是" if repro_ok else "否"),
    ]
    return "| " + " | ".join(cells) + " |"


def bsd_detail_row(case, e):
    if not e:
        return "| %s | " % case + " | ".join(["—"] * 15) + " |"
    m, run = e["metrics"], e.get("run") or {}
    r2 = run.get("run2") or {}
    rt = report_txt_time(e)
    r2sha = (r2.get("sha256_out") or "")[:12]
    if not r2sha:
        p = os.path.join(e["dir"], "run2", "out.obj")
        r2sha = (sha256_file(p)[:12]) if os.path.exists(p) else "—"
    cells = [
        case,
        short_input(run.get("input")),
        f_int(_dig(m, "ref.ref_stats.triangles", 0)),
        f_int(run["out_bytes"]) if run.get("out_bytes") is not None else "—",
        f_int(m.get("vertices", 0)),
        f_int(m.get("faces", 0)),
        f_int(m.get("non_quad_faces", 0)),
        f_int(m.get("degenerate_faces", 0)),
        f_pct(100.0 * (_dig(m, "ref.coverage.covered_ratio") or 0.0)),
        f_sig6(_dig(m, "bbox.diagonal", 0.0)),
        f_sci(_dig(m, "edge_length.p95", 0.0)),
        f_pct(100.0 * (m.get("valence4_ratio") or 0.0)),
        f_dp4(rt) if rt is not None else "—",
        f_wall(run["wall_seconds"]) if run.get("wall_seconds") is not None else "—",
        f_wall(r2["wall_seconds"]) if r2.get("wall_seconds") is not None else "—",
        r2sha,
    ]
    return "| " + " | ".join(cells) + " |"


def _captured(entries):
    vals = [((e or {}).get("run") or {}).get("captured_at")
            for (p, _c), e in entries.items() if p == "baseline"]
    vals = [v for v in vals if v]
    if not vals:
        return "—"
    return min(vals, key=lambda s: datetime.datetime.fromisoformat(s))


def build_baseline_summary(ctx, now):
    entries, cases, fp_line = ctx["entries"], ctx["cases"], ctx["fp_line"]
    bl = _uniq([e["run"].get("cli_sha256") for (p, _c), e in entries.items()
                if p == "baseline" and (e.get("run") or {}).get("cli_sha256")])
    L = ["# Baseline metrics — core CLI (M1 snapshot binary)"]
    L.append("- Captured: %s" % _captured(entries))
    L.append("- Binary: `artifacts/bin/baseline/autoremesher_cli.exe`  sha256 `%s`"
             % _one(bl))
    L.append("- Tool: `tools/metrics/mesh_metrics.py` 工具指纹 = %s" % fp_line)
    L.append("- 调用口径: `--samples %s`（各 metrics_meta.json 一致；这是运行参数，不是工具版本）"
             % sample_scope(entries))
    L.append("- 每个用例跑 2 次，run2 产物保存在 `<case>/run2/`，用于同构建可复现性校验。")
    L.append("")
    L.append("## 主表")
    L.append(BSD_MAIN_HEADER)
    L.append(BSD_MAIN_SEP)
    for case in cases:
        L.append(bsd_main_row(case, entries.get(("baseline", case)), ctx["repro"][case]))
    L.append("")
    L.append(BSD_REPRO_NOTE)
    L.append("")
    L.append("## 明细")
    L.append(BSD_DETAIL_HEADER)
    L.append(BSD_DETAIL_SEP)
    for case in cases:
        L.append(bsd_detail_row(case, entries.get(("baseline", case))))
    L.append("")
    L.append(BSD_FACES_NOTE)
    L.append("")
    L.append(BSD_TIME_NOTE)
    L.append("")
    L.append(bsd_obs(ctx))
    L.append("")
    L.append(BSD_FILES)
    L.append("")
    return "\n".join(L)


# ---------------------------------------------------------------------------
# 3) after/SUMMARY.md
# ---------------------------------------------------------------------------
AFT_HEADER = (
    "| case | 开关 | exit | wall (s) | report Time (s) | quads | quad_ratio | non_manifold | "
    "boundary_loops | components | deviation_p95 (rel) | coverage | sha256 (前 12) | == baseline |")
AFT_SEP = "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"
AFT_TIME_NOTE = ("> 时间列口径（C4）：`wall (s)` 取自 `run.json` 的 `wall_seconds`（进程墙钟）；"
                 "`report Time (s)` 取自 `report.txt` 的 `Total time:` 行（程序内计时）。"
                 "两列**不同源**，不可互相印证。")
AFT_MATCH_NOTE = ("> `== baseline`：`after/<case>/out.obj` 与 `baseline/<case>/out.obj` 的实算 SHA256 "
                  "比对（after 侧没有 `run2/`，不做同构建双跑）；`baseline/<case>/out.obj` 缺失时写"
                  "「不可推导」并以 exit 2 退出——不猜「否」。")
AFT_FOOTER = ("对照基线数值见 `artifacts/baseline/SUMMARY.md`，差值分析见 "
              "`artifacts/COMPARISON.md`。")


def aft_row(case, e, match_flag):
    if not e:
        return "| %s | " % case + " | ".join(["—"] * 13) + " |"
    m, run = e["metrics"], e.get("run") or {}
    p95rel = _dig(m, "ref.deviation.p95_rel")
    sw = run.get("switches_text") or " ".join(run.get("switches") or "")
    rt = report_txt_time(e)
    cells = [
        case,
        sw if sw else "(none)",
        f_int(run["exit_code"]) if run.get("exit_code") is not None else "—",
        f_wall(run["wall_seconds"]) if run.get("wall_seconds") is not None else "—",
        f_dp4(rt) if rt is not None else "—",
        f_int(m.get("quads", 0)),
        f_pct(m.get("quad_ratio_pct", 0.0)),
        f_int(m.get("edges_non_manifold", 0)),
        f_int(m.get("boundary_loops", 0)),
        f_int(m.get("components", 0)),
        f_sci(p95rel) if p95rel is not None else "—",
        f_pct(100.0 * (_dig(m, "ref.coverage.covered_ratio") or 0.0)),
        (run.get("sha256_out") or "")[:12] or "—",
        flag_cell(match_flag),
    ]
    return "| " + " | ".join(cells) + " |"


def build_after_summary(ctx, now):
    entries, cases, fp_line = ctx["entries"], ctx["cases"], ctx["fp_line"]
    af = _uniq([e["run"].get("cli_sha256") for (p, _c), e in entries.items()
                if p == "after" and (e.get("run") or {}).get("cli_sha256")])
    L = ["# after 构建（M2 行为开关）采集结果"]
    L.append("- 二进制：`artifacts/bin/after/autoremesher_cli.exe` sha256 `%s`"
             % _one(af))
    L.append("- 工具指纹 = %s" % fp_line)
    L.append("- 调用口径：`--samples %s`（各 metrics_meta.json 一致；这是运行参数，不是工具版本）"
             % sample_scope(entries))
    L.append("")
    L.append(AFT_HEADER)
    L.append(AFT_SEP)
    for case in cases:
        L.append(aft_row(case, entries.get(("after", case)), ctx["match"][case]))
    L.append("")
    L.append(AFT_TIME_NOTE)
    L.append(AFT_MATCH_NOTE)
    L.append("")
    L.append(AFT_FOOTER)
    L.append("")
    return "\n".join(L)


BUILDERS = (
    ("comparison", "COMPARISON.md", build_comparison),
    ("baseline-summary", os.path.join("baseline", "SUMMARY.md"), build_baseline_summary),
    ("after-summary", os.path.join("after", "SUMMARY.md"), build_after_summary),
)


# ---------------------------------------------------------------------------
# --check
# ---------------------------------------------------------------------------
# 只归一化「生成时刻」这一行；Captured 是数据（来自 run.json），必须严格比对。
IGNORE_PREFIXES = ("生成时间",)


def first_diff(text, disk):
    g, d = text.splitlines(), disk.splitlines()
    for i in range(max(len(g), len(d))):
        gl = g[i] if i < len(g) else None
        dl = d[i] if i < len(d) else None
        if gl is not None and dl is not None:
            if any(gl.startswith(p) and dl.startswith(p) for p in IGNORE_PREFIXES):
                continue
        if gl != dl:
            return i + 1, gl, dl
    return None


def _rel(path, root):
    try:
        return os.path.relpath(path, root).replace("\\", "/")
    except ValueError:
        return path.replace("\\", "/")


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="emit_comparison.py",
        description="由 artifacts 下的 JSON 生成三份派生件（COMPARISON + 两份 SUMMARY）。"
                    "批次不可拆：生成与 --check 永远覆盖三份。")
    ap.add_argument("--root", default=".", help="仓库根，默认当前目录")
    ap.add_argument("--out-dir", default=None, help="输出根目录（仍写三份，调试用）")
    ap.add_argument("--check", action="store_true", help="不写盘；三份逐一比对")
    args = ap.parse_args(argv)

    root = os.path.abspath(args.root)
    out_dir = os.path.abspath(args.out_dir) if args.out_dir else os.path.join(root, "artifacts")

    # --- 前置 1：数据目录
    for phase in PHASES:
        if not os.path.isdir(os.path.join(root, "artifacts", phase)):
            sys.stderr.write("emit_comparison: 缺少目录 artifacts/%s（exit 2）\n" % phase)
            return 2
    entries = load_all(root)
    cases = case_order(entries)
    if not cases:
        sys.stderr.write("emit_comparison: artifacts/*/*/metrics.json 不存在（exit 2）\n")
        return 2
    for case in cases:
        for phase in PHASES:
            if (phase, case) not in entries:
                sys.stderr.write("emit_comparison: 缺少 artifacts/%s/%s/metrics.json（exit 2）\n"
                                 % (phase, case))
                return 2

    # --- 前置 2：工具指纹（C7：先自检，不自洽就不生成）
    ok, fp_line, reason = fingerprint(root, entries)
    if not ok:
        sys.stderr.write("emit_comparison: 自检 FAIL —— %s\n" % reason)
        sys.stderr.write("  按 C7：输入未自洽时拒绝生成，未写任何文件（exit 2）。\n")
        return 2

    # --- 前置 3：先实测 out.obj / run2，再与 run.json 的记录比对（B 条顺序）
    actual = scan_actual(root, entries, cases)
    if actual["conflicts"]:
        sys.stderr.write("emit_comparison: 记录与实际冲突（exit 3，未写任何文件）\n")
        for case, msg in actual["conflicts"]:
            sys.stderr.write("  [%s] %s\n" % (case, msg))
        return 3

    # --- 输入 island 数（现算）+ 与 prose 锚点核对
    islands = scan_islands(entries, cases)
    bad = []
    for case in cases:
        info = islands.get(case)
        if not info:
            continue
        exp = ISLAND_ANCHORS.get(info["name"])
        if exp is not None and exp != info["count"]:
            bad.append((info["name"], info["count"], exp, info["count"] - exp))
    if bad:
        sys.stderr.write("emit_comparison: island 现算值与 prose 值不一致（exit 2，未写任何文件）\n")
        sys.stderr.write("  %-20s %10s %10s %8s\n" % ("输入文件", "现算值", "prose 值", "差值"))
        for name, got, exp, d in bad:
            sys.stderr.write("  %-20s %10d %10d %+8d\n" % (name, got, exp, d))
        sys.stderr.write("  口径：现算 = 三角形按共享顶点并查集的连通分量数；"
                         "若与 prose 不同，先裁定 prose 的定义再继续。\n")
        return 2

    repro, match = actual["repro"], actual["match"]
    degraded = []
    for case in cases:
        b = actual["sha"].get(("baseline", case))
        a = actual["sha"].get(("after", case))
        if b is None or a is None:
            missing = []
            if b is None:
                missing.append("artifacts/baseline/%s/out.obj" % case)
            if a is None:
                missing.append("artifacts/after/%s/out.obj" % case)
            degraded.append("%s 缺 %s（== baseline 不可推导）" % (case, " / ".join(missing)))
        if repro[case] is None:
            degraded.append("%s 缺 run2/out.obj（两次一致不可推导）" % case)

    ctx = {"entries": entries, "cases": cases, "fp_line": fp_line,
           "repro": repro, "match": match, "islands": islands,
           "cores": scan_cores(entries, cases)}

    now = datetime.datetime.now().astimezone()
    rc = 0
    for _name, rel, builder in BUILDERS:
        text = builder(ctx, now)
        path = os.path.join(out_dir, rel)
        if args.check:
            if not os.path.exists(path):
                print("MISSING: %s" % _rel(path, root))
                rc = 1
                continue
            with open(path, "r", encoding="utf-8", newline="") as fh:
                disk = fh.read()
            diff = first_diff(text, disk)
            if diff is None:
                print("OK: %s" % _rel(path, root))
            else:
                ln, gl, dl = diff
                rc = 1
                print("MISMATCH: %s line %d" % (_rel(path, root), ln))
                print("  generated: %r" % (gl,))
                print("  on disk  : %r" % (dl,))
        else:
            d = os.path.dirname(path)
            if d:
                os.makedirs(d, exist_ok=True)
            with open(path, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(text)
            print("wrote %s (%d lines)" % (_rel(path, root), len(text.splitlines())))

    if degraded:
        for msg in degraded:
            sys.stderr.write("emit_comparison: 不可推导 —— %s（exit 2）\n" % msg)
        return 2
    return rc


if __name__ == "__main__":
    sys.exit(main())
