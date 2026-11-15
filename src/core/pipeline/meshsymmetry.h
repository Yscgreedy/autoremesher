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
    size_t clippedFaces = 0; // faces the plane cut in two
    size_t mirroredFaces = 0; // faces added by the mirror
    size_t verticesBefore = 0;
    size_t verticesAfter = 0;
    double plane = 0.0; // coordinate of the mirror plane along the axis
    double tolerance = 0.0; // how close to the plane counts as on the seam
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
// A thin wrapper over mirrorHalf(..., /*keepPositive=*/true); call mirrorHalf
// directly when the other half is the one to keep.
SymmetryResult mirrorSymmetrize(const std::vector<Vector3>& vertices,
    const std::vector<std::vector<size_t>>& faces,
    const std::vector<std::vector<Vector2>>& faceUvs,
    const std::vector<Vector2>& vertexUvs,
    int axis, double plane, double tolerance);

// One half of the mirror pass, without the measuring and snapping steps: cut at
// plane, weld the seam, mirror the kept half across it.
// keepPositive == true keeps the vertices with coordinate >= plane and mirrors
// them onto the negative side, which is what mirrorSymmetrize has always done;
// keepPositive == false keeps coordinate <= plane and mirrors it onto the
// positive side.  Both halves share one code path with the sign of the plane test
// flipped, so they cannot drift apart the way two copies eventually would.
// axis: 1 = x, 2 = y, 3 = z.  tolerance is in model units; SymmetryStats reports
// the seam tolerance actually used (it is scaled by the mesh size).
SymmetryResult mirrorHalf(const std::vector<Vector3>& vertices,
    const std::vector<std::vector<size_t>>& faces,
    const std::vector<std::vector<Vector2>>& faceUvs,
    const std::vector<Vector2>& vertexUvs,
    int axis, double plane, double tolerance, bool keepPositive);

// What evaluateSymmetry saw.  All of it is filled in even for a mesh that is
// barely symmetric: the caller deciding "snap, mirror, or leave it alone" needs
// to know how far off it is, not just whether it passed.
struct SymmetryCheck {
    int axis = 0;
    double plane = 0.0; // the mirror plane actually used
    double matchPct = 0.0; // vertices with a mirror partner, in percent
    double maxDeviation = 0.0; // model units, matched vertices only
    double meanDeviation = 0.0; // model units, matched vertices only
    size_t consideredVertices = 0;
    size_t matchedVertices = 0;
    bool planeEstimated = false; // plane came from the median estimate
};

// Measure symmetry without touching the mesh: every vertex is mirrored across
// plane and looked up among the vertices themselves, and the distance to the
// nearest one decides whether it has a partner.
// axis: 1 = x, 2 = y, 3 = z.  tolerance is in model units: a vertex whose mirror
// lands within tolerance of another vertex counts as matched.
// estimatePlane == true ignores plane and uses the median of the vertex
// coordinates along axis instead.  The median and not the mean: a shoe or a foot
// is heavier on one side, and its mean coordinate sits off the true mirror plane
// by however lopsided the mass distribution is, while the median ignores the
// tail.  A vertex lying on the plane is its own mirror: it matches at distance 0
// and is never searched for.
// faces belongs to the same mesh and is accepted for symmetry with the other
// entry points; the ratio is over all vertices, not only the referenced ones,
// because a loose vertex is still asymmetric mesh data.
SymmetryCheck evaluateSymmetry(const std::vector<Vector3>& vertices,
    const std::vector<std::vector<size_t>>& faces,
    int axis, double plane, bool estimatePlane, double tolerance);

// What snapToPlane moved.  movedVertices equals snappedVertices today; it is a
// separate counter because the next stage ("pull this vertex onto its mirror's
// position") would move vertices without snapping them onto the plane.
struct SymmetrySnapStats {
    size_t snappedVertices = 0;
    size_t movedVertices = 0;
    double maxSnap = 0.0; // model units
    double minSnap = 0.0; // model units
};

// Pull the vertices within tolerance of the plane exactly onto it and touch
// nothing else: no face is added, dropped or renumbered, so topology and face
// count survive and only the seam moves.  Vertices outside the band are left
// byte for byte alone.
// axis: 1 = x, 2 = y, 3 = z.  tolerance is in model units.
// A null pointer, an empty mesh or an out-of-range axis returns empty stats.
SymmetrySnapStats snapToPlane(std::vector<Vector3>* vertices, int axis,
    double plane, double tolerance);

}

#endif
