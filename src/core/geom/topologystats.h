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
};

// Count a polygon soup as it is: a repeated index or a zero-area face is counted
// and still contributes its edges, because an input check has to describe the
// file, not the file it should have been.
TopologyStats analyzeTopology(const std::vector<Vector3>& vertices,
    const std::vector<std::vector<size_t>>& faces);

}

#endif
