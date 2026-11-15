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
#ifndef AUTO_REMESHER_UVTRANSFER_H
#define AUTO_REMESHER_UVTRANSFER_H
#include <AutoRemesher/Vector2>
#include <AutoRemesher/Vector3>
#include <unordered_map>
#include <vector>

namespace AutoRemesher {

// Carries an island's parameterization onto the extracted quad mesh.
//
// The extractor works in 3D and its cleanup passes move the vertices a little, so
// a quad vertex sits close to, but not on, the isotropic triangles the
// parameterization was solved on.  A point is therefore located in that triangle
// mesh, and the UV follows from the triangle's affine map.
//
// One chart per face matters: a cover seam runs between faces, so each face asks
// for the chart under its centroid and maps all of its corners through that one
// chart.  Mapping every vertex on its own instead puts seams inside faces and
// collapses them in UV space.
class UvTransfer {
public:
    UvTransfer(const std::vector<Vector3>& vertices,
        const std::vector<std::vector<size_t>>& triangles,
        const std::vector<std::vector<Vector2>>& triangleUvs);

    // UV of point in the parameterization.  False when no triangle within eight
    // grid cells is close, i.e. the point is off the surface this island covers.
    bool locate(const Vector3& point, Vector2* uv) const;

    // Triangle whose closest point is nearest to point, so a caller can map a
    // whole face through one chart.
    bool locateTriangle(const Vector3& point, size_t* triangleIndex) const;

    // UV of point in the affine map of one triangle: the point is projected onto
    // the triangle's plane and its coordinates are applied to the triangle's UVs.
    // Extrapolates outside the triangle, which is what a quad corner larger than
    // its source triangle needs.
    bool evaluate(const Vector3& point, size_t triangleIndex, Vector2* uv) const;

    bool valid() const
    {
        return !m_triangleUvs.empty() && !m_cellStarts.empty();
    }

private:
    struct CellKey {
        int x = 0;
        int y = 0;
        int z = 0;
        bool operator==(const CellKey& other) const
        {
            return x == other.x && y == other.y && z == other.z;
        }
    };
    struct CellKeyHash {
        size_t operator()(const CellKey& key) const;
    };

    CellKey cellOf(const Vector3& point) const;
    bool locateDetailed(const Vector3& point, size_t* triangleIndex,
        double* weightA, double* weightB, double* weightC) const;

    std::vector<Vector3> m_vertices;
    std::vector<std::vector<size_t>> m_triangles;
    std::vector<std::vector<Vector2>> m_triangleUvs;
    double m_cellSize = 1.0;
    std::unordered_map<CellKey, std::pair<size_t, size_t>, CellKeyHash> m_cellStarts;
    std::vector<size_t> m_cellTriangles;
};

}

#endif
