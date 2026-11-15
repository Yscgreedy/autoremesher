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
#include <AutoRemesher/MeshSymmetry>
#include <algorithm>
#include <cmath>
#include <core/base/logging.h>
#include <map>
#include <unordered_map>

namespace AutoRemesher {

SymmetryResult mirrorSymmetrize(const std::vector<Vector3>& sourceVertices,
    const std::vector<std::vector<size_t>>& sourceFaces,
    const std::vector<std::vector<Vector2>>& sourceFaceUvs,
    const std::vector<Vector2>& vertexUvs,
    int axisValue, double plane, double tolerance)
{
    SymmetryResult result;
    SymmetryStats& stats = result.stats;
    if (axisValue < 1 || axisValue > 3 || sourceVertices.empty())
        return result;
    stats.axis = axisValue;
    stats.plane = plane;
    stats.verticesBefore = sourceVertices.size();
    const size_t axis = (size_t)(axisValue - 1);
    // The quad mesh is cut at the plane and the positive half is mirrored onto the
    // negative one, so the result is symmetric by construction instead of by
    // hope: pairing vertices after the fact only works when the topology already
    // happens to mirror.  The cut row is the price: a few faces lose a corner.
    Vector3 minimum = sourceVertices[0];
    Vector3 maximum = sourceVertices[0];
    for (const auto& vertex : sourceVertices) {
        for (size_t i = 0; i < 3; ++i) {
            minimum[i] = std::min(minimum[i], vertex[i]);
            maximum[i] = std::max(maximum[i], vertex[i]);
        }
    }
    const double diagonal = (maximum - minimum).length();
    const double seamTolerance = std::max(1e-12, tolerance * (diagonal > 0.0 ? diagonal : 1.0));
    stats.tolerance = seamTolerance;

    const bool keepUvs = sourceFaceUvs.size() == sourceFaces.size();
    const bool keepVertexUvs = !vertexUvs.empty() && vertexUvs.size() == sourceVertices.size();
    std::vector<Vector3> vertices;
    std::vector<std::vector<size_t>> faces;
    std::vector<std::vector<Vector2>> faceUvs;
    std::vector<Vector2> keptVertexUvs;
    std::vector<size_t> originalToKept(sourceVertices.size(), (size_t)-1);
    const size_t noIndex = (size_t)-1;

    const auto keepVertex = [&](size_t index) {
        if (originalToKept[index] != noIndex)
            return originalToKept[index];
        const size_t kept = vertices.size();
        vertices.push_back(sourceVertices[index]);
        if (keepVertexUvs)
            keptVertexUvs.push_back(vertexUvs[index]);
        originalToKept[index] = kept;
        return kept;
    };
    const auto snapToSeam = [&](Vector3 point) {
        if (std::fabs(point[axis] - plane) <= seamTolerance)
            point[axis] = plane;
        return point;
    };

    for (size_t faceIndex = 0; faceIndex < sourceFaces.size(); ++faceIndex) {
        const std::vector<size_t>& face = sourceFaces[faceIndex];
        if (face.size() < 3)
            continue;
        // Classify by the exact sign: a face whose vertices sit inside the seam
        // band still has a side, and dropping it would tear a hole next to the
        // plane instead of cutting it.
        bool anyPositive = false;
        bool anyNegative = false;
        for (const size_t corner : face) {
            const double coordinate = sourceVertices[corner][axis] - plane;
            if (coordinate > 0.0)
                anyPositive = true;
            else if (coordinate < 0.0)
                anyNegative = true;
        }
        if (!anyNegative) {
            std::vector<size_t> keptFace;
            keptFace.reserve(face.size());
            for (const size_t corner : face)
                keptFace.push_back(keepVertex(corner));
            faces.push_back(std::move(keptFace));
            if (keepUvs) {
                if (sourceFaceUvs[faceIndex].size() == face.size())
                    faceUvs.push_back(sourceFaceUvs[faceIndex]);
                else
                    faceUvs.push_back(std::vector<Vector2>());
            }
            continue;
        }
        if (!anyPositive)
            continue;
        // Sutherland-Hodgman against coordinate >= 0, carrying the face's own UV
        // so a corner created by the cut still samples the chart of that face.
        const bool faceHasUvs = keepUvs && sourceFaceUvs[faceIndex].size() == face.size();
        const std::vector<Vector2> sourceUvs = faceHasUvs ? sourceFaceUvs[faceIndex] : std::vector<Vector2>();
        std::vector<Vector3> clipped;
        std::vector<Vector2> clippedUvs;
        std::vector<Vector2> clippedPointUvs;
        // Which original vertex each clipped corner came from, or noIndex for a
        // point the cut created.  Reusing the original index is what keeps the
        // clipped face stitched to its neighbours: a fresh copy of an unmoved
        // corner would crack the mesh along every cut face.
        std::vector<size_t> clippedSource;
        const size_t count = face.size();
        for (size_t i = 0; i < count; ++i) {
            const size_t j = (i + 1) % count;
            const double di = sourceVertices[face[i]][axis] - plane;
            const double dj = sourceVertices[face[j]][axis] - plane;
            if (di >= 0.0) {
                clipped.push_back(sourceVertices[face[i]]);
                clippedSource.push_back(face[i]);
                if (faceHasUvs)
                    clippedUvs.push_back(sourceUvs[i]);
                if (keepVertexUvs)
                    clippedPointUvs.push_back(vertexUvs[face[i]]);
            }
            if ((di > 0.0 && dj < 0.0) || (di < 0.0 && dj > 0.0)) {
                const double ratio = di / (di - dj);
                const Vector3 point = sourceVertices[face[i]] * (1.0 - ratio)
                    + sourceVertices[face[j]] * ratio;
                clipped.push_back(snapToSeam(point));
                clippedSource.push_back(noIndex);
                if (faceHasUvs)
                    clippedUvs.push_back(sourceUvs[i] * (1.0 - ratio) + sourceUvs[j] * ratio);
                if (keepVertexUvs)
                    clippedPointUvs.push_back(vertexUvs[face[i]] * (1.0 - ratio) + vertexUvs[face[j]] * ratio);
            }
        }
        if (clipped.size() < 3)
            continue;
        ++stats.clippedFaces;
        std::vector<size_t> keptFace;
        keptFace.reserve(clipped.size());
        for (size_t i = 0; i < clipped.size(); ++i) {
            if (clippedSource[i] != noIndex) {
                keptFace.push_back(keepVertex(clippedSource[i]));
                continue;
            }
            keptFace.push_back(vertices.size());
            vertices.push_back(clipped[i]);
            if (keepVertexUvs)
                keptVertexUvs.push_back(clippedPointUvs[i]);
        }
        faces.push_back(std::move(keptFace));
        if (keepUvs)
            faceUvs.push_back(faceHasUvs ? std::move(clippedUvs) : std::vector<Vector2>());
    }

    // Everything on the plane snaps exactly onto it.
    std::vector<char> onPlane(vertices.size(), 0);
    for (size_t i = 0; i < vertices.size(); ++i) {
        if (std::fabs(vertices[i][axis] - plane) <= seamTolerance) {
            vertices[i][axis] = plane;
            onPlane[i] = 1;
        }
    }

    // Weld the seam.  Each face invented its own corner where the plane cut it, so
    // the same point exists once per face; without welding those corners the
    // mirror would leave the cut open.
    {
        std::vector<size_t> weld(vertices.size());
        for (size_t i = 0; i < vertices.size(); ++i)
            weld[i] = i;
        std::map<std::pair<long long, long long>, size_t> seamMap;
        const double cell = std::max(seamTolerance, 1e-12);
        for (size_t i = 0; i < vertices.size(); ++i) {
            if (!onPlane[i])
                continue;
            long long first = 0;
            long long second = 0;
            bool firstSeen = false;
            for (size_t k = 0; k < 3; ++k) {
                if (k == axis)
                    continue;
                const long long value = (long long)std::llround(vertices[i][k] / cell);
                if (!firstSeen) {
                    first = value;
                    firstSeen = true;
                } else {
                    second = value;
                }
            }
            const std::pair<long long, long long> key(first, second);
            // Rounding alone is not enough: two seam points a hair apart can land
            // on different sides of a cell boundary, which leaves a sliver face
            // with its own private copy of the edge.  Search the neighbouring
            // cells and take the closest point within the tolerance.
            size_t canonical = noIndex;
            double best = seamTolerance;
            for (long long da = -1; da <= 1; ++da) {
                for (long long db = -1; db <= 1; ++db) {
                    const auto found = seamMap.find({ first + da, second + db });
                    if (found == seamMap.end())
                        continue;
                    const double distance = (vertices[found->second] - vertices[i]).length();
                    if (distance <= best) {
                        best = distance;
                        canonical = found->second;
                    }
                }
            }
            if (canonical == noIndex)
                seamMap[key] = i;
            else
                weld[i] = canonical;
        }
        for (auto& face : faces) {
            std::vector<size_t> remapped;
            remapped.reserve(face.size());
            for (const size_t corner : face) {
                const size_t target = weld[corner];
                // A welded corner can meet an earlier one anywhere in the face,
                // not only at its end, so dedupe the whole polygon.
                if (std::find(remapped.begin(), remapped.end(), target) != remapped.end())
                    continue;
                remapped.push_back(target);
            }
            if (remapped.size() > 1 && remapped.front() == remapped.back())
                remapped.pop_back();
            face = remapped;
        }
        // Drop the faces the weld collapsed and compact what is still referenced.
        std::vector<std::vector<size_t>> keptFaces;
        keptFaces.reserve(faces.size());
        std::vector<std::vector<Vector2>> keptFaceUvs;
        for (size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
            if (faces[faceIndex].size() < 3)
                continue;
            keptFaces.push_back(std::move(faces[faceIndex]));
            if (keepUvs)
                keptFaceUvs.push_back(std::move(faceUvs[faceIndex]));
        }
        faces = std::move(keptFaces);
        faceUvs = std::move(keptFaceUvs);

        std::vector<size_t> compact(vertices.size(), noIndex);
        std::vector<Vector3> compacted;
        compacted.reserve(vertices.size());
        for (auto& face : faces) {
            for (auto& corner : face) {
                if (compact[corner] == noIndex) {
                    compact[corner] = compacted.size();
                    compacted.push_back(vertices[corner]);
                }
                corner = compact[corner];
            }
        }
        vertices.swap(compacted);
        if (keepVertexUvs) {
            // The compaction renumbered the vertices; the UVs have to follow, or
            // the array stops describing the mesh it is attached to.  Note the
            // swap above: the new count is vertices.size(), not compacted.size().
            std::vector<size_t> inverse(vertices.size(), noIndex);
            for (size_t i = 0; i < compact.size(); ++i) {
                if (compact[i] != noIndex)
                    inverse[compact[i]] = i;
            }
            std::vector<Vector2> compactedUvs;
            compactedUvs.reserve(vertices.size());
            for (size_t i = 0; i < vertices.size(); ++i)
                compactedUvs.push_back(inverse[i] != noIndex ? keptVertexUvs[inverse[i]] : Vector2());
            keptVertexUvs.swap(compactedUvs);
        }
    }

    onPlane.assign(vertices.size(), 0);
    for (size_t i = 0; i < vertices.size(); ++i) {
        if (vertices[i][axis] == plane)
            onPlane[i] = 1;
    }

    const size_t keptCount = vertices.size();
    std::vector<size_t> mirrorIndex(keptCount, noIndex);
    for (size_t i = 0; i < keptCount; ++i) {
        if (onPlane[i]) {
            mirrorIndex[i] = i;
            continue;
        }
        Vector3 mirrored = vertices[i];
        mirrored[axis] = 2.0 * plane - mirrored[axis];
        mirrorIndex[i] = vertices.size();
        const Vector2 mirroredUv = keepVertexUvs ? keptVertexUvs[i] : Vector2();
        vertices.push_back(mirrored);
        if (keepVertexUvs)
            keptVertexUvs.push_back(mirroredUv);
    }

    const size_t keptFaceCount = faces.size();
    for (size_t faceIndex = 0; faceIndex < keptFaceCount; ++faceIndex) {
        const std::vector<size_t> face = faces[faceIndex];
        bool hasPositive = false;
        for (const size_t corner : face) {
            if (vertices[corner][axis] > plane) {
                hasPositive = true;
                break;
            }
        }
        if (!hasPositive)
            continue;
        std::vector<size_t> mirroredFace;
        mirroredFace.reserve(face.size());
        for (auto it = face.rbegin(); it != face.rend(); ++it)
            mirroredFace.push_back(mirrorIndex[*it]);
        faces.push_back(std::move(mirroredFace));
        if (keepUvs) {
            if (faceUvs[faceIndex].empty()) {
                faceUvs.push_back(std::vector<Vector2>());
            } else {
                std::vector<Vector2> mirroredUvs(faceUvs[faceIndex].rbegin(), faceUvs[faceIndex].rend());
                faceUvs.push_back(std::move(mirroredUvs));
            }
        }
        ++stats.mirroredFaces;
    }

    result.vertices = std::move(vertices);
    result.faces = std::move(faces);
    result.faceUvs = std::move(faceUvs);
    if (keepVertexUvs && keptVertexUvs.size() == result.vertices.size())
        result.vertexUvs = std::move(keptVertexUvs);
    if (keepVertexUvs && keptVertexUvs.size() != result.vertices.size())
        Log::write(Log::Debug, "symmetry uv bookkeeping lost: " + std::to_string(keptVertexUvs.size()) + " uv(s) for " + std::to_string(result.vertices.size()) + " vertices\n");
    for (size_t i = 0; i < result.vertices.size(); ++i) {
        if (result.vertices[i][axis] == plane)
            ++stats.seamVertices;
    }
    stats.verticesAfter = result.vertices.size();
    return result;
}

}
