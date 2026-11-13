#!/usr/bin/env python3
"""Enforce the core module layering.

Modules may only include modules at or below themselves:

    base < geom < numeric < field < cover < param < extract < pipeline

Includes are read through the public AutoRemesher forwarders, so the check works
on the names the code actually writes rather than on relative file paths.  The
core is also forbidden from including Qt: that is what keeps the library
buildable on machines without Qt (see D9).

Exit code 0 = clean, 1 = violation, 2 = the tree does not look like a core tree.
"""

import os
import re
import sys

MODULES = ["base", "geom", "numeric", "field", "cover", "param", "extract", "pipeline"]
LAYER = {name: index for index, name in enumerate(MODULES)}
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')

ALLOWED_THIRD_PARTY = ("Eigen/", "tbb/", "oneapi/tbb/", "meshoptimizer.h", "isotropicremesher.h",
                       "isotropichalfedgemesh.h", "axisalignedboundingboxtree.h",
                       "axisalignedboundingbox.h")


def is_standard_header(header):
    """Standard library headers are angle-bracket names without a slash or dot."""
    if "/" in header or "." in header:
        return False
    return not (header.startswith("Q") or header.startswith("Qt") or header.lower().startswith("windows"))


def build_maps(root):
    module_of_file = {}
    for module in MODULES:
        directory = os.path.join(root, "src", "core", module)
        if not os.path.isdir(directory):
            continue
        for name in os.listdir(directory):
            module_of_file[name] = module
    forwarder_to_module = {}
    include_dir = os.path.join(root, "include", "AutoRemesher")
    if os.path.isdir(include_dir):
        for name in os.listdir(include_dir):
            with open(os.path.join(include_dir, name), "r", encoding="utf-8", errors="replace") as handle:
                for line in handle:
                    match = INCLUDE.match(line)
                    if match:
                        target = os.path.basename(match.group(1))
                        if target in module_of_file:
                            forwarder_to_module[name] = module_of_file[target]
    return module_of_file, forwarder_to_module


def check(root):
    module_of_file, forwarder_to_module = build_maps(root)
    if not module_of_file:
        print("no modules found under src/core", file=sys.stderr)
        return 2
    violations = []
    checked = 0
    for module in MODULES:
        directory = os.path.join(root, "src", "core", module)
        if not os.path.isdir(directory):
            continue
        for name in sorted(os.listdir(directory)):
            if not name.endswith((".h", ".cpp")):
                continue
            checked += 1
            path = os.path.join(directory, name)
            with open(path, "r", encoding="utf-8", errors="replace") as handle:
                for number, line in enumerate(handle, 1):
                    match = INCLUDE.match(line)
                    if not match:
                        continue
                    header = match.group(1)
                    if header.startswith("Q") or header.startswith("Qt"):
                        violations.append("%s:%d includes Qt header <%s>" % (path, number, header))
                        continue
                    if header.startswith("AutoRemesher/"):
                        target_module = forwarder_to_module.get(header.split("/", 1)[1])
                        if target_module is None:
                            violations.append("%s:%d unknown forwarder <%s>" % (path, number, header))
                        elif LAYER[target_module] > LAYER[module]:
                            violations.append("%s:%d %s includes upward into %s (<%s>)"
                                              % (path, number, module, target_module, header))
                        continue
                    if header.startswith(ALLOWED_THIRD_PARTY) or is_standard_header(header):
                        continue
                    violations.append("%s:%d unexpected include <%s>" % (path, number, header))
    for violation in violations:
        print(violation)
    print("checked %d files, %d violation(s)" % (checked, len(violations)))
    return 1 if violations else 0


def resolve_root(argument):
    """Accept a repo root, a src directory, or the core directory itself.

    CI is not the only caller: a human will type a path at least once, and a
    wrong root used to look like a violation (exit 2) instead of a usage error.
    """
    root = os.path.abspath(argument)
    candidates = [root, os.path.dirname(root), os.path.dirname(os.path.dirname(root))]
    for candidate in candidates:
        if os.path.isdir(os.path.join(candidate, "src", "core")):
            return candidate
    return root


if __name__ == "__main__":
    sys.exit(check(resolve_root(sys.argv[1] if len(sys.argv) > 1 else ".")))