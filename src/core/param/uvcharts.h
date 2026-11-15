/*
 *  Copyright (c) 2026 Jeremy HU <jeremy-at-dust3d dot org>. All rights reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 */
#ifndef AUTO_REMESHER_UVCHARTS_H
#define AUTO_REMESHER_UVCHARTS_H
#include <AutoRemesher/Vector2>
#include <AutoRemesher/Vector3>
#include <cstddef>
#include <vector>

namespace AutoRemesher {

// A quad mesh split along the seams of its own UV charts.
//
// Per-vertex UVs can only describe a seam by cutting the mesh there: one vertex
// cannot carry two UVs.  The per-face chart UVs say which faces belong to the
// same chart (their UVs agree on a shared edge) and which do not (the UVs differ
// by a whole cover cell).  This splits the vertices on the boundaries between
// charts, so every face keeps its chart's UV, faces inside a chart still share
// vertices, and the seams appear exactly where the cover has them.
struct ChartSplit {
    std::vector<Vector3> vertices;
    std::vector<std::vector<size_t>> faces;
    std::vector<Vector2> vertexUvs;
    size_t chartCount = 0;
    size_t splitVertices = 0; // vertices that had to be duplicated
};

// faceUvs is one UV pair per face corner (same order as faces); a face without
// UVs is left untouched and its corners do not join any chart.  uvScale is the
// UV extent used to decide whether two faces agree (pass the bounding box
// diagonal of the UVs, or 0 to let the function estimate it).
// faceCharts, when it has one entry per face, is the cover's own chart id for that
// face and skips the inference above entirely - the solver knows where it cut.
ChartSplit splitAtChartSeams(const std::vector<Vector3>& vertices,
    const std::vector<std::vector<size_t>>& faces,
    const std::vector<std::vector<Vector2>>& faceUvs,
    const std::vector<int>& faceCharts = std::vector<int>(),
    double uvScale = 0.0);

// How well the UVs a remesh produced can actually be baked.
//
// "Bakeable" is not "the UVs look fine in the viewport".  A bake samples the map
// per texel, so a layout only works when the charts stay inside the [0,1] cover
// square (area <= 1) and no two triangles fight over the same texel (overlap
// ~ 0).  A layout can be seamless and still fail this: the remesher lays charts
// out as strips and normalises the whole mesh by max(spanU, spanV), which leaves
// charts stacked on top of each other, and corner UVs are per-triangle affine
// extrapolations that do not stitch neighbouring faces of one chart.
//
// Every number here is defined so that an independent measurement of the
// exported OBJ reproduces it - the fan-triangulated face areas and a 512x512
// coverage grid are what tools/capability_gate.py and an actual bake see -
// because a metric the consumer cannot cross-check against Blender is worth
// less than no metric.  Fan triangulation (0,k,k+1) is part of that definition:
// the corner/hybrid/seam modes emit quads whose UVs are computed per triangle,
// so a folded corner has to be measured per triangle on both sides of the table.
struct UvStats {
    // Sum of |det|/2 over the fan triangles of every face, in cover-square UV
    // units.  It is a hard lower bound rather than a heuristic: if the area
    // exceeds 1 the triangles cannot all fit inside [0,1]^2 without covering
    // something twice.
    double area = 0.0;
    // Percentage of covered cells that more than one triangle covers, over the
    // [0,1]^2 square cut into 512x512 cells.  A cell counts as covered when its
    // centre lies inside a triangle; the per-cell count saturates at 2 because
    // the question is only "is this texel claimed twice", and a fifth overlapping
    // chart does not make the bake any more broken than the second one.
    // Cells whose centre falls outside the square are dropped, never clamped to
    // the border: clamping would fold the out-of-range part of an oversized
    // layout back onto an edge and report overlap for geometry that simply is
    // not in the cover.  0 when nothing is covered.  Deterministic - one thread,
    // fixed pixel order, no container iteration order involved.
    double overlapPct = 0.0;
    // Number of distinct ids in faceCharts; 0 when the caller passed none.
    size_t chartCount = 0;
    // Relative stretch of the worst triangle, in units of the model's own texel
    // density: per triangle, (longest UV edge / longest 3D edge), then the
    // largest of those ratios divided by the median of all of them.  The raw
    // ratio is useless across models because it carries the global UV-to-world
    // scale; dividing by the median cancels it, so 1.0 reads as "no worse than
    // this model's typical triangle" and 4.0 as "squeezed four times harder",
    // whatever unit the mesh is in.  0 when no triangle was measurable, or when
    // the median itself is 0 (then the ratio carries no information).
    double maxStretch = 0.0;
    double medianStretch = 0.0;
    // area <= 1 + 1e-6 && overlapPct < 1.0, and nothing else.  Deliberately just
    // these two conditions, so the flag keeps saying exactly one thing - the
    // layout is an injective map into the cover - and stays comparable with
    // hand measurements; chartCount and the stretch numbers are diagnostics for
    // the caller, since a single chart with a huge stretch is still bakeable.
    bool bakeable = false;
    // Fan triangles left out of every metric above: UVs that are not finite, UV
    // area 0 (a collapsed or folded-flat triangle says nothing about stretch),
    // and the fan triangles of faces whose UV corner count does not match their
    // vertex count.  Surfaced because a large count means the numbers above
    // describe only part of the mesh - the usual cause is a bake that silently
    // covers half the model.
    size_t strayTriangles = 0;
};

// Measures the UVs the way a bake would see them.  vertices/faces describe the
// 3D mesh (only its edge lengths are used, for the stretch ratios); faceUvs
// holds one UV per face corner, in faces order; faceCharts is optional and only
// feeds chartCount.  Faces without a matching UV list are counted in
// strayTriangles and otherwise ignored, so a partially unwrapped mesh still
// yields usable numbers for the part that is unwrapped.

// Packs a UV layout so it can actually be baked into one texture.
//
// The remesher's own layout gives every island a horizontal strip and normalises
// the whole mesh by its largest span, which leaves the charts of one mesh stacked
// on top of each other - measured on a shoe, every mode overlapped (corner 55%,
// vertex 100%, hybrid 62%, seam 58% of the covered cells).  A baker cannot use
// that: two faces fighting over one texel smear each other.
//
// This pass groups faces into charts by their own UVs (two faces belong to one
// chart when their UVs agree along the edge they share), then gives each chart
// one rigid scale+translate, shelf-packs the chart boxes by decreasing height and
// scales the result into [0,1]^2 with a margin.  Charts never overlap, so the
// measurement after this call reports area <= 1 and overlap ~ 0 for the corner
// channel.
//
// Constraints that matter:
// - Only faceUvs is transformed.  A vertex shared by two charts cannot carry two
//   UVs, so the per-vertex channel (vertex mode) stays overlapping by
//   construction - that is what --uv-mode seam exists for.  vertexUvs, when
//   given, is re-derived from the packed corners only where every corner of that
//   vertex agrees, which is exactly the interior of a chart.
// - Deterministic: charts are numbered in face order and sorted by height with the
//   chart id as tie break, so the same mesh always packs the same way.
struct UvPackResult {
    size_t chartCount = 0;
    size_t packedCharts = 0;
    double scale = 1.0;
    double fillRatio = 0.0; // chart box area / used pack box area
    bool ok = false;
};

UvPackResult packUvCharts(const std::vector<std::vector<size_t>>& faces,
    std::vector<std::vector<Vector2>>* faceUvs,
    std::vector<Vector2>* vertexUvs,
    double margin);

UvStats collectUvStats(const std::vector<Vector3>& vertices,
    const std::vector<std::vector<size_t>>& faces,
    const std::vector<std::vector<Vector2>>& faceUvs,
    const std::vector<int>& faceCharts);

}

#endif
