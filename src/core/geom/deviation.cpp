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
#include <AutoRemesher/Deviation>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <unordered_map>
#include <vector>

namespace AutoRemesher {

namespace {

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
        size_t operator()(const CellKey& key) const
        {
            return ((size_t)(key.x * 73856093) ^ (size_t)(key.y * 19349663) ^ (size_t)(key.z * 83492791));
        }
    };

    double closestPointOnTriangle(const Vector3& point,
        const Vector3& a, const Vector3& b, const Vector3& c)
    {
        const Vector3 ab = b - a;
        const Vector3 ac = c - a;
        const Vector3 ap = point - a;
        const double d1 = Vector3::dotProduct(ab, ap);
        const double d2 = Vector3::dotProduct(ac, ap);
        if (d1 <= 0.0 && d2 <= 0.0)
            return (point - a).lengthSquared();
        const Vector3 bp = point - b;
        const double d3 = Vector3::dotProduct(ab, bp);
        const double d4 = Vector3::dotProduct(ac, bp);
        if (d3 >= 0.0 && d4 <= d3)
            return (point - b).lengthSquared();
        const double vc = d1 * d4 - d3 * d2;
        if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
            const double denom = d1 - d3;
            const double v = (denom != 0.0) ? d1 / denom : 0.0;
            return (point - (a + ab * v)).lengthSquared();
        }
        const Vector3 cp = point - c;
        const double d5 = Vector3::dotProduct(ab, cp);
        const double d6 = Vector3::dotProduct(ac, cp);
        if (d6 >= 0.0 && d5 <= d6)
            return (point - c).lengthSquared();
        const double vb = d5 * d2 - d1 * d6;
        if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
            const double denom = d2 - d6;
            const double w = (denom != 0.0) ? d2 / denom : 0.0;
            return (point - (a + ac * w)).lengthSquared();
        }
        const double va = d3 * d6 - d5 * d4;
        if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
            const double denom = (d4 - d3) + (d5 - d6);
            const double w = (denom != 0.0) ? (d4 - d3) / denom : 0.0;
            return (point - (b + (c - b) * w)).lengthSquared();
        }
        const double denom = va + vb + vc;
        if (!(std::fabs(denom) > 0.0))
            return (point - a).lengthSquared();
        const double v = vb / denom;
        const double w = vc / denom;
        return (point - (a + ab * v + ac * w)).lengthSquared();
    }

}

DeviationStats measureDeviation(const std::vector<Vector3>& points,
    const std::vector<Vector3>& surfaceVertices,
    const std::vector<std::vector<size_t>>& surfaceFaces,
    double cellScale)
{
    DeviationStats stats;
    if (points.empty() || surfaceFaces.empty() || surfaceVertices.empty())
        return stats;

    // Cell size from the surface's own edges, so the search means the same thing
    // on a millimetre-scale shoe and a metre-scale character.
    double totalEdge = 0.0;
    size_t edgeCount = 0;
    for (const auto& face : surfaceFaces) {
        if (face.size() < 3)
            continue;
        for (size_t i = 0; i < face.size(); ++i) {
            const size_t j = (i + 1) % face.size();
            if (face[i] >= surfaceVertices.size() || face[j] >= surfaceVertices.size())
                continue;
            totalEdge += (surfaceVertices[face[i]] - surfaceVertices[face[j]]).length();
            ++edgeCount;
        }
    }
    if (0 == edgeCount)
        return stats;
    const double cellSize = std::max(1e-12, cellScale * totalEdge / (double)edgeCount);

    const auto cellOf = [cellSize](const Vector3& point, CellKey* key) {
        key->x = (int)std::floor(point.x() / cellSize);
        key->y = (int)std::floor(point.y() / cellSize);
        key->z = (int)std::floor(point.z() / cellSize);
    };

    std::unordered_map<CellKey, std::vector<size_t>, CellKeyHash> grid;
    grid.reserve(surfaceFaces.size() * 2);
    for (size_t faceIndex = 0; faceIndex < surfaceFaces.size(); ++faceIndex) {
        const auto& face = surfaceFaces[faceIndex];
        if (face.size() < 3)
            continue;
        Vector3 low = surfaceVertices[face[0]];
        Vector3 high = low;
        bool usable = true;
        for (const size_t vertex : face) {
            if (vertex >= surfaceVertices.size()) {
                usable = false;
                break;
            }
            const Vector3& position = surfaceVertices[vertex];
            for (size_t axis = 0; axis < 3; ++axis) {
                low[axis] = std::min(low[axis], position[axis]);
                high[axis] = std::max(high[axis], position[axis]);
            }
        }
        if (!usable)
            continue;
        CellKey lowCell;
        CellKey highCell;
        cellOf(low, &lowCell);
        cellOf(high, &highCell);
        for (int x = lowCell.x; x <= highCell.x; ++x) {
            for (int y = lowCell.y; y <= highCell.y; ++y) {
                for (int z = lowCell.z; z <= highCell.z; ++z) {
                    CellKey key;
                    key.x = x;
                    key.y = y;
                    key.z = z;
                    grid[key].push_back(faceIndex);
                }
            }
        }
    }

    std::vector<double> distances;
    distances.reserve(points.size());
    const int maxRing = 8;
    for (const auto& point : points) {
        CellKey center;
        cellOf(point, &center);
        double bestSquared = std::numeric_limits<double>::max();
        int bestRing = maxRing + 1;
        bool found = false;
        for (int ring = 0; ring <= maxRing && ring <= bestRing; ++ring) {
            for (int x = center.x - ring; x <= center.x + ring; ++x) {
                for (int y = center.y - ring; y <= center.y + ring; ++y) {
                    for (int z = center.z - ring; z <= center.z + ring; ++z) {
                        if (ring > 0) {
                            const bool onShell = std::abs(x - center.x) == ring
                                || std::abs(y - center.y) == ring
                                || std::abs(z - center.z) == ring;
                            if (!onShell)
                                continue;
                        }
                        CellKey key;
                        key.x = x;
                        key.y = y;
                        key.z = z;
                        const auto cell = grid.find(key);
                        if (cell == grid.end())
                            continue;
                        for (const size_t faceIndex : cell->second) {
                            const auto& face = surfaceFaces[faceIndex];
                            if (face.size() < 3)
                                continue;
                            const double squared = closestPointOnTriangle(point,
                                surfaceVertices[face[0]], surfaceVertices[face[1]], surfaceVertices[face[2]]);
                            if (squared < bestSquared) {
                                bestSquared = squared;
                                found = true;
                                bestRing = std::max(std::abs(x - center.x),
                                    std::max(std::abs(y - center.y), std::abs(z - center.z)));
                            }
                        }
                    }
                }
            }
        }
        if (!found) {
            ++stats.unresolved;
            continue;
        }
        distances.push_back(std::sqrt(bestSquared));
    }

    stats.samples = distances.size();
    if (distances.empty())
        return stats;
    std::sort(distances.begin(), distances.end());
    const auto at = [&distances](double fraction) {
        const size_t index = (size_t)(fraction * (double)(distances.size() - 1));
        return distances[index];
    };
    double total = 0.0;
    for (const double distance : distances)
        total += distance;
    stats.mean = total / (double)distances.size();
    stats.p50 = at(0.50);
    stats.p95 = at(0.95);
    stats.max = distances.back();
    return stats;
}

}
