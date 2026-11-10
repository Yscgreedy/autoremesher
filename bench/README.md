# arbench — headless benchmark for the AutoRemesher core

The pipeline core (`src/AutoRemesher`) has no Qt dependency, so it can be driven
without the GUI.  `arbench` loads an OBJ, runs one remesh, and prints the wall
clock, the result counts, the peak working set and (with `--verbose`) the same
phase report the GUI shows.

## Build

```bat
bench\build_tbb.bat      :: vendored TBB 2017, static, once
bench\build_bench.bat    :: build\bench\arbench.exe
bench\build_pristine.bat :: same benchmark built from HEAD, for A/B numbers
```

These need MSVC (VS 2017 or newer) plus CMake and Ninja; the Qt SDK is not
involved.  **After touching a header that changes a class layout, delete
`build\bench` first** — a partial rebuild mixes translation units compiled
against different layouts and crashes in confusing places.

## Run

```bat
build\bench\arbench.exe --input model.obj --target-quads 15000 ^
  --adaptivity 0.3 --anisotropy 1 --sharp-edge 60 --repeat 3 --verbose
```

| option | meaning |
|---|---|
| `--target-quads` | target quad count; the core wants `2 × quads` triangles, the tool converts |
| `--edge-scaling` `--adaptivity` `--anisotropy` `--sharp-edge` `--smooth-normal` | the GUI parameters of the same name |
| `--repeat N` | run N times, report best and median |
| `--verbose` | print the per-phase report |

## Debug switches

| environment variable | effect |
|---|---|
| `AR_TRACE=1` | print every progress status line (which stage is running) |
| `AR_VERIFY_CHARGES=1` | compare the incremental cross field charges against a full recompute |
| — | an unhandled exception prints a DbgHelp symbolised stack before the process exits |

## Reading the numbers

`Cores kept busy across the parallel phase` in the phase report is
`accumulated / wall`: it is 1.00 for a single-island model no matter how many
cores the machine has, because islands are the unit of parallelism.  Models made
of many disconnected patches (the `head_in` sample splits into 8979 islands)
report a much higher number, and their per-phase times are sums over islands,
not wall clock.
