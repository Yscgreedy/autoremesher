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
#include <AutoRemesher/TopologyStats>
#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace AutoRemesher {

TopologyStats analyzeTopology(const std::vector<Vector3>& vertices,
    const std::vector<std::vector<size_t>>& faces)
{
    TopologyStats stats;
    stats.vertices = vertices.size();
    stats.faces = faces.size();
    if (faces.empty())
        return stats;

    // An edge is keyed by its ordered endpoints, so both half edges of a
    // manifold edge land on the same key and the face count per key is the
    // multiplicity: 1 = boundary, 2 = manifold interior, >2 = non-manifold.
    typedef std::pair<size_t, size_t> EdgeKey;
    struct EdgeKeyHash {
        size_t operator()(const EdgeKey& key) const
        {
            const size_t first = std::hash<size_t>()(key.first);
            const size_t second = std::hash<size_t>()(key.second);
            return first ^ (second + 0x9e3779b9ULL + (first << 6) + (first >> 2));
        }
    };
    const auto edgeKey = [](size_t from, size_t to) -> EdgeKey {
        return (from < to) ? EdgeKey(from, to) : EdgeKey(to, from);
    };

    std::unordered_map<EdgeKey, size_t, EdgeKeyHash> edgeFaceCount;
    std::unordered_map<EdgeKey, size_t, EdgeKeyHash> edgeFirstFace;
    edgeFaceCount.reserve(faces.size() * 3);

    std::vector<size_t> parent(faces.size());
    std::vector<size_t> rank(faces.size(), 0);
    std::iota(parent.begin(), parent.end(), 0);
    std::function<size_t(size_t)> findRoot = [&parent](size_t index) -> size_t {
        while (parent[index] != index) {
            parent[index] = parent[parent[index]];
            index = parent[index];
        }
        return index;
    };
    const auto unionFaces = [&](size_t first, size_t second) {
        size_t a = findRoot(first);
        size_t b = findRoot(second);
        if (a == b)
            return;
        if (rank[a] < rank[b])
            std::swap(a, b);
        parent[b] = a;
        if (rank[a] == rank[b])
            ++rank[a];
    };

    // Directed boundary edges: from -> to, kept so the loops can be walked.
    std::unordered_map<size_t, size_t> boundaryNext;

    for (size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
        const std::vector<size_t>& face = faces[faceIndex];
        if (face.size() < 3) {
            ++stats.degenerateFaces;
            continue;
        }
        if (4 == face.size())
            ++stats.quads;
        else
            ++stats.nonQuads;
        stats.triangles += face.size() - 2;

        bool degenerate = false;
        for (size_t i = 0; i < face.size() && !degenerate; ++i) {
            for (size_t j = i + 1; j < face.size(); ++j) {
                if (face[i] == face[j]) {
                    degenerate = true;
                    break;
                }
            }
        }
        if (!degenerate && face.size() >= 3) {
            double area = 0.0;
            for (size_t i = 2; i < face.size(); ++i) {
                if (face[i - 1] >= vertices.size() || face[i] >= vertices.size()
                    || face[0] >= vertices.size()) {
                    degenerate = true;
                    break;
                }
                area += Vector3::area(vertices[face[0]], vertices[face[i - 1]], vertices[face[i]]);
            }
            if (!(area > 0.0))
                degenerate = true;
        }
        if (degenerate)
            ++stats.degenerateFaces;

        for (size_t i = 0; i < face.size(); ++i) {
            const size_t from = face[i];
            const size_t to = face[(i + 1) % face.size()];
            if (from == to || from >= vertices.size() || to >= vertices.size())
                continue;
            const EdgeKey key = edgeKey(from, to);
            auto insertResult = edgeFaceCount.insert(std::make_pair(key, (size_t)1));
            if (insertResult.second) {
                edgeFirstFace[key] = faceIndex;
            } else {
                ++insertResult.first->second;
                const auto firstFace = edgeFirstFace.find(key);
                if (firstFace != edgeFirstFace.end())
                    unionFaces(faceIndex, firstFace->second);
            }
        }
    }

    for (const auto& it : edgeFaceCount) {
        if (1 == it.second) {
            ++stats.boundaryEdges;
            // The direction is recovered below from the faces that own the edge;
            // walking needs it, counting does not.
        } else if (it.second > 2) {
            ++stats.nonManifoldEdges;
        }
    }

    // Second pass for the direction of every boundary edge: a boundary edge is
    // owned by exactly one face, so its half edge there is the direction the
    // loop walks in.
    for (const std::vector<size_t>& face : faces) {
        if (face.size() < 3)
            continue;
        for (size_t i = 0; i < face.size(); ++i) {
            const size_t from = face[i];
            const size_t to = face[(i + 1) % face.size()];
            if (from == to || from >= vertices.size() || to >= vertices.size())
                continue;
            const auto count = edgeFaceCount.find(edgeKey(from, to));
            if (count == edgeFaceCount.end() || 1 != count->second)
                continue;
            boundaryNext[from] = to;
        }
    }

    // Every boundary vertex has at most one outgoing boundary edge on a clean
    // shell, so a walk that returns to its start is one loop.  A pinch point
    // (several outgoing edges) is not a loop and is deliberately not counted.
    std::unordered_set<size_t> walked;
    for (const auto& it : boundaryNext) {
        if (walked.end() != walked.find(it.first))
            continue;
        std::unordered_set<size_t> local;
        size_t current = it.first;
        while (boundaryNext.end() != boundaryNext.find(current)
            && local.end() == local.find(current)) {
            local.insert(current);
            current = boundaryNext[current];
        }
        for (const size_t vertex : local)
            walked.insert(vertex);
        if (current == it.first)
            ++stats.boundaryLoops;
    }

    std::unordered_set<size_t> roots;
    for (size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
        if (faces[faceIndex].size() < 3)
            continue;
        roots.insert(findRoot(faceIndex));
    }
    stats.components = roots.size();
    // Filled last so that both derived counters see the final boundary and
    // non-manifold counts no matter how the passes above are reordered later.
    stats.edgesWithMoreThanTwoFaces = stats.nonManifoldEdges;
    stats.nonManifoldEdgesBlender = stats.boundaryEdges + stats.edgesWithMoreThanTwoFaces;
    return stats;
}

