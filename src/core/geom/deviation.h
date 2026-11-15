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
#ifndef AUTO_REMESHER_DEVIATION_H
#define AUTO_REMESHER_DEVIATION_H
#include <AutoRemesher/Vector3>
#include <cstddef>
#include <vector>

namespace AutoRemesher {

// How far the output sits from the surface it was made from.  A retopology tool
// that cannot answer this leaves the user with eyeballing only.
struct DeviationStats {
    size_t samples = 0;
    size_t unresolved = 0;   // points with no triangle within the search rings
    double mean = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    double max = 0.0;
};

// Distance from every point to the nearest place on the triangle surface
// (exact point-triangle distance, uniform grid over the triangles).  cellScale is
// the grid cell size in average surface edges; 8 grid rings are searched.
DeviationStats measureDeviation(const std::vector<Vector3>& points,
    const std::vector<Vector3>& surfaceVertices,
    const std::vector<std::vector<size_t>>& surfaceFaces,
    double cellScale = 2.0);

}

#endif
