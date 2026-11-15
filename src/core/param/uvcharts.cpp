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
#include <AutoRemesher/UvCharts>
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <utility>
#include <vector>

namespace AutoRemesher {

namespace {

    struct EdgeCorner {
        size_t face = 0;
        size_t corner = 0;
    };

    double uvDistance(const Vector2& first, const Vector2& second)
    {
        const double du = first.x() - second.x();
        const double dv = first.y() - second.y();
        return std::sqrt(du * du + dv * dv);
    }

}

ChartSplit splitAtChartSeams(const std::vector<Vector3>& vertices,
    const std::vector<std::vector<size_t>>& faces,
    const std::vector<std::vector<Vector2>>& faceUvs,
    const std::vector<int>& faceCharts,
    double uvScale)
{
    ChartSplit result;
    result.vertices = vertices;
    result.faces = faces;
    if (faces.empty() || faceUvs.size() != faces.size())
        return result;

    if (!(uvScale > 0.0)) {
        bool have = false;
        double minU = 0.0;
        double maxU = 0.0;
        double minV = 0.0;
        double maxV = 0.0;
        for (const auto& faceUvsOfFace : faceUvs) {
            for (const auto& uv : faceUvsOfFace) {
                minU = have ? std::min(minU, uv.x()) : uv.x();
                maxU = have ? std::max(maxU, uv.x()) : uv.x();
                minV = have ? std::min(minV, uv.y()) : uv.y();
                maxV = have ? std::max(maxV, uv.y()) : uv.y();
                have = true;
            }
        }
        uvScale = have ? std::max(maxU - minU, maxV - minV) : 1.0;
    }
    if (!(uvScale > 0.0))
        uvScale = 1.0;
    // 1% of the UV extent: an independent per-face evaluation of the same chart
    // disagrees by ~1e-3 (extrapolation error), while a cover seam is a whole
    // cell away (>= 0.3 here), so this separates the two without merging charts.
    const double tolerance = 1e-2 * uvScale;

    // Two faces belong to the same chart when their UVs agree on the edge they
    // share.  Across a cover seam the values differ by a whole cell, which is
    // orders of magnitude above this tolerance.
    std::vector<size_t> parent(faces.size());
    std::iota(parent.begin(), parent.end(), 0);
    std::vector<size_t> rank(faces.size(), 0);
    const auto findRoot = [&parent](size_t index) {
        while (parent[index] != index) {
            parent[index] = parent[parent[index]];
            index = parent[index];
        }
        return index;
    };
    const auto unite = [&](size_t first, size_t second) {
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

    std::map<std::pair<size_t, size_t>, std::vector<EdgeCorner>> edges;
    for (size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
        const auto& face = faces[faceIndex];
        if (faceUvs[faceIndex].size() != face.size())
            continue;
        for (size_t i = 0; i < face.size(); ++i) {
            size_t first = face[i];
            size_t second = face[(i + 1) % face.size()];
            if (first > second)
                std::swap(first, second);
            edges[{ first, second }].push_back({ faceIndex, i });
        }
    }

    const auto uvAt = [&](size_t faceIndex, size_t vertex) -> const Vector2* {
        const auto& face = faces[faceIndex];
        const auto& uvs = faceUvs[faceIndex];
        for (size_t i = 0; i < face.size(); ++i) {
            if (face[i] == vertex)
                return &uvs[i];
        }
        return nullptr;
    };

    for (const auto& edge : edges) {
        if (edge.second.size() != 2)
            continue;
        const size_t firstFace = edge.second[0].face;
        const size_t secondFace = edge.second[1].face;
        const Vector2* firstA = uvAt(firstFace, edge.first.first);
        const Vector2* secondA = uvAt(secondFace, edge.first.first);
        const Vector2* firstB = uvAt(firstFace, edge.first.second);
        const Vector2* secondB = uvAt(secondFace, edge.first.second);
        if (nullptr == firstA || nullptr == secondA || nullptr == firstB || nullptr == secondB)
            continue;
        const double difference = std::max(uvDistance(*firstA, *secondA),
            uvDistance(*firstB, *secondB));
        if (difference <= tolerance)
            unite(firstFace, secondFace);
    }

    std::vector<size_t> chart(faces.size());
    if (faceCharts.size() == faces.size()) {
        // The solver told us where it cut: use its ids as they are.
        for (size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex)
            chart[faceIndex] = (size_t)(unsigned int)faceCharts[faceIndex];
    } else {
        for (size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex)
            chart[faceIndex] = findRoot(faceIndex);
    }
    std::vector<size_t> chartIds(chart);
    std::sort(chartIds.begin(), chartIds.end());
    chartIds.erase(std::unique(chartIds.begin(), chartIds.end()), chartIds.end());
    result.chartCount = chartIds.size();

    // One vertex per (vertex, chart).  The first chart keeps the original index,
    // every other chart gets a copy at the same position with its own UV.
    std::vector<Vector2> vertexUvs(vertices.size());
    std::vector<char> assigned(vertices.size(), 0);
    std::map<std::pair<size_t, size_t>, size_t> copies;
    for (size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
        const auto& face = faces[faceIndex];
        if (faceUvs[faceIndex].size() != face.size())
            continue;
        for (size_t i = 0; i < face.size(); ++i) {
            const size_t vertex = face[i];
            const std::pair<size_t, size_t> key(vertex, chart[faceIndex]);
            if (copies.find(key) != copies.end())
                continue;
            if (!assigned[vertex]) {
                assigned[vertex] = 1;
                vertexUvs[vertex] = faceUvs[faceIndex][i];
                copies[key] = vertex;
                continue;
            }
            const size_t copyIndex = result.vertices.size();
            result.vertices.push_back(vertices[vertex]);
            vertexUvs.push_back(faceUvs[faceIndex][i]);
            copies[key] = copyIndex;
            ++result.splitVertices;
        }
    }

    for (size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
        auto& face = result.faces[faceIndex];
        if (faceUvs[faceIndex].size() != face.size())
            continue;
        for (size_t i = 0; i < face.size(); ++i) {
            const auto found = copies.find({ faces[faceIndex][i], chart[faceIndex] });
            if (found != copies.end())
                face[i] = found->second;
        }
    }

    // Drop the vertices no face references any more (a vertex whose every chart
    // got its own copy) and rebuild the UV array for the compacted numbering.
    std::vector<size_t> remap(result.vertices.size(), (size_t)-1);
    std::vector<Vector3> compactedVertices;
    std::vector<Vector2> compactedUvs;
    compactedVertices.reserve(result.vertices.size());
    compactedUvs.reserve(result.vertices.size());
    for (auto& face : result.faces) {
        for (auto& corner : face) {
            if (remap[corner] == (size_t)-1) {
                remap[corner] = compactedVertices.size();
                compactedVertices.push_back(result.vertices[corner]);
                compactedUvs.push_back(vertexUvs[corner]);
            }
            corner = remap[corner];
        }
    }
    result.vertices.swap(compactedVertices);
    result.vertexUvs.swap(compactedUvs);
    return result;
}

}
