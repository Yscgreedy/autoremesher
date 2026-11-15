#include <AutoRemesher/UvTransfer>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {

double closestPointOnTriangle(const AutoRemesher::Vector3& point,
    const AutoRemesher::Vector3& a,
    const AutoRemesher::Vector3& b,
    const AutoRemesher::Vector3& c,
    double* weightA, double* weightB, double* weightC)
{
    const AutoRemesher::Vector3 ab = b - a;
    const AutoRemesher::Vector3 ac = c - a;
    const AutoRemesher::Vector3 ap = point - a;
    const double d1 = AutoRemesher::Vector3::dotProduct(ab, ap);
    const double d2 = AutoRemesher::Vector3::dotProduct(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0) {
        *weightA = 1.0;
        *weightB = 0.0;
        *weightC = 0.0;
        return (point - a).lengthSquared();
    }
    const AutoRemesher::Vector3 bp = point - b;
    const double d3 = AutoRemesher::Vector3::dotProduct(ab, bp);
    const double d4 = AutoRemesher::Vector3::dotProduct(ac, bp);
    if (d3 >= 0.0 && d4 <= d3) {
        *weightA = 0.0;
        *weightB = 1.0;
        *weightC = 0.0;
        return (point - b).lengthSquared();
    }
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
        const double denom = d1 - d3;
        const double v = (denom != 0.0) ? d1 / denom : 0.0;
        *weightA = 1.0 - v;
        *weightB = v;
        *weightC = 0.0;
        return (point - (a + ab * v)).lengthSquared();
    }
    const AutoRemesher::Vector3 cp = point - c;
    const double d5 = AutoRemesher::Vector3::dotProduct(ab, cp);
    const double d6 = AutoRemesher::Vector3::dotProduct(ac, cp);
    if (d6 >= 0.0 && d5 <= d6) {
        *weightA = 0.0;
        *weightB = 0.0;
        *weightC = 1.0;
        return (point - c).lengthSquared();
    }
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
        const double denom = d2 - d6;
        const double w = (denom != 0.0) ? d2 / denom : 0.0;
        *weightA = 1.0 - w;
        *weightB = 0.0;
        *weightC = w;
        return (point - (a + ac * w)).lengthSquared();
    }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
        const double denom = (d4 - d3) + (d5 - d6);
        const double w = (denom != 0.0) ? (d4 - d3) / denom : 0.0;
        *weightA = 0.0;
        *weightB = 1.0 - w;
        *weightC = w;
        return (point - (b + (c - b) * w)).lengthSquared();
    }
    const double denom = va + vb + vc;
    if (!(std::fabs(denom) > 0.0)) {
        *weightA = 1.0;
        *weightB = 0.0;
        *weightC = 0.0;
        return (point - a).lengthSquared();
    }
    const double v = vb / denom;
    const double w = vc / denom;
    *weightA = 1.0 - v - w;
    *weightB = v;
    *weightC = w;
    return (point - (a + ab * v + ac * w)).lengthSquared();
}

}

