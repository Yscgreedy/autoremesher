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
| `--threads N` | pin the TBB pool, e.g. to compare 1 and 32 threads for bit equality |
| `--exact-target-quads` | solve each island's cover scale from the quad budget (see below) |
| `--absolute-tolerance` | use the legacy fixed 1e-5 cross point weld instead of the default relative step |
| `--keep-island-fraction F` | keep every extracted patch above F of the total area (0 = largest only, the default) |
| `--keep-all-islands` | shorthand for a near-zero fraction; keeps every patch |
| `--weld-patches` | merge coincident patch vertices and drop duplicate faces (experimental, see below) |
| `--graded-holes` | default: seal 9-65 vertex holes with a centroid fan; `--legacy-holes` disables it |
| `--projected-caps` | also seal holes over 65 vertices with a two-ring cap projected onto the source surface (experimental) |
| `--local-remesh` | energy-guided local merge/split pass after the cleanup passes (experimental, see below) |
| `--output PATH` | write the remeshed OBJ (v/f), for `mesh_metrics.py` |
| `--verbose` | print the per-phase report |

### Local topology optimisation (experimental, off; two stages)

One energy (`sum (faceSize-4)^2 + 0.25 * sum (valence-4)^2`) drives both stages.

**Stage 1, face pair re-pairing.**  For two faces sharing an edge, the union is
either kept as one face or cut along any diagonal that is not already an edge, and
only the *end state* is scored.  That makes two-step moves reachable (a pentagon
next to a triangle becomes a hexagon and then two quads), which single-step hill
climbing could never accept.  A move is only taken when it strictly reduces the
non-quad count, or when it is a pure quad/quad diagonal swap that lowers the
valence energy.

The limit is arithmetic, not implementational: re-pairing two faces preserves the
sum of their corner counts, so "two faces become two quads" only exists for 3+3
and 3+5.  On head_clean 20k the adjacency census is 4:5 = 1690, 5:5 = 80, 3:4 =
194, 3:5 = 16, so this stage can remove about 1% of the leftovers (measured: 433
-> 423).

**Stage 2, patch re-quadrangulation.**  Cut out a small disk around a non-quad
face (<= 14 faces), drop its interior vertices and quadrangulate the boundary loop
with an explicit corner-list recursion.  Two constructions compete on one cost:

- a chord split, which cuts the polygon into two even sub-polygons (18 corners =
  1430 quadrangulations to enumerate);
- a **centre fan**: one new point at the polygon centroid, projected onto the
  source surface, with len/2 quads fanned around it so that every quad spans two
  boundary edges and one radial edge.  Nothing has to cut across the patch, which
  is what made the chord version fail on curved patches.

Because the patch interior disappears, the corner deficit that made a face a
pentagon disappears with it.  Three invariants bound the stage:

- a patch boundary is even only if the patch contains an odd sided face (growing
  by a quad changes the boundary by 4 - 2e, always even), so a patch made only of
  pentagons and quads can never be quadrangulated;
- the patch is grown **at the corners that make it fail**: the worst concave
  corners of the projected boundary (the quads covering them are concave, which the
  geometry test refuses) and boundary vertices already at valence three (replacing a
  patch costs every boundary corner one valence).  Growing a full ring instead
  usually leaves the same corners concave, which is why it kept failing;
- a quad may not reuse an edge that survives outside the patch (that edge would
  end up with three owners), although an edge the patch is about to delete is fine;
- a boundary vertex must not end up below valence 3.  Replacing a patch deletes the
  edges that ran into its interior, so each boundary corner loses one valence.  On
  head_clean this refuses about a third of the otherwise valid patches; allowing it
  reaches 345 non-quads (-20%) but adds 36 valence-2 vertices, which is a worse
  trade than the ~10% it buys.

Measured (exact target, adaptivity 0.3, anisotropy 1), both stages together:

| model | non-quads before | after | quads |
|---|---|---|---|
| head_clean 20k | 433 | **211** (-51.3%) | 19,550 -> 19,303 (-1.3%) |
| multi_part 10k | 610 | **366** (-40.0%) | 9,854 -> 9,548 (-3.1%) |
| head 1e-3 10k | 426 | **250** (-41.3%) | 10,369 -> 10,105 (-2.5%) |
| head_in 10k (8979 islands) | 432 | **222** (-48.6%) | 10,088 -> 9,865 (-2.2%) |
| body_clean 15k | 66 | **30** (-54.5%) | 15,502 -> 15,446 (-0.4%) |

