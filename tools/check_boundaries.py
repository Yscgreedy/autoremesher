#!/usr/bin/env python3
"""Enforce the core module layering and the structural budgets around it.

Layout being enforced (each layer may include itself and every layer below it):

    base < geom < numeric < field < cover < param < extract < pipeline

Rules, all machine-checked here:

  R1  no Qt header, anywhere under src/core or src/cli
  R2  no upward include: <AutoRemesher/X> is resolved through the forwarding
      headers (following chains) to its module, and that module's rank must not
      exceed the includer's; <core/module/file.h> is checked directly
  R3  no relative includes; only standard headers, the allowed third party
      headers and the two project forms above
  R4  library-side output ratchet: the number of std::cout / std::cerr /
      printf / fprintf / fputs statements per module may not grow past the
      recorded baseline (tools/boundary_baseline.json)
  R5  the core and the CLI must not reach into a user interface directory
      (gui/, legacy/, ui-go/)
  R6  every directory directly under src/core must be a known module
  R7  geom/PositionKey must not use a 32-bit long for its quantised key
  R8  file size budget: a module file may not exceed SIZE_WARN lines unless it
      is listed in the baseline as an exemption, and an exempt file may not grow
      past the size recorded for it

R4 and R8 are ratchets, not bans: the current tree already violates the ideal
("no output from the library", "keep files small") and fixing that is separate
work.  A ratchet makes the debt visible and stops it from growing, and raising
a number is an explicit, reviewable diff of the baseline file.

Exit codes: 0 clean, 1 violations, 2 the tree does not look like a core tree.
"""

import argparse
import json
import os
import re
import sys

MODULES = ["base", "geom", "numeric", "field", "cover", "param", "extract", "pipeline"]
LAYER = {name: index for index, name in enumerate(MODULES)}

INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')
OUTPUT_CALL = re.compile(r'\b(std::cout|std::cerr|printf|fprintf|fputs)\b')
BARE_LONG = re.compile(r'(?<![\w])long(?![\w])')

ALLOWED_THIRD_PARTY = (
    "Eigen/",
    "tbb/",
    "oneapi/tbb/",
    "meshoptimizer.h",
    "isotropicremesher.h",
    "isotropichalfedgemesh.h",
    "axisalignedboundingboxtree.h",
    "axisalignedboundingbox.h",
)

UI_DIRECTORIES = ("gui/", "legacy/", "ui-go/", "src/gui/")
SCANNED_ROOTS = ("src/core", "src/cli")
DEFAULT_BASELINE = os.path.join("tools", "boundary_baseline.json")
SIZE_WARN = 1500
POSITIONKEY = os.path.join("src", "core", "base", "positionkey.h")


def is_standard_header(header):
    """Standard headers are angle-bracket names without a slash or a dot."""
    if "/" in header or "." in header:
        return False
    return not (header.startswith("Q") or header.startswith("Qt")
                or header.lower().startswith("windows"))


def strip_comments(text):
    """Drop // and /* */ comments so token rules do not fire on prose."""
    out = []
    index = 0
    length = len(text)
    while index < length:
        if text.startswith("//", index):
            newline = text.find("\n", index)
            if newline < 0:
                break
            out.append("\n")
            index = newline + 1
        elif text.startswith("/*", index):
            end = text.find("*/", index + 2)
            if end < 0:
                break
            out.append("\n" * text.count("\n", index, end))
            index = end + 2
        else:
            out.append(text[index])
            index += 1
    return "".join(out)


def module_files(root):
    """Map every file name under src/core/<module> to its module."""
    mapping = {}
    for module in MODULES:
        directory = os.path.join(root, "src", "core", module)
        if not os.path.isdir(directory):
            continue
        for name in os.listdir(directory):
            if name.endswith((".h", ".cpp")):
                mapping.setdefault(name, module)
    return mapping