namespace AutoRemesher {

size_t UvTransfer::CellKeyHash::operator()(const CellKey& key) const
{
    const size_t first = std::hash<int>()(key.x);
    const size_t second = std::hash<int>()(key.y);
    const size_t third = std::hash<int>()(key.z);
    return first ^ (second * 73856093u) ^ (third * 19349663u);
}

UvTransfer::UvTransfer(const std::vector<Vector3>& vertices,
    const std::vector<std::vector<size_t>>& triangles,
    const std::vector<std::vector<Vector2>>& triangleUvs)
    : m_vertices(vertices)
    , m_triangles(triangles)
    , m_triangleUvs(triangleUvs)
{
    if (m_triangles.empty() || m_triangleUvs.size() != m_triangles.size()
        || m_vertices.empty())
        return;
    double totalEdge = 0.0;
    size_t edgeCount = 0;
    for (const auto& triangle : m_triangles) {
        for (size_t i = 0; i < triangle.size(); ++i) {
            const size_t j = (i + 1) % triangle.size();
            if (triangle[i] >= m_vertices.size() || triangle[j] >= m_vertices.size())
                continue;
            totalEdge += (m_vertices[triangle[i]] - m_vertices[triangle[j]]).length();
            ++edgeCount;
        }
    }
    if (0 == edgeCount)
        return;
    m_cellSize = totalEdge / (double)edgeCount;
    if (!(m_cellSize > 0.0))
        return;
    std::unordered_map<CellKey, std::vector<size_t>, CellKeyHash> cells;
    for (size_t triangleIndex = 0; triangleIndex < m_triangles.size(); ++triangleIndex) {
        const auto& triangle = m_triangles[triangleIndex];
        if (triangle.size() < 3)
            continue;
        bool skippable = false;
        for (const size_t index : triangle) {
            if (index >= m_vertices.size()) {
                skippable = true;
                break;
            }
        }
        if (skippable)
            continue;
        const Vector3& a = m_vertices[triangle[0]];
        Vector3 low = a;
        Vector3 high = a;
        for (size_t i = 1; i < triangle.size(); ++i) {
            const Vector3& vertex = m_vertices[triangle[i]];
            for (int axis = 0; axis < 3; ++axis) {
                low[axis] = std::min(low[axis], vertex[axis]);
                high[axis] = std::max(high[axis], vertex[axis]);
            }
        }
        const CellKey lowCell = cellOf(low);
        const CellKey highCell = cellOf(high);
        for (int x = lowCell.x; x <= highCell.x; ++x) {
            for (int y = lowCell.y; y <= highCell.y; ++y) {
                for (int z = lowCell.z; z <= highCell.z; ++z) {
                    CellKey key;
                    key.x = x;
                    key.y = y;
                    key.z = z;
                    cells[key].push_back(triangleIndex);
                }
            }
        }
    }
    m_cellTriangles.reserve(m_triangles.size() * 2);
    for (auto& cell : cells) {
        const size_t begin = m_cellTriangles.size();
        m_cellTriangles.insert(m_cellTriangles.end(), cell.second.begin(), cell.second.end());
        m_cellStarts[cell.first] = std::make_pair(begin, m_cellTriangles.size());
    }
}

UvTransfer::CellKey UvTransfer::cellOf(const Vector3& point) const
{
    CellKey key;
    key.x = (int)std::floor(point.x() / m_cellSize);
    key.y = (int)std::floor(point.y() / m_cellSize);
    key.z = (int)std::floor(point.z() / m_cellSize);
    return key;
}

bool UvTransfer::locateDetailed(const Vector3& point, size_t* triangleIndex,
    double* weightA, double* weightB, double* weightC) const
{
    if (!valid())
        return false;
    const CellKey center = cellOf(point);
    double bestDistance = std::numeric_limits<double>::max();
    double bestA = 0.0;
    double bestB = 0.0;
    double bestC = 0.0;
    size_t bestTriangle = std::numeric_limits<size_t>::max();
    const int maxRing = 8;
    int bestRing = maxRing + 1;
    for (int ring = 0; ring <= maxRing; ++ring) {
        if (bestRing <= ring - 1)
            break;
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
                    const auto cell = m_cellStarts.find({ x, y, z });
                    if (cell == m_cellStarts.end())
                        continue;
                    for (size_t i = cell->second.first; i < cell->second.second; ++i) {
                        const size_t candidate = m_cellTriangles[i];
                        const auto& triangle = m_triangles[candidate];
                        double candidateA = 0.0;
                        double candidateB = 0.0;
                        double candidateC = 0.0;
                        const double distance = closestPointOnTriangle(point,
                            m_vertices[triangle[0]], m_vertices[triangle[1]], m_vertices[triangle[2]],
                            &candidateA, &candidateB, &candidateC);
                        if (distance >= bestDistance)
                            continue;
                        bestDistance = distance;
                        bestTriangle = candidate;
                        bestA = candidateA;
                        bestB = candidateB;
                        bestC = candidateC;
                        bestRing = std::max(std::abs(x - center.x),
                            std::max(std::abs(y - center.y), std::abs(z - center.z)));
                    }
                }
            }
        }
        if (bestTriangle != std::numeric_limits<size_t>::max() && ring > bestRing)
            break;
    }
    if (bestTriangle == std::numeric_limits<size_t>::max())
        return false;
    *triangleIndex = bestTriangle;
    *weightA = bestA;
    *weightB = bestB;
    *weightC = bestC;
    return true;
}

