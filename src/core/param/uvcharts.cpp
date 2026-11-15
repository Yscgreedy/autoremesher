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
#include <limits>
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

namespace {

    // Union-find over face indices; the UV packing only needs "same chart or
    // not", and a parent array is both faster and easier to make deterministic
    // than a hash of chart members.
    size_t findChartRoot(std::vector<size_t>& parent, size_t index)
    {
        size_t root = index;
        while (parent[root] != root)
            root = parent[root];
        while (parent[index] != root) {
            const size_t next = parent[index];
            parent[index] = root;
            index = next;
        }
        return root;
    }

    void uniteChart(std::vector<size_t>& parent, size_t first, size_t second)
    {
        const size_t firstRoot = findChartRoot(parent, first);
        const size_t secondRoot = findChartRoot(parent, second);
        if (firstRoot == secondRoot)
            return;
        // Smaller index wins, so the representative does not depend on the order
        // the edges happened to be visited in.
        if (firstRoot < secondRoot)
            parent[secondRoot] = firstRoot;
        else
            parent[firstRoot] = secondRoot;
    }

} // namespace

UvPackResult packUvCharts(const std::vector<std::vector<size_t>>& faces,
    std::vector<std::vector<Vector2>>* faceUvs,
    std::vector<Vector2>* vertexUvs,
    double margin)
{
    UvPackResult result;
    if (nullptr == faceUvs || faceUvs->size() != faces.size() || faces.empty())
        return result;

    // The two channels are compared on shared edges, so the tolerance has to be
    // relative to the layout: a cover unit is one quad, and 1e-4 of the whole
    // layout is far below a corner and far above the last-bit noise of two
    // triangles that computed the same intersection point.
    double minU = 0.0;
    double maxU = 0.0;
    double minV = 0.0;
    double maxV = 0.0;
    bool haveUv = false;
    for (const auto& cornerUvs : *faceUvs) {
        for (const auto& uv : cornerUvs) {
            if (!std::isfinite(uv.x()) || !std::isfinite(uv.y()))
                return result;
            minU = haveUv ? std::min(minU, uv.x()) : uv.x();
            maxU = haveUv ? std::max(maxU, uv.x()) : uv.x();
            minV = haveUv ? std::min(minV, uv.y()) : uv.y();
            maxV = haveUv ? std::max(maxV, uv.y()) : uv.y();
            haveUv = true;
        }
    }
    if (!haveUv)
        return result;
    const double diagonal = Vector2(maxU - minU, maxV - minV).length();
    const double tolerance = std::max(1e-9, 1e-4 * (diagonal > 0.0 ? diagonal : 1.0));

    // One entry per undirected edge: which face owns it and at which corner, so a
    // shared edge can be compared without a second pass over both faces.
    std::map<std::pair<size_t, size_t>, std::vector<std::pair<size_t, size_t>>> edgeOwners;
    for (size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
        const auto& face = faces[faceIndex];
        if (face.size() < 3 || (*faceUvs)[faceIndex].size() != face.size())
            continue;
        for (size_t corner = 0; corner < face.size(); ++corner) {
            const size_t first = face[corner];
            const size_t second = face[(corner + 1) % face.size()];
            const auto edge = first < second ? std::make_pair(first, second)
                                             : std::make_pair(second, first);
            edgeOwners[edge].push_back(std::make_pair(faceIndex, corner));
        }
    }

    // Faces are grouped into charts through the UV translation that carries one
    // face's UVs onto its neighbour's.  Corner UVs are per-triangle affine
    // extrapolations of the cover, so neighbours agree only up to a whole cover
    // cell: the translation is an integer vector.  Comparing the UVs directly,
    // without allowing that offset, finds one chart per face - which packs without
    // overlap but leaves a bake with a seam line on every edge.
    // How close two faces have to place their shared edge to count as one chart.
    // A quad's UV edge is one cover cell, so a tenth of a cell is well inside
    // "same chart" and well outside the difference between two charts; the cell is
    // estimated from the median shortest UV edge because the cover scale is not
    // known at this point and a fixed epsilon is wrong in every other unit.
    std::vector<double> shortestEdges;
    shortestEdges.reserve(faces.size());
    for (size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
        const auto& face = faces[faceIndex];
        const auto& cornerUvs = (*faceUvs)[faceIndex];
        if (face.size() < 3 || cornerUvs.size() != face.size())
            continue;
        double shortest = std::numeric_limits<double>::max();
        for (size_t corner = 0; corner < face.size(); ++corner)
            shortest = std::min(shortest, (cornerUvs[(corner + 1) % face.size()] - cornerUvs[corner]).length());
        if (shortest > 0.0 && std::isfinite(shortest))
            shortestEdges.push_back(shortest);
    }
    double cellSize = 0.0;
    if (!shortestEdges.empty()) {
        std::sort(shortestEdges.begin(), shortestEdges.end());
        cellSize = shortestEdges[shortestEdges.size() / 2];
    }
    const double chartTolerance = cellSize > 0.0 ? 0.15 * cellSize : tolerance;
    std::vector<std::vector<std::pair<size_t, Vector2>>> neighbours(faces.size());
    for (const auto& it : edgeOwners) {
        if (it.second.size() != 2)
            continue;
        const size_t firstFace = it.second[0].first;
        const size_t firstCorner = it.second[0].second;
        const size_t secondFace = it.second[1].first;
        const size_t secondCorner = it.second[1].second;
        const auto& firstFaceRef = faces[firstFace];
        const auto& secondFaceRef = faces[secondFace];
        const auto& firstUvs = (*faceUvs)[firstFace];
        const auto& secondUvs = (*faceUvs)[secondFace];
        if (firstUvs.size() != firstFaceRef.size() || secondUvs.size() != secondFaceRef.size())
            continue;
        // The two faces walk the shared edge in opposite directions.
        const size_t firstNext = (firstCorner + 1) % firstFaceRef.size();
        const size_t secondNext = (secondCorner + 1) % secondFaceRef.size();
        const Vector2 forward = secondUvs[secondNext] - firstUvs[firstCorner];
        const Vector2 backward = secondUvs[secondCorner] - firstUvs[firstNext];
        const Vector2 delta((forward.x() + backward.x()) * 0.5,
            (forward.y() + backward.y()) * 0.5);
        if (delta.length() > chartTolerance)
            continue;
        neighbours[firstFace].push_back(std::make_pair(secondFace, delta));
        neighbours[secondFace].push_back(std::make_pair(firstFace, Vector2(-delta.x(), -delta.y())));
    }

    // Breadth-first over the adjacency, accumulating the offset that brings each
    // face into its chart's frame.  Propagation from the chart's seed face keeps a
    // chart's internal cell structure intact instead of flattening it.
    std::vector<size_t> chartOfFace(faces.size(), (size_t)-1);
    std::vector<Vector2> offsetOfFace(faces.size());
    size_t chartCount = 0;
    std::vector<size_t> pending;
    for (size_t seed = 0; seed < faces.size(); ++seed) {
        if (chartOfFace[seed] != (size_t)-1)
            continue;
        chartOfFace[seed] = chartCount;
        pending.clear();
        pending.push_back(seed);
        while (!pending.empty()) {
            const size_t current = pending.back();
            pending.pop_back();
            for (const auto& neighbour : neighbours[current]) {
                if (chartOfFace[neighbour.first] != (size_t)-1)
                    continue;
                chartOfFace[neighbour.first] = chartCount;
                offsetOfFace[neighbour.first] = Vector2(
                    offsetOfFace[current].x() + neighbour.second.x(),
                    offsetOfFace[current].y() + neighbour.second.y());
                pending.push_back(neighbour.first);
            }
        }
        ++chartCount;
    }
    result.chartCount = chartCount;

    struct ChartBox {
        double minU = 0.0;
        double maxU = 0.0;
        double minV = 0.0;
        double maxV = 0.0;
        bool valid = false;
        double width() const { return maxU - minU; }
        double height() const { return maxV - minV; }
    };
    std::vector<ChartBox> boxes(chartCount);
    for (size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
        ChartBox& box = boxes[chartOfFace[faceIndex]];
        const auto& cornerUvs = (*faceUvs)[faceIndex];
        const Vector2 offset = offsetOfFace[faceIndex];
        for (const auto& uv : cornerUvs) {
            const double u = uv.x() - offset.x();
            const double v = uv.y() - offset.y();
            if (!box.valid) {
                box.minU = box.maxU = u;
                box.minV = box.maxV = v;
                box.valid = true;
                continue;
            }
            box.minU = std::min(box.minU, u);
            box.maxU = std::max(box.maxU, u);
            box.minV = std::min(box.minV, v);
            box.maxV = std::max(box.maxV, v);
        }
    }

    // Shelf packing by decreasing height.  The bin width is the square root of the
    // total box area inflated a little, so a layout of equal strips fills one row
    // and a layout of one tall chart still opens a row for it.
    std::vector<size_t> order;
    order.reserve(boxes.size());
    double totalArea = 0.0;
    double widest = 0.0;
    double tallest = 0.0;
    for (size_t i = 0; i < boxes.size(); ++i) {
        if (!boxes[i].valid)
            continue;
        order.push_back(i);
        totalArea += boxes[i].width() * boxes[i].height();
        widest = std::max(widest, boxes[i].width());
        tallest = std::max(tallest, boxes[i].height());
    }
    if (order.empty())
        return result;
    std::sort(order.begin(), order.end(), [&boxes](size_t first, size_t second) {
        const double firstHeight = boxes[first].height();
        const double secondHeight = boxes[second].height();
        if (firstHeight != secondHeight)
            return firstHeight > secondHeight;
        return first < second;
    });
    const double gap = std::max(1e-9, margin);
    const double packedArea = totalArea > 0.0 ? totalArea : 1.0;
    double binWidth = std::max(widest, std::sqrt(packedArea) * 1.15);
    binWidth = std::max(binWidth, 1e-9);

    std::vector<Vector2> chartOffset(boxes.size());
    std::vector<char> placed(boxes.size(), 0);
    double cursorX = 0.0;
    double cursorY = 0.0;
    double shelfHeight = 0.0;
    double usedWidth = 0.0;
    for (const size_t chart : order) {
        const ChartBox& box = boxes[chart];
        const double width = box.width() + gap;
        const double height = box.height() + gap;
        if (cursorX > 0.0 && cursorX + width > binWidth) {
            cursorY += shelfHeight;
            cursorX = 0.0;
            shelfHeight = 0.0;
        }
        chartOffset[chart] = Vector2(cursorX, cursorY);
        placed[chart] = 1;
        cursorX += width;
        usedWidth = std::max(usedWidth, cursorX);
        shelfHeight = std::max(shelfHeight, height);
    }
    const double usedHeight = cursorY + shelfHeight;
    const double extent = std::max(std::max(usedWidth, usedHeight), 1e-9);
    const double usable = std::max(1e-6, 1.0 - 2.0 * gap);
    const double scale = usable / extent;

    // Apply: every corner of a chart moves by the same scale and offset, which is
    // what keeps the chart's internal shape (and its seam agreements) intact.
    for (size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
        const size_t chart = chartOfFace[faceIndex];
        const ChartBox& box = boxes[chart];
        const Vector2 offset(gap + (chartOffset[chart].x() - box.minU) * scale,
            gap + (chartOffset[chart].y() - box.minV) * scale);
        auto& cornerUvs = (*faceUvs)[faceIndex];
        const Vector2 faceOffset = offsetOfFace[faceIndex];
        for (auto& uv : cornerUvs) {
            const double u = uv.x() - faceOffset.x();
            const double v = uv.y() - faceOffset.y();
            uv = Vector2(u * scale + offset.x(), v * scale + offset.y());
        }
    }

    // The per-vertex channel can only follow where a vertex sits inside one chart;
    // a vertex shared by two charts has no single correct packed UV, and guessing
    // one would put a fold in the seam.
    if (nullptr != vertexUvs && !vertexUvs->empty()) {
        std::vector<Vector2> accumulated(vertexUvs->size());
        std::vector<size_t> references(vertexUvs->size(), 0);
        for (size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
            const auto& face = faces[faceIndex];
            const auto& cornerUvs = (*faceUvs)[faceIndex];
            if (cornerUvs.size() != face.size())
                continue;
            for (size_t corner = 0; corner < face.size(); ++corner) {
                const size_t vertex = face[corner];
                if (vertex >= vertexUvs->size())
                    continue;
                if (0 == references[vertex])
                    accumulated[vertex] = cornerUvs[corner];
                references[vertex] = 1;
            }
        }
        // First corner wins, never an average: averaging across two charts puts the
        // vertex *between* them, and a quad spanning two chart boxes becomes a
        // hundred times too large in UV space.  The per-vertex channel is only
        // well defined when the mesh is cut at the chart seams (--uv-mode seam),
        // which the caller turns on together with packing for exactly this reason.
        for (size_t vertex = 0; vertex < vertexUvs->size(); ++vertex) {
            if (references[vertex] > 0)
                (*vertexUvs)[vertex] = accumulated[vertex];
        }
    }

    result.packedCharts = order.size();
    result.scale = scale;
    result.fillRatio = (usedWidth * usedHeight) > 0.0 ? totalArea / (usedWidth * usedHeight) : 0.0;
    result.ok = order.size() == boxes.size();
    return result;
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