namespace {

    typedef std::pair<size_t, size_t> QualityEdgeKey;

    struct QualityEdgeKeyHash {
        size_t operator()(const QualityEdgeKey& key) const
        {
            const size_t first = std::hash<size_t>()(key.first);
            const size_t second = std::hash<size_t>()(key.second);
            return first ^ (second + 0x9e3779b9ULL + (first << 6) + (first >> 2));
        }
    };

    // Order statistic, not an interpolation: (size_t)(p * n) clipped to the last
    // element.  A consumer comparing these against another tool's percentiles can
    // point at a value that exists in the mesh; an interpolated one exists only
    // inside the report and is a fresh argument every time the two tools differ.
    double qualityQuantile(const std::vector<double>& sortedValues, double fraction)
    {
        if (sortedValues.empty())
            return 0.0;
        size_t index = (size_t)(fraction * (double)sortedValues.size());
        if (index >= sortedValues.size())
            index = sortedValues.size() - 1;
        return sortedValues[index];
    }

    QualityRange qualityRange(std::vector<double>& values)
    {
        QualityRange range;
        if (values.empty())
            return range;
        std::sort(values.begin(), values.end());
        range.min = values.front();
        range.p50 = qualityQuantile(values, 0.5);
        range.p95 = qualityQuantile(values, 0.95);
        range.max = values.back();
        return range;
    }

}

QualityStats collectQuality(const std::vector<Vector3>& vertices,
    const std::vector<std::vector<size_t>>& faces, double bboxDiagonal)
{
    QualityStats quality;

    // Thresholds are relative to the model with an absolute floor: a mesh scaled
    // to nothing, or a caller that has no bounding box yet, still needs an
    // epsilon that is not zero, and a fixed absolute epsilon would call every edge
    // of a 2 m model degenerate the moment the same mesh is exported in mm.
    const double diagonal = (bboxDiagonal > 0.0) ? bboxDiagonal : 0.0;
    const double zeroLengthThreshold = std::max(1e-12, 1e-6 * diagonal);
    const double zeroAreaThreshold = std::max(1e-18, 1e-14 * diagonal * diagonal);

    // One table serves both the topology and the geometry: an edge's multiplicity
    // says whether it is a boundary edge, and its two endpoints say how long it is
    // and which vertices it gives valence to.  Building it once keeps the function
    // at one face pass plus one table pass, which is what makes 20k faces fit in a
    // frame's worth of time without reaching for TBB.
    std::unordered_map<QualityEdgeKey, size_t, QualityEdgeKeyHash> edgeFaceCount;
    edgeFaceCount.reserve(faces.size() * 3);
    std::vector<double> quadAspects;
    quadAspects.reserve(faces.size() / 2);

    for (size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
        const std::vector<size_t>& face = faces[faceIndex];
        if (face.size() < 3) {
            // Fewer than three corners bounds no area, and analyzeTopology already
            // calls that degenerate; both reports then agree on the same file.
            ++quality.degenerateFaces;
            continue;
        }

        bool indicesValid = true;
        for (size_t i = 0; i < face.size(); ++i) {
            if (face[i] >= vertices.size()) {
                indicesValid = false;
                break;
            }
        }

        bool degenerate = false;
        for (size_t i = 0; i < face.size() && !degenerate; ++i) {
            for (size_t j = i + 1; j < face.size(); ++j) {
                if (face[i] == face[j]) {
                    degenerate = true;
                    break;
                }
            }
        }

        double area = 0.0;
        if (indicesValid) {
            // The same fan decomposition analyzeTopology uses, so the two
            // functions cannot disagree on whether a given quad has area.
            for (size_t i = 2; i < face.size(); ++i)
                area += Vector3::area(vertices[face[0]], vertices[face[i - 1]], vertices[face[i]]);
        }
        // A negated comparison, so a NaN area from non-finite input counts as
        // degenerate instead of slipping through as "not small enough".
        if (!(area > zeroAreaThreshold))
            degenerate = true;
        if (degenerate)
            ++quality.degenerateFaces;

        if (4 == face.size() && indicesValid) {
            double shortest = 0.0;
            double longest = 0.0;
            for (size_t i = 0; i < 4; ++i) {
                const double length = (vertices[face[(i + 1) % 4]] - vertices[face[i]]).length();
                if (0 == i || length < shortest)
                    shortest = length;
                if (0 == i || length > longest)
                    longest = length;
            }
            // A zero shortest side has no finite ratio: leaving it in would put an
            // infinity into a list that is then sorted and indexed, so one collapsed
            // quad would drag p95 to inf on an otherwise healthy mesh.  Such a quad
            // is still visible - as a zero length edge, and as a zero area face when
            // the collapse takes the whole area with it - and both aspect numbers
            // below describe the same set of quads so they cannot be read as two
            // different populations.
            if (shortest > 0.0 && std::isfinite(longest)) {
                quadAspects.push_back(longest / shortest);
                if (area > 0.0 && std::isfinite(area)) {
                    const double areaAspect = longest * longest / (4.0 * area);
                    if (areaAspect > quality.quadAspectAreaMax)
                        quality.quadAspectAreaMax = areaAspect;
                }
            }
        }

        for (size_t i = 0; i < face.size(); ++i) {
            const size_t from = face[i];
            const size_t to = face[(i + 1) % face.size()];
            // A repeated index makes a self loop with no direction and no length; it
            // is skipped here exactly as analyzeTopology skips it, so the edge counts
            // of the two functions stay comparable on the same file.
            if (from == to || from >= vertices.size() || to >= vertices.size())
                continue;
            const QualityEdgeKey key = (from < to) ? QualityEdgeKey(from, to) : QualityEdgeKey(to, from);
            auto insertResult = edgeFaceCount.insert(std::make_pair(key, (size_t)1));
            if (!insertResult.second)
                ++insertResult.first->second;
        }
    }

    std::vector<size_t> valence(vertices.size(), 0);
    std::vector<bool> onBoundaryEdge(vertices.size(), false);
    std::vector<double> edgeLengths;
    edgeLengths.reserve(edgeFaceCount.size());

    for (const auto& it : edgeFaceCount) {
        const size_t from = it.first.first;
        const size_t to = it.first.second;
        // valence counts distinct edges, not incident corners: a quad touching the
        // same edge twice is a broken face, not a higher valence.
        ++valence[from];
        ++valence[to];
        if (1 == it.second) {
            onBoundaryEdge[from] = true;
            onBoundaryEdge[to] = true;
        }
        const double length = (vertices[to] - vertices[from]).length();
        // Filtered before it reaches the sorter: sorting a list containing NaN is
        // undefined behaviour, not merely a bad maximum.
        if (std::isfinite(length)) {
            edgeLengths.push_back(length);
            if (length <= zeroLengthThreshold)
                ++quality.zeroLengthEdges;
        }
    }

    for (size_t vertexIndex = 0; vertexIndex < valence.size(); ++vertexIndex) {
        const size_t vertexValence = valence[vertexIndex];
        if (vertexValence > quality.valenceMax)
            quality.valenceMax = vertexValence;
        // valence 0 means no face touches the vertex, so it is not on the surface
        // at all; counting it would invent a pole for a stray point.
        if (0 == vertexValence || onBoundaryEdge[vertexIndex])
            continue;
        ++quality.interiorVertices;
        if (3 == vertexValence)
            ++quality.valence3;
        else if (4 == vertexValence)
            ++quality.valence4;
        else if (5 == vertexValence)
            ++quality.valence5;
        else if (vertexValence > 5)
            ++quality.valenceGt5;
        if (4 != vertexValence)
            ++quality.poleCount;
    }

    if (quality.interiorVertices > 0)
        quality.polesPct = 100.0 * (double)quality.poleCount / (double)quality.interiorVertices;

    quality.edgeLength = qualityRange(edgeLengths);
    quality.quadAspect = qualityRange(quadAspects);
    return quality;
}

}