def forwarder_targets(root):
    """Map a forwarder name (include/AutoRemesher/X) to the header it names."""
    targets = {}
    include_dir = os.path.join(root, "include", "AutoRemesher")
    if not os.path.isdir(include_dir):
        return targets
    for name in os.listdir(include_dir):
        with open(os.path.join(include_dir, name), "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                match = INCLUDE.match(line)
                if match:
                    targets[name] = os.path.basename(match.group(1))
                    break
    return targets


def resolve_forwarder(name, targets, files, seen=None):
    """Follow a chain of forwarding headers to the module that owns the target."""
    seen = seen or set()
    if name in seen:
        return None
    seen.add(name)
    target = targets.get(name)
    if target is None:
        return None
    if target in files:
        return files[target]
    return resolve_forwarder(target, targets, files, seen)


def includer_rank(location):
    """Rank of the layer a file belongs to; files outside src/core sit on top."""
    parts = location.split("/")
    if len(parts) > 2 and parts[0] == "src" and parts[1] == "core" and parts[2] in LAYER:
        return LAYER[parts[2]]
    return len(MODULES)


def iter_sources(root):
    for scanned in SCANNED_ROOTS:
        directory = os.path.join(root, scanned)
        if not os.path.isdir(directory):
            continue
        for base, _dirs, names in os.walk(directory):
            for name in sorted(names):
                if name.endswith((".h", ".cpp")):
                    yield os.path.join(base, name)


def relative(path, root):
    return os.path.relpath(path, root).replace(os.sep, "/")


def check_tree(root, baseline):
    files = module_files(root)
    targets = forwarder_targets(root)
    if not files:
        print("no modules found under src/core", file=sys.stderr)
        return 2, {}

    violations = []
    output_counts = {}
    oversized = {}
    checked = 0

    for path in iter_sources(root):
        checked += 1
        location = relative(path, root)
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            text = handle.read()
        bare = strip_comments(text)

        for number, line in enumerate(text.splitlines(), 1):
            match = INCLUDE.match(line)
            if not match:
                continue
            header = match.group(1)
            if header.startswith("Q") or header.startswith("Qt"):
                violations.append("%s:%d R1 includes Qt header <%s>" % (location, number, header))
                continue
            if header.startswith("AutoRemesher/"):
                owner = resolve_forwarder(header.split("/", 1)[1], targets, files)
                if owner is None:
                    violations.append("%s:%d R2 unknown forwarder <%s>" % (location, number, header))
                elif LAYER[owner] > includer_rank(location):
                    violations.append("%s:%d R2 %s includes upward into %s (<%s>)"
                                      % (location, number, location, owner, header))
                continue
            if header.startswith("core/"):
                parts = header.split("/")
                if len(parts) != 3 or parts[1] not in LAYER:
                    violations.append("%s:%d R3 malformed core include <%s>" % (location, number, header))
                elif LAYER[parts[1]] > includer_rank(location):
                    violations.append("%s:%d R2 %s includes upward into %s (<%s>)"
                                      % (location, number, location, parts[1], header))
                continue
            if header.startswith(UI_DIRECTORIES):
                violations.append("%s:%d R5 includes a user interface header <%s>" % (location, number, header))
                continue
            if header.startswith(ALLOWED_THIRD_PARTY) or is_standard_header(header):
                continue
            violations.append("%s:%d R3 unexpected include <%s>" % (location, number, header))

        if location.startswith("src/core/"):
            module = location.split("/")[2]
            hits = len(OUTPUT_CALL.findall(bare))
            if hits:
                output_counts[module] = output_counts.get(module, 0) + hits

    core_dirs = [name for name in os.listdir(os.path.join(root, "src", "core"))
                 if os.path.isdir(os.path.join(root, "src", "core", name))]
    for name in core_dirs:
        if name not in LAYER:
            violations.append("src/core/%s R6 directory is not a known module" % name)

    positionkey = os.path.join(root, POSITIONKEY)
    if os.path.isfile(positionkey):
        with open(positionkey, "r", encoding="utf-8", errors="replace") as handle:
            for number, line in enumerate(strip_comments(handle.read()).splitlines(), 1):
                if not BARE_LONG.search(line):
                    continue
                if re.search(r'\blong\s+long\b', line):
                    line = re.sub(r'\blong\s+long\b', "", line)
                if BARE_LONG.search(line):
                    violations.append("%s/%s:%d R7 32-bit long in the quantised key"
                                      % ("src/core/base", os.path.basename(positionkey), number))

    recorded_outputs = baseline.get("r4_output_counts", {})
    for module, count in sorted(output_counts.items()):
        allowed = recorded_outputs.get(module, 0)
        if count > allowed:
            violations.append("src/core/%s R4 %d library-side output calls, baseline allows %d"
                              % (module, count, allowed))

    recorded_sizes = baseline.get("r8_exempt_files", {})
    for module in MODULES:
        directory = os.path.join(root, "src", "core", module)
        if not os.path.isdir(directory):
            continue
        for name in sorted(os.listdir(directory)):
            if not name.endswith((".cpp", ".h")):
                continue
            path = os.path.join(directory, name)
            location = relative(path, root)
            with open(path, "r", encoding="utf-8", errors="replace") as handle:
                text = handle.read()
            lines = text.count("\n") + (0 if text.endswith("\n") else 1)
            if lines > SIZE_WARN:
                oversized[location] = lines
            allowed = recorded_sizes.get(location)
            if allowed is None and lines > SIZE_WARN:
                violations.append("%s R8 %d lines exceeds the %d line budget (add it to "
                                  "r8_exempt_files after reviewing the size)" % (location, lines, SIZE_WARN))
            elif allowed is not None and lines > allowed:
                violations.append("%s R8 grew to %d lines, baseline allows %d" % (location, lines, allowed))

    print("checked %d files, %d violation(s)" % (checked, len(violations)))
    for violation in violations:
        print("  " + violation)
    return (1 if violations else 0), {"r4_output_counts": output_counts, "r8_exempt_files": oversized}


def resolve_root(argument):
    """Accept a repo root, a src directory, or the core directory itself."""
    root = os.path.abspath(argument)
    candidates = [root, os.path.dirname(root), os.path.dirname(os.path.dirname(root))]
    for candidate in candidates:
        if os.path.isdir(os.path.join(candidate, "src", "core")):
            return candidate
    return root


def load_baseline(path):
    if not os.path.isfile(path):
        return {"r4_output_counts": {}, "r8_file_sizes": {}}
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def write_baseline(path, baseline):
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(baseline, handle, indent=2, sort_keys=True)
        handle.write("\n")
    print("wrote %s" % path)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Check the src/core layering and its budgets.")
    parser.add_argument("root", nargs="?", default=".", help="repo root (or any path inside it)")
    parser.add_argument("--write-baseline", action="store_true",
                        help="record the current R4/R8 numbers instead of comparing against them")
    parser.add_argument("--baseline", default=None, help="baseline file (default tools/boundary_baseline.json)")
    args = parser.parse_args(argv)

    root = resolve_root(args.root)
    baseline_path = args.baseline or os.path.join(root, DEFAULT_BASELINE)
    baseline = load_baseline(baseline_path)

    if args.write_baseline:
        _status, measured = check_tree(root, {"r4_output_counts": {}, "r8_exempt_files": {}})
        write_baseline(baseline_path, measured)
        return 0

    status, _measured = check_tree(root, baseline)
    return status


if __name__ == "__main__":
    sys.exit(main())
