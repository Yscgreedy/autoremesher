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
#ifndef AUTO_REMESHER_TOPOLOGYSTATS_H
#define AUTO_REMESHER_TOPOLOGYSTATS_H
#include <AutoRemesher/Vector3>
#include <cstddef>
#include <vector>

namespace AutoRemesher {

// Topology of a polygon soup, counted with shared-edge adjacency - the same
// convention tools/metrics/mesh_metrics.py uses, so the numbers a caller reads
// out of the CLI and out of the Python toolkit describe the same thing.
struct TopologyStats {
    size_t vertices = 0;
    size_t faces = 0;
    size_t triangles = 0;
    size_t quads = 0;
    size_t nonQuads = 0;
    size_t boundaryEdges = 0;
    size_t boundaryLoops = 0;
    size_t nonManifoldEdges = 0;
    size_t components = 0;
    size_t degenerateFaces = 0;

    // "Non-manifold" means two different things to two different tools: here it
    // is an edge used by more than two faces, while Blender's Non Manifold
    // select also picks up boundary edges. One number forces the consumer to
    // guess which convention it is looking at, and a report that quietly
    // disagrees with Blender by the boundary count is worse than no report, so
    // both halves are spelled out. nonManifoldEdges keeps its historical
    // meaning; edgesWithMoreThanTwoFaces is that same count under a name that
    // cannot be misread, and nonManifoldEdgesBlender is the Blender-side sum.
    size_t edgesWithMoreThanTwoFaces = 0;
    size_t nonManifoldEdgesBlender = 0;
};

// Count a polygon soup as it is: a repeated index or a zero-area face is counted
// and still contributes its edges, because an input check has to describe the
// file, not the file it should have been.
TopologyStats analyzeTopology(const std::vector<Vector3>& vertices,
    const std::vector<std::vector<size_t>>& faces);

// A quantile triple over a set of measured values, kept as plain doubles rather
// than a sorted vector so the struct stays copyable and cheap to put in a JSON
// report.  The quantiles are order statistics (an actual value from the mesh, no
// interpolation), because these numbers are compared against another tool's
// percentiles and an interpolated value is harder to explain than a coarse rank.
struct QualityRange {
    double min = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    double max = 0.0;
};

// Quality of a quad mesh in the terms a retopology reviewer uses: how many
// unexpected poles sit away from the border, how uniform the edge lengths are,
// and how far a quad is from a square.  None of this is topology - it needs the
// vertex positions - which is why it is a separate struct: analyzeTopology has
// to stay cheap and position-free for the input checks that run before any
// geometry is trusted.
struct QualityStats {
    // Interior means "the faces around this vertex close up". A vertex no face
    // touches has no valence at all, so it is not a surface vertex and is not
    // counted here; a stray point would otherwise be reported as a valence-0
    // pole, i.e. as a defect the surface does not have.
    size_t interiorVertices = 0;
    // Interior vertices whose valence is not 4. Boundary vertices are excluded
    // by construction: their valence is low by design and counting them would
    // make every open model look full of poles.
    size_t poleCount = 0;
    double polesPct = 0.0;
    // Over all vertices, boundary included, so a bad fan at the border is not
    // hidden.
    size_t valenceMax = 0;
    // Histogram over interior vertices only. A vertex with valence below 3
    // cannot be interior on a real shell, so it lands in poleCount but in no
    // bucket rather than being silently reported as >5.
    size_t valence3 = 0;
    size_t valence4 = 0;
    size_t valence5 = 0;
    size_t valenceGt5 = 0;
    // Model units, one sample per unique edge (a half-edge walk would count
    // every interior edge twice and skew nothing but is wasted work).
    QualityRange edgeLength;
    // max edge / min edge over the four sides of each quad. Triangles and n-gons
    // are left out: the ratio of a degenerate polygon has no meaning to compare
    // across tools.
    QualityRange quadAspect;
    // max_edge^2 / (4 * area) for quads. Blender's own aspect is area-relative,
    // the plain side ratio is not, so both are reported instead of arguing about
    // which one is "the" aspect.
    double quadAspectAreaMax = 0.0;
    size_t zeroLengthEdges = 0;
    size_t degenerateFaces = 0;
};

// bboxDiagonal is the bounding box diagonal of the mesh in model units - the
// only scale the caller knows.  Every relative threshold is derived from it so
// the same file measured in mm and in m agrees on what counts as degenerate.
// A non-positive diagonal falls back to absolute epsilons.
QualityStats collectQuality(const std::vector<Vector3>& vertices,
    const std::vector<std::vector<size_t>>& faces, double bboxDiagonal);

}

#endif