bool UvTransfer::locate(const Vector3& point, Vector2* uv) const
{
    if (nullptr == uv)
        return false;
    size_t triangleIndex = 0;
    double weightA = 0.0;
    double weightB = 0.0;
    double weightC = 0.0;
    if (!locateDetailed(point, &triangleIndex, &weightA, &weightB, &weightC))
        return false;
    const auto& cornerUvs = m_triangleUvs[triangleIndex];
    if (cornerUvs.size() < 3)
        return false;
    uv->setX(weightA * cornerUvs[0].x() + weightB * cornerUvs[1].x() + weightC * cornerUvs[2].x());
    uv->setY(weightA * cornerUvs[0].y() + weightB * cornerUvs[1].y() + weightC * cornerUvs[2].y());
    return std::isfinite(uv->x()) && std::isfinite(uv->y());
}

bool UvTransfer::locateTriangle(const Vector3& point, size_t* triangleIndex) const
{
    if (nullptr == triangleIndex)
        return false;
    double weightA = 0.0;
    double weightB = 0.0;
    double weightC = 0.0;
    return locateDetailed(point, triangleIndex, &weightA, &weightB, &weightC);
}

bool UvTransfer::evaluate(const Vector3& point, size_t triangleIndex, Vector2* uv) const
{
    if (nullptr == uv || triangleIndex >= m_triangles.size()
        || triangleIndex >= m_triangleUvs.size())
        return false;
    const auto& triangle = m_triangles[triangleIndex];
    const auto& cornerUvs = m_triangleUvs[triangleIndex];
    if (triangle.size() < 3 || cornerUvs.size() < 3)
        return false;
    const Vector3& a = m_vertices[triangle[0]];
    const Vector3& b = m_vertices[triangle[1]];
    const Vector3& c = m_vertices[triangle[2]];
    const Vector3 first = b - a;
    const Vector3 second = c - a;
    // Project onto the triangle's plane and solve for the coordinates in the
    // (first, second) basis: that is the same map the parameterization carries.
    Vector3 normal = Vector3::crossProduct(first, second);
    const double normalLengthSquared = normal.lengthSquared();
    if (!(normalLengthSquared > 0.0))
        return locate(point, uv);
    const Vector3 relative = point - a;
    const double offset = Vector3::dotProduct(relative, normal) / normalLengthSquared;
    const Vector3 projected = point - normal * offset;
    const Vector3 inPlane = projected - a;
    const double g11 = Vector3::dotProduct(first, first);
    const double g12 = Vector3::dotProduct(first, second);
    const double g22 = Vector3::dotProduct(second, second);
    const double r1 = Vector3::dotProduct(first, inPlane);
    const double r2 = Vector3::dotProduct(second, inPlane);
    const double determinant = g11 * g22 - g12 * g12;
    if (!(std::fabs(determinant) > 0.0))
        return locate(point, uv);
    const double x = (g22 * r1 - g12 * r2) / determinant;
    const double y = (g11 * r2 - g12 * r1) / determinant;
    uv->setX(cornerUvs[0].x() + x * (cornerUvs[1].x() - cornerUvs[0].x())
        + y * (cornerUvs[2].x() - cornerUvs[0].x()));
    uv->setY(cornerUvs[0].y() + x * (cornerUvs[1].y() - cornerUvs[0].y())
        + y * (cornerUvs[2].y() - cornerUvs[0].y()));
    return std::isfinite(uv->x()) && std::isfinite(uv->y());
}

}
