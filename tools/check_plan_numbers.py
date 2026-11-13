#!/usr/bin/env python3
"""Check that the plan document's effort ledgers agree with each other.

Three views of the same work must agree:
  1. the per-milestone table in 5.3 (sum of its 人日 column),
  2. the per-task table in 9.1 (sum of its 人日 column == its 合计 row),
  3. the ASCII formula block (each line a + b + ... == its stated total).

Usage:  python tools/check_plan_numbers.py [path-to-plan.md]
Exit:   0 consistent / 1 inconsistent / 2 cannot locate the sections
"""
import re, sys, pathlib, os

def rows(lines, start_pred, stop_pred):
    out, on = [], False
    for ln in lines:
        if not on and start_pred(ln):
            on = True; continue
        if on and stop_pred(ln):
            break
        if on: out.append(ln)
    return out

def table_rows(block, want_col):
    """Return [(label, value)] for table rows whose want_col-th cell is a number."""
    got = []
    for ln in block:
        s = ln.strip()
        if not s.startswith("|") or set(s) <= set("|- "):
            continue
        cells = [c.strip() for c in s.strip("|").split("|")]
        if len(cells) <= want_col:
            continue
        raw = cells[want_col].replace("**", "").strip()
        try:
            got.append((cells[0].replace("**", ""), float(raw)))
        except ValueError:
            continue
    return got

def main():
    root = pathlib.Path(__file__).resolve().parent.parent
    path = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else root / "docs" / "ARCHITECTURE-REFACTOR-PLAN.md"
    if not path.exists():
        print("cannot find", path); return 2
    lines = path.read_text(encoding="utf-8").splitlines()
    errs = []

    # --- 5.3 milestone table: label | name | action | 人日 | 前置  ---
    b53 = rows(lines, lambda l: l.startswith("### 5.3"), lambda l: l.startswith("### 5.4") or l.startswith("## "))
    t53 = [(lab, v) for lab, v in table_rows(b53, 3) if lab.startswith("M")]
    sum53 = sum(v for _, v in t53)

    # --- 9.1 task table: label | 人日 | 依赖 | 备注 ---
    b91 = rows(lines, lambda l: l.startswith("### 9.1"), lambda l: l.startswith("### 9.2") or l.startswith("## "))
    t91 = table_rows(b91, 1)
    total91 = [v for lab, v in t91 if "合计" in lab]
    sum91 = sum(v for lab, v in t91 if "合计" not in lab and v > 0)

    # --- formula block inside 9.1 (lines like "M3  8.5 = 3.0 + 2.0 + 0.5 + 3.0") ---
    formula = []
    for ln in b91:
        m = re.match(r"^\s*(M\d)\s+([\d.]+)\s*=\s*([\d.]+(?:\s*\+\s*[\d.]+)*)\s*$", ln)
        if m:
            parts = [float(x) for x in re.split(r"\s*\+\s*", m.group(3))]
            formula.append((m.group(1), float(m.group(2)), parts))
    for name, stated, parts in formula:
        if abs(sum(parts) - stated) > 1e-9:
            errs.append(f"formula {name}: stated {stated} != sum {sum(parts)} of {parts}")
    fsum = sum(s for _, s, _ in formula)

    if not t53: errs.append("5.3 milestone table not found / no numeric 人日 cells")
    if not t91: errs.append("9.1 task table not found")
    if len(total91) != 1: errs.append(f"9.1 must have exactly one 合计 row, found {len(total91)}")
    if abs(sum91 - sum53) > 1e-9:
        errs.append(f"5.3 sum {sum53:g} != 9.1 task sum {sum91:g}")
    if total91 and abs(total91[0] - sum91) > 1e-9:
        errs.append(f"9.1 合计 row {total91[0]:g} != its own column sum {sum91:g}")
    if formula and abs(fsum - sum91) > 1e-9:
        errs.append(f"9.1 formula per-milestone sum {fsum:g} != task sum {sum91:g}")

    print(f"5.3 rows={len(t53)} sum={sum53:g}")
    print(f"9.1 rows={len(t91) - len(total91)} sum={sum91:g} stated_total={total91[0] if total91 else None}")
    print(f"9.1 formula milestones={[n for n, _, _ in formula]} sum={fsum:g}")
    if errs:
        print("")
        for e in errs: print("INCONSISTENT:", e)
        return 1
    print("OK: ledgers agree")
    return 0

if __name__ == "__main__":
    sys.exit(main())