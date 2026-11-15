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
    return stats;
}

}
