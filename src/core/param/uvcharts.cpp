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

namespace {

    // Cells per side of the cover square.  512 is what the independent
    // measurement of the exported OBJ uses, so the two can be compared directly;
    // coarser would under-report thin overlaps, finer costs time for texels no
    // bake resolution would ever ask for.
    const size_t UvCoverGridSize = 512;

    // Below this the fan triangle has no area left in double precision.  Such
    // triangles are dropped instead of rasterised: a folded quad paints a line
    // of cells that says nothing about overlap, and the count would depend on
    // which side of the fold the extra cell landed on.
    const double UvCoverMinDet = 1e-18;

    bool isFinite(const Vector2& uv)
    {
        return std::isfinite(uv.x()) && std::isfinite(uv.y());
    }

    // Rasterises one UV triangle into the coverage grid by testing cell centres.
    //
    // The edge functions are evaluated in cell units, scaled by the sign of the
    // determinant: that turns "inside" into "all three >= 0" for both winding
    // orders with no division per cell, and keeping them in cell units means the
    // per-cell work is one multiply-add per edge instead of two divisions.  The
    // terms are rebuilt from the cell index on every row rather than accumulated
    // across columns, because 512 accumulated steps of 1/512 drift by ~1e-13 and
    // would flip the cells whose centre sits on a shared edge.
    void rasterizeUvTriangle(std::vector<unsigned char>& cells,
        size_t& coveredCells, size_t& overlappedCells,
        const Vector2& a, const Vector2& b, const Vector2& c, double det)
    {
        const double grid = (double)UvCoverGridSize;
        const double sign = det > 0.0 ? 1.0 : -1.0;
        const double edge0X = sign * (b.y() - c.y());
        const double edge0Y = sign * (c.x() - b.x());
        const double edge1X = sign * (c.y() - a.y());
        const double edge1Y = sign * (a.x() - c.x());
        const double edge2 = std::fabs(det) * grid;
        const double anchorX = c.x() * grid;
        const double anchorY = c.y() * grid;

        // Clipped, not clamped: only the part of the triangle that is inside the
        // cover is measured.  The clamp to the grid range happens on the double
        // value before the cast, so a model with UVs in the thousands cannot
        // overflow the integer range (and a wildly out-of-range triangle simply
        // covers nothing).
        const double lowU = std::min(a.x(), std::min(b.x(), c.x())) * grid;
        const double highU = std::max(a.x(), std::max(b.x(), c.x())) * grid;
        const double lowV = std::min(a.y(), std::min(b.y(), c.y())) * grid;
        const double highV = std::max(a.y(), std::max(b.y(), c.y())) * grid;
        const double firstX = std::max(0.0, std::min(grid - 1.0, std::floor(lowU)));
        const double lastX = std::max(0.0, std::min(grid - 1.0, std::floor(highU)));
        const double firstY = std::max(0.0, std::min(grid - 1.0, std::floor(lowV)));
        const double lastY = std::max(0.0, std::min(grid - 1.0, std::floor(highV)));
        if (!(firstX <= lastX) || !(firstY <= lastY))
            return;
        const size_t firstColumn = (size_t)firstX;
        const size_t lastColumn = (size_t)lastX;
        const size_t firstRow = (size_t)firstY;
        const size_t lastRow = (size_t)lastY;

        for (size_t row = firstRow; row <= lastRow; ++row) {
            const double offsetY = ((double)row + 0.5) - anchorY;
            const double rowBase0 = edge0X * (0.5 - anchorX) + edge0Y * offsetY;
            const double rowBase1 = edge1X * (0.5 - anchorX) + edge1Y * offsetY;
            unsigned char* cell = &cells[row * UvCoverGridSize];
            for (size_t column = firstColumn; column <= lastColumn; ++column) {
                const double edge0 = rowBase0 + edge0X * (double)column;
                const double edge1 = rowBase1 + edge1X * (double)column;
                if (edge0 < 0.0 || edge1 < 0.0 || (edge2 - edge0 - edge1) < 0.0)
                    continue;
                if (0 == cell[column]) {
                    cell[column] = 1;
                    ++coveredCells;
                } else if (1 == cell[column]) {
                    cell[column] = 2;
                    ++overlappedCells;
                }
            }
        }
    }

}

