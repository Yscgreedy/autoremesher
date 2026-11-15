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
    size_t splitVertices = 0;   // vertices that had to be duplicated
};

// faceUvs is one UV pair per face corner (same order as faces); a face without
// UVs is left untouched and its corners do not join any chart.  uvScale is the
// UV extent used to decide whether two faces agree (pass the bounding box
// diagonal of the UVs, or 0 to let the function estimate it).
ChartSplit splitAtChartSeams(const std::vector<Vector3>& vertices,
    const std::vector<std::vector<size_t>>& faces,
    const std::vector<std::vector<Vector2>>& faceUvs,
    double uvScale = 0.0);

}

#endif
