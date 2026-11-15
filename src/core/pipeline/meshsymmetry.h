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
#ifndef AUTO_REMESHER_MESHSYMMETRY_H
#define AUTO_REMESHER_MESHSYMMETRY_H
#include <AutoRemesher/Vector2>
#include <AutoRemesher/Vector3>
#include <cstddef>
#include <vector>

namespace AutoRemesher {

// What the symmetry pass did.  The pass cuts the quad mesh at the mirror plane,
// welds the cut and mirrors the positive half onto the negative one, so the
// result is symmetric by construction.  The numbers worth reporting are how much
// had to be cut and how many faces the mirror added.
struct SymmetryStats {
    int axis = 0;
    size_t seamVertices = 0;
    size_t clippedFaces = 0;   // faces the plane cut in two
    size_t mirroredFaces = 0;  // faces added by the mirror
    size_t verticesBefore = 0;
    size_t verticesAfter = 0;
    double plane = 0.0;        // coordinate of the mirror plane along the axis
    double tolerance = 0.0;    // how close to the plane counts as on the seam
};

struct SymmetryResult {
    std::vector<Vector3> vertices;
    std::vector<std::vector<size_t>> faces;
    std::vector<std::vector<Vector2>> faceUvs;
    std::vector<Vector2> vertexUvs;
    SymmetryStats stats;
};

// Mirror-symmetrise a quad mesh across axis (1 = x, 2 = y, 3 = z) at plane.
// tolerance is in model units: vertices closer than that to the plane are the
// seam.  UVs are carried through the cut (interpolated for the corners the cut
// creates) and mirrored with their faces; pass an empty faceUvs to skip them.
// A face lying entirely on the plane is dropped: it has no thickness to mirror.
// vertexUvs is per vertex (same order as vertices) and may be empty; faceUvs is
// per face corner and may be empty.  Both, either or neither may be supplied.
SymmetryResult mirrorSymmetrize(const std::vector<Vector3>& vertices,
    const std::vector<std::vector<size_t>>& faces,
    const std::vector<std::vector<Vector2>>& faceUvs,
    const std::vector<Vector2>& vertexUvs,
    int axis, double plane, double tolerance);

}

#endif