UvStats collectUvStats(const std::vector<Vector3>& vertices,
    const std::vector<std::vector<size_t>>& faces,
    const std::vector<std::vector<Vector2>>& faceUvs,
    const std::vector<int>& faceCharts)
{
    UvStats stats;

    // The solver wrote the chart ids down itself; sorting instead of a set keeps
    // the count independent of any container iteration order.
    if (!faceCharts.empty()) {
        std::vector<int> chartIds(faceCharts);
        std::sort(chartIds.begin(), chartIds.end());
        stats.chartCount = (size_t)(std::unique(chartIds.begin(), chartIds.end()) - chartIds.begin());
    }

    std::vector<double> stretches;
    stretches.reserve(faces.size());
    std::vector<unsigned char> cells(UvCoverGridSize * UvCoverGridSize, 0);
    size_t coveredCells = 0;
    size_t overlappedCells = 0;

    for (size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
        const auto& face = faces[faceIndex];
        const size_t cornerCount = face.size();
        if (faceIndex >= faceUvs.size() || faceUvs[faceIndex].size() != cornerCount) {
            // No UV for this face: it is not laid out at all, so it feeds no
            // metric - but the count of what was skipped is part of the answer.
            stats.strayTriangles += cornerCount >= 3 ? cornerCount - 2 : 1;
            continue;
        }
        const auto& uvs = faceUvs[faceIndex];
        for (size_t k = 1; k + 1 < cornerCount; ++k) {
            const Vector2& a = uvs[0];
            const Vector2& b = uvs[k];
            const Vector2& c = uvs[k + 1];
            if (!isFinite(a) || !isFinite(b) || !isFinite(c)) {
                // A NaN UV reaching the grid would poison the whole row it is
                // compared against, so the triangle is dropped before anything
                // else looks at it.
                ++stats.strayTriangles;
                continue;
            }
            const double det = (b.y() - c.y()) * (a.x() - c.x()) + (c.x() - b.x()) * (a.y() - c.y());
            if (!(std::fabs(det) > UvCoverMinDet)) {
                ++stats.strayTriangles;
                continue;
            }
            stats.area += 0.5 * std::fabs(det);
            rasterizeUvTriangle(cells, coveredCells, overlappedCells, a, b, c, det);

            // Stretch is measured on the same fan triangles as the area, so a
            // squeezed corner of a quad is reported the way a bake would find it.
            const size_t first = face[0];
            const size_t second = face[k];
            const size_t third = face[k + 1];
            if (first >= vertices.size() || second >= vertices.size() || third >= vertices.size())
                continue;
            const double uvLongest = std::max(uvDistance(a, b),
                std::max(uvDistance(b, c), uvDistance(c, a)));
            const double worldLongest = std::max((vertices[second] - vertices[first]).length(),
                std::max((vertices[third] - vertices[second]).length(),
                    (vertices[first] - vertices[third]).length()));
            // A 3D-degenerate triangle (welded or duplicate corners) would divide
            // by zero and put an infinity into the maximum, hiding every real
            // stretch behind one broken face; it is left out of the sample
            // instead.  It is not a stray: its UVs are fine.
            if (worldLongest > 0.0)
                stretches.push_back(uvLongest / worldLongest);
        }
    }

    stats.overlapPct = coveredCells > 0
        ? 100.0 * (double)overlappedCells / (double)coveredCells
        : 0.0;

    if (!stretches.empty()) {
        std::sort(stretches.begin(), stretches.end());
        const size_t middle = stretches.size() / 2;
        stats.medianStretch = (stretches.size() % 2) != 0
            ? stretches[middle]
            : 0.5 * (stretches[middle - 1] + stretches[middle]);
        if (stats.medianStretch > 0.0)
            stats.maxStretch = stretches.back() / stats.medianStretch;
    }

    // Exactly the two conditions the field documents: area 0 (nothing laid out)
    // passes, because "is anything unwrapped" is the caller's question, asked
    // through area itself.
    // An empty layout is not bakeable either: "no overlap" over no texels is true
    // and useless, and a caller reading bakeable: true would try to bake nothing.
    stats.bakeable = stats.area > 1e-12 && stats.area <= 1.0 + 1e-6 && stats.overlapPct < 1.0;
    return stats;
}

}