Manifoldness, boundary loops and component count are unchanged; valence-2 vertices
go 64 -> 41 and normal-inconsistent edges 1060 -> 942, both better.  The price is
density (every patch replaces k faces by k/2 - 1 to k/2 faces) and valence: valence-4
share drops 90.9% -> 89.7% and irregular vertices go 1822 -> 2015, because replacing
a patch deletes the edges that ran into its interior and costs every boundary corner
one valence.  A caller that needs an exact quad count should pre-scale by 1-2%.

Sub-polygon solutions are memoised by corner sequence: the same arc is reached
through many split orders, and without the memo the search is exponential in the
corner count (28 corners took 58 s against 22 s with it, identical output hash).

Knobs: `AR_TOPO_STATS=1` prints the face size histogram, the adjacency census,
every applied move, the patch counters and the quad refusal breakdown;
`AR_PATCH_FAN` (1), `AR_PATCH_REFLEX` (0.8), `AR_PATCH_SINE` (0.05),
`AR_PATCH_VALENCE` (0.5), `AR_PATCH_VALENCE_FLOOR` (3), `AR_PATCH_FAN_MAX` (12),
`AR_PATCH_REBUILD` (0, set 1 to rebuild the edge index after every patch instead of
patching it in place),
`AR_PATCH_PLANARITY` (0.5), `AR_PATCH_FACES` (20) and `AR_PATCH_CORNERS` (22) tune
the patch search.

### Hole filling (default: graded)

Holes up to 8 vertices still use the greedy quad fill.  Loops of 9-65 vertices are
sealed with a centroid fan whose adjacent triangles are paired into quads; the
fan apex is then projected back onto the source surface by `smoothAndProject`.
On the sample models this cuts non-manifold edges by 54-75% (head_clean 128 -> 48)
and boundary loops from 5 to 1, at a cost of 0.2-0.6 pp of reference coverage.
Loops over 65 vertices are still left open; they are the remaining coverage gap
and need a projected cap with local subdivision.  `--legacy-holes` restores the
old behaviour.

### Island retention / patch weld (experimental, off)

Keeping the patches that the legacy code discards does **not** recover coverage on
the sample models (multi_part 0.407 -> 0.401, head at 1e-3 0.588 -> 0.586): the
discarded patches are overlapping fragments, not missing surface.  Welding their
coincident vertices turns the seam edges into edges shared by three faces, so
non-manifold counts rise 2-3x (132 -> 428 on multi_part).  Proper seam stitching
needs the redundant faces removed as well, which is the QEx-style local remesh
work.  These flags exist for that work; the defaults keep the legacy behaviour.

### Cross point weld (default: relative)

`PositionKey` collapses cross points that differ by float noise.  The historical
step was an absolute `1e-5`, which on a model authored in millimetres is 1% of
the model and welds the whole surface together.  The default is now one
thousandth of the island's voxel size, so every unit scale welds at the same
rate; `PositionKey` also stores `long long` and saturates instead of overflowing
past 21474 units.  `--absolute-tolerance` restores the old behaviour for A/B.

### `--exact-target-quads`

The legacy scale is `scaling x average edge length`.  With an adaptive density
field that lands 2x below the requested count (body_clean 15k target: 7,131
quads).  With the switch on, each island solves the cover scale against its
share of the budget, then corrects it once or twice from the parameterization's
UV area (one quad is one unit cell; the mesh boundary eats half a cell per unit
of UV perimeter).  Measured on the sample models: 9,999 / 10,000 (head_in),
19,479 / 20,000 (head_clean), 50,637 / 50,000, 15,536 / 15,000, 15,600 / 15,000,
4,725 / 5,000 - i.e. within 6% instead of 2x.  The extra cover solves cost
15-50% of the parameterization phase, so the switch is off by default.

## Debug switches

| environment variable | effect |
|---|---|
| `AR_TRACE=1` | print every progress status line (which stage is running) |
| `AR_VERIFY_CHARGES=1` | compare the incremental cross field charges against a full recompute |
| `AR_SIDE_DOT` | the extraction's face normal veto (default 0.259, about 75 degrees); raising or lowering it A/B tests whether the pentagons come from that veto, they do not (see section 13.2) |
| `AR_TOPO_STATS=1` | print the topology pass census: face sizes, non-quad adjacency pairs, every applied move, patch counters |
| — | an unhandled exception prints a DbgHelp symbolised stack before the process exits |

## Reading the numbers

`Cores kept busy across the parallel phase` in the phase report is
`accumulated / wall`: it is 1.00 for a single-island model no matter how many
cores the machine has, because islands are the unit of parallelism.  Models made
of many disconnected patches (the `head_in` sample splits into 8979 islands)
report a much higher number, and their per-phase times are sums over islands,
not wall clock.
