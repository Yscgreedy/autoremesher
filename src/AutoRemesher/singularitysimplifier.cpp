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
#include <AutoRemesher/SingularitySimplifier>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <set>
#include <utility>

namespace AutoRemesher {
namespace {
    Vector3 unit(const Vector3& value, const Vector3& fallback)
    {
        return value.length() < 1e-12 ? fallback.normalized() : value.normalized();
    }
    struct Frame {
        Vector3 u;
        Vector3 v;
    };

    Frame frameForFace(const SurfaceMesh& mesh, size_t face)
    {
        const Vector3 normal = unit(mesh.faceNormal(face), Vector3(0, 0, 1));
        Vector3 u = mesh.edgeVector(3 * face);
        u = u - Vector3::dotProduct(u, normal) * normal;
        u = unit(u, std::fabs(normal.x()) < 0.9 ? Vector3(1, 0, 0) : Vector3(0, 1, 0));
        u = u - Vector3::dotProduct(u, normal) * normal;
        u = unit(u, Vector3(1, 0, 0));
        return { u, Vector3::crossProduct(normal, u) };
    }

    double angleInFrame(const Vector3& vector, const Frame& frame)
    {
        return std::atan2(Vector3::dotProduct(vector, frame.v), Vector3::dotProduct(vector, frame.u));
    }

    int quarterTurn(const SurfaceMesh& mesh, size_t corner,
        const std::vector<Frame>& frames,
        const std::vector<double>& fieldAngles)
    {
        const size_t opposite = mesh.oppositeCorner(corner);
        if (opposite == SurfaceMesh::npos)
            return 0;
        const size_t f = mesh.cornerFace(corner), g = mesh.cornerFace(opposite);
        const Vector3 edge = mesh.edgeVector(corner);
        const double connection = angleInFrame(edge, frames[g]) - angleInFrame(edge, frames[f]);
        const int turns = static_cast<int>(std::lround((fieldAngles[g] - fieldAngles[f] - connection) / (M_PI / 2.0)));
        return ((turns % 4) + 4) % 4;
    }
}

SingularitySimplifier::SingularitySimplifier(const SurfaceMesh& mesh,
    std::vector<Vector3>* faceField)
    : m_mesh(mesh)
    , m_field(faceField)
{
}

// The closed-fan test only depends on topology, so it is evaluated once per
// vertex; the sum itself is mirrored in m_chargeSum and kept up to date by
// writeMismatch().
void SingularitySimplifier::buildChargeSums()
{
    const size_t vertexCount = m_mesh.vertexCount();
    m_chargeSum.assign(vertexCount, 0);
    m_chargeClosed.assign(vertexCount, 0);
    for (size_t v = 0; v < vertexCount; ++v) {
        const std::vector<size_t>& corners = m_mesh.cornersAroundVertex(v);
        if (corners.empty())
            continue;
        size_t c = corners.front();
        const size_t start = c;
        bool closed = true;
        size_t steps = 0;
        do {
            if (++steps > m_mesh.cornerCount()) {
                closed = false;
                break;
            }
            const size_t opposite = m_mesh.oppositeCorner(c);
            if (SurfaceMesh::npos == opposite) {
                closed = false;
                break;
            }
            c = m_mesh.nextCorner(opposite);
            if (m_mesh.cornerVertex(c) != v) {
                closed = false;
                break;
            }
        } while (c != start);
        m_chargeClosed[v] = closed ? 1 : 0;
    }
}

void SingularitySimplifier::writeMismatch(size_t corner, int value)
{
    const int previous = m_mismatch[corner];
    if (previous == value)
        return;
    m_mismatch[corner] = value;
    const size_t vertex = m_mesh.cornerVertex(corner);
    if (vertex < m_chargeSum.size())
        m_chargeSum[vertex] += value - previous;
}

std::vector<int> SingularitySimplifier::vertexCharges() const
{
    if (nullptr == m_field || m_field->size() != m_mesh.faceCount())
        return std::vector<int>(m_mesh.vertexCount(), 0);
    // Without cached mismatches there is nothing to sum, so fall back to the
    // per-corner quarter turn evaluation.
    if (m_mismatch.size() != m_mesh.cornerCount()
        || m_chargeSum.size() != m_mesh.vertexCount())
        return vertexChargesUncached();
    std::vector<int> result(m_mesh.vertexCount(), 0);
    for (size_t v = 0; v < result.size(); ++v) {
        if (!m_chargeClosed[v])
            continue;
        result[v] = ((m_chargeSum[v] % 4) + 4) % 4;
    }
    static const bool verifyCharges = nullptr != getenv("AR_VERIFY_CHARGES");
    if (verifyCharges) {
        const std::vector<int> reference = vertexChargesUncached();
        for (size_t v = 0; v < reference.size() && v < result.size(); ++v)
            if (reference[v] != result[v])
                fprintf(stderr, "charge mismatch at %zu: incremental %d reference %d\n",
                    v, result[v], reference[v]);
    }
    return result;
}

const std::vector<int>& SingularitySimplifier::chargesCached() const
{
    if (m_chargeCache.size() != m_mesh.vertexCount())
        m_chargeCache.assign(m_mesh.vertexCount(), 0);
    if (nullptr == m_field || m_field->size() != m_mesh.faceCount()
        || m_mismatch.size() != m_mesh.cornerCount()
        || m_chargeSum.size() != m_mesh.vertexCount()) {
        m_chargeCache = vertexChargesUncached();
        return m_chargeCache;
    }
    for (size_t v = 0; v < m_chargeCache.size(); ++v)
        m_chargeCache[v] = m_chargeClosed[v] ? ((m_chargeSum[v] % 4) + 4) % 4 : 0;
    return m_chargeCache;
}

std::vector<int> SingularitySimplifier::vertexChargesUncached() const
{
    std::vector<int> result(m_mesh.vertexCount(), 0);
    if (nullptr == m_field || m_field->size() != m_mesh.faceCount())
        return result;
    // simplify() is an inner loop that calls this several times per cancelled
    // pair, so only pay for the per-face frames when the cached corner
    // mismatches are missing and quarterTurn actually needs them.
    const bool useCachedMismatch = m_mismatch.size() == m_mesh.cornerCount();
    std::vector<Frame> frames;
    std::vector<double> fieldAngles;
    if (!useCachedMismatch) {
        frames.reserve(m_mesh.faceCount());
        fieldAngles.reserve(m_mesh.faceCount());
        for (size_t f = 0; f < m_mesh.faceCount(); ++f) {
            frames.push_back(frameForFace(m_mesh, f));
            fieldAngles.push_back(angleInFrame((*m_field)[f], frames.back()));
        }
    }
    for (size_t v = 0; v < m_mesh.vertexCount(); ++v) {
        const std::vector<size_t>& corners = m_mesh.cornersAroundVertex(v);
        if (corners.empty())
            continue;
        size_t c = corners.front();
        const size_t start = c;
        int sum = 0;
        size_t steps = 0;
        do {
            if (++steps > m_mesh.cornerCount()) {
                sum = 0;
                break;
            }
            sum += useCachedMismatch ? m_mismatch[c] : quarterTurn(m_mesh, c, frames, fieldAngles);
            const size_t opposite = m_mesh.oppositeCorner(c);
            if (opposite == SurfaceMesh::npos) {
                sum = 0;
                break;
            }
            c = m_mesh.nextCorner(opposite);
            if (m_mesh.cornerVertex(c) != v) {
                sum = 0;
                break;
            }
        } while (c != start);
        if (c == start) {
            result[v] = ((sum % 4) + 4) % 4;
        }
    }
    return result;
}

size_t SingularitySimplifier::singularityCount() const
{
    size_t result = 0;
    for (int charge : vertexCharges())
        if (charge != 0)
            ++result;
    return result;
}

void SingularitySimplifier::buildFramesAndConnection()
{
    const size_t faces = m_mesh.faceCount(), corners = m_mesh.cornerCount();
    m_frameU.resize(faces);
    m_frameV.resize(faces);
    m_angles.resize(faces);
    for (size_t f = 0; f < faces; ++f) {
        const Frame frame = frameForFace(m_mesh, f);
        m_frameU[f] = frame.u;
        m_frameV[f] = frame.v;
        m_angles[f] = std::atan2(Vector3::dotProduct((*m_field)[f], frame.v),
            Vector3::dotProduct((*m_field)[f], frame.u));
    }
    m_connection.assign(corners, 0.0);
    m_mismatch.assign(corners, 0);
    m_sharpCorner.assign(corners, false);
    buildChargeSums();
    for (size_t c = 0; c < corners; ++c) {
        const size_t other = m_mesh.oppositeCorner(c);
        if (other == SurfaceMesh::npos)
            continue;
        const size_t f = m_mesh.cornerFace(c), g = m_mesh.cornerFace(other);
        const Vector3 edge = m_mesh.edgeVector(c);
        m_connection[c] = std::atan2(Vector3::dotProduct(edge, m_frameV[g]), Vector3::dotProduct(edge, m_frameU[g])) - std::atan2(Vector3::dotProduct(edge, m_frameV[f]), Vector3::dotProduct(edge, m_frameU[f]));
        m_sharpCorner[c] = m_mesh.normalAngle(c) >= m_sharpEdgeDegrees * M_PI / 180.0;
    }
    std::vector<size_t> every(faces);
    for (size_t f = 0; f < faces; ++f)
        every[f] = f;
    updateMismatches(every);
}

void SingularitySimplifier::updateMismatches(const std::vector<size_t>& faces)
{
    for (const size_t f : faces)
        for (size_t c = 3 * f; c < 3 * f + 3; ++c) {
            const size_t other = m_mesh.oppositeCorner(c);
            if (other == SurfaceMesh::npos)
                continue;
            const size_t g = m_mesh.cornerFace(other);
            const int turns = static_cast<int>(std::lround((m_angles[g] - (m_angles[f] + m_connection[c])) / (M_PI / 2.0)));
            writeMismatch(c, ((turns % 4) + 4) % 4);
            writeMismatch(other, (4 - m_mismatch[c]) % 4);
        }
}

std::vector<size_t> SingularitySimplifier::facetsAroundVertex(size_t vertex) const
{
    const std::vector<size_t>& incident = m_mesh.cornersAroundVertex(vertex);
    if (incident.empty())
        return {};
    std::vector<size_t> result;
    size_t c = incident.front(), start = c;
    do {
        if (result.size() > m_mesh.cornerCount())
            return {};
        result.push_back(m_mesh.cornerFace(c));
        const size_t other = m_mesh.oppositeCorner(c);
        if (other == SurfaceMesh::npos)
            return {};
        c = m_mesh.nextCorner(other);
        if (m_mesh.cornerVertex(c) != vertex)
            return {};
    } while (c != start);
    return result;
}

size_t SingularitySimplifier::cornerAlongEdge(size_t from, size_t to) const
{
    for (const size_t c : m_mesh.cornersAroundVertex(from)) {
        if (m_mesh.cornerVertex(m_mesh.nextCorner(c)) == to && m_mesh.oppositeCorner(c) != SurfaceMesh::npos)
            return c;
    }
    return SurfaceMesh::npos;
}

std::vector<size_t> SingularitySimplifier::pathBetween(size_t first, size_t second,
    const std::vector<int>& regionStamp, int regionGeneration,
    const std::vector<size_t>& freeFaces) const
{
    if (m_pathPrevious.size() != m_mesh.vertexCount()) {
        m_pathPrevious.assign(m_mesh.vertexCount(), SurfaceMesh::npos);
        m_pathStamp.assign(m_mesh.vertexCount(), 0);
    }
    const int generation = ++m_generation;
    std::vector<char> isFree(m_mesh.faceCount(), 0);
    for (const size_t f : freeFaces)
        isFree[f] = 1;
    const auto usable = [&](size_t vertex) {
        const std::vector<size_t> fan = facetsAroundVertex(vertex);
        if (fan.empty())
            return false;
        for (const size_t f : fan)
            if (regionStamp[f] != regionGeneration)
                return false;
        return true;
    };
    const auto crossable = [&](size_t corner) {
        const size_t other = m_mesh.oppositeCorner(corner);
        return other != SurfaceMesh::npos && isFree[m_mesh.cornerFace(corner)] && isFree[m_mesh.cornerFace(other)];
    };
    std::deque<size_t> queue;
    m_pathStamp[first] = generation;
    m_pathPrevious[first] = first;
    queue.push_back(first);
    while (!queue.empty()) {
        const size_t v = queue.front();
        queue.pop_front();
        if (v == second) {
            std::vector<size_t> path;
            for (size_t at = second; at != first; at = m_pathPrevious[at])
                path.push_back(at);
            path.push_back(first);
            std::reverse(path.begin(), path.end());
            return path;
        }
        for (const size_t c : m_mesh.cornersAroundVertex(v)) {
            const size_t next = m_mesh.cornerVertex(m_mesh.nextCorner(c));
            if (m_pathStamp[next] == generation || !crossable(c))
                continue;
            if (next != second && !usable(next))
                continue;
            m_pathStamp[next] = generation;
            m_pathPrevious[next] = v;
            queue.push_back(next);
        }
    }
    return {};
}

bool SingularitySimplifier::cancelPair(size_t first, size_t second, size_t hops)
{
    const size_t radius = hops + m_regionMargin;
    const size_t faceCount = m_mesh.faceCount();
    const size_t vertexCount = m_mesh.vertexCount();
    const size_t cornerCount = m_mesh.cornerCount();
    if (m_ballDistance.size() != faceCount) {
        m_ballDistance.assign(faceCount, SurfaceMesh::npos);
        m_ballStampFirst.assign(faceCount, 0);
        m_ballStampSecond.assign(faceCount, 0);
        m_regionStamp.assign(faceCount, 0);
    }
    if (m_affectedStamp.size() != vertexCount)
        m_affectedStamp.assign(vertexCount, 0);
    if (m_jump.size() != cornerCount) {
        m_jump.assign(cornerCount, 0);
        m_jumpStamp.assign(cornerCount, 0);
    }

    // Every array below is reused and cleared through a generation stamp, so a
    // candidate costs work proportional to its own neighborhood instead of a
    // pass over the whole mesh.
    const int generationFirst = ++m_generation;
    const int generationSecond = ++m_generation;
    std::vector<size_t> firstBall, secondBall;
    const auto ball = [&](const std::vector<size_t>& seeds, std::vector<int>& stamps,
                          int stamp, std::vector<size_t>& visited) {
        visited.clear();
        std::deque<size_t> queue;
        for (size_t f : seeds)
            if (stamps[f] != stamp) {
                stamps[f] = stamp;
                m_ballDistance[f] = 0;
                visited.push_back(f);
                queue.push_back(f);
            }
        while (!queue.empty()) {
            const size_t f = queue.front();
            queue.pop_front();
            if (m_ballDistance[f] >= radius)
                continue;
            for (size_t c = 3 * f; c < 3 * f + 3; ++c) {
                const size_t other = m_mesh.oppositeCorner(c);
                if (other == SurfaceMesh::npos)
                    continue;
                const size_t g = m_mesh.cornerFace(other);
                if (stamps[g] == stamp)
                    continue;
                stamps[g] = stamp;
                m_ballDistance[g] = m_ballDistance[f] + 1;
                visited.push_back(g);
                queue.push_back(g);
            }
        }
    };

    const std::vector<size_t> firstFan = facetsAroundVertex(first), secondFan = facetsAroundVertex(second);
    if (firstFan.empty() || secondFan.empty())
        return false;
    ball(firstFan, m_ballStampFirst, generationFirst, firstBall);
    ball(secondFan, m_ballStampSecond, generationSecond, secondBall);

    const bool firstIsSmaller = firstBall.size() <= secondBall.size();
    const std::vector<size_t>& smallerBall = firstIsSmaller ? firstBall : secondBall;
    const std::vector<int>& otherStamps = firstIsSmaller ? m_ballStampSecond : m_ballStampFirst;
    const int otherGeneration = firstIsSmaller ? generationSecond : generationFirst;
    const int regionGeneration = ++m_generation;
    std::vector<size_t> region, freeFaces;
    for (const size_t f : smallerBall) {
        if (otherStamps[f] != otherGeneration)
            continue;
        m_regionStamp[f] = regionGeneration;
        region.push_back(f);
    }
    if (region.empty())
        return false;
    // The Gauss-Seidel sweep below walks the free faces in this order, so keep the
    // ascending face order the whole-mesh scan used to produce; the result then
    // does not depend on which of the two balls happened to be smaller.
    std::sort(region.begin(), region.end());
    for (const size_t f : region) {
        bool frozen = false;
        for (size_t c = 3 * f; c < 3 * f + 3; ++c) {
            const size_t other = m_mesh.oppositeCorner(c);
            if (m_sharpCorner[c] || other == SurfaceMesh::npos
                || m_regionStamp[m_mesh.cornerFace(other)] != regionGeneration) {
                frozen = true;
                break;
            }
        }
        if (!frozen)
            freeFaces.push_back(f);
    }
    if (freeFaces.empty())
        return false;
    for (const size_t v : { first, second })
        for (const size_t f : facetsAroundVertex(v))
            if (m_regionStamp[f] != regionGeneration)
                return false;
    const std::vector<size_t> path = pathBetween(first, second, m_regionStamp,
        regionGeneration, freeFaces);
    if (path.size() < 2)
        return false;

    const int affectedGeneration = ++m_generation;
    std::vector<size_t> affectedVertices;
    for (const size_t f : region)
        for (size_t c = 3 * f; c < 3 * f + 3; ++c) {
            const size_t vertex = m_mesh.cornerVertex(c);
            if (m_affectedStamp[vertex] == affectedGeneration)
                continue;
            m_affectedStamp[vertex] = affectedGeneration;
            affectedVertices.push_back(vertex);
        }
    const std::vector<int>& chargesBefore = chargesCached();
    size_t before = 0;
    for (const size_t vertex : affectedVertices)
        if (0 != chargesBefore[vertex])
            ++before;

    std::vector<double> saved;
    saved.reserve(freeFaces.size());
    for (const size_t f : freeFaces)
        saved.push_back(m_angles[f]);

    const int jumpGeneration = ++m_generation;
    const auto addJump = [&](size_t corner, int delta) {
        if (m_jumpStamp[corner] != jumpGeneration) {
            m_jumpStamp[corner] = jumpGeneration;
            m_jump[corner] = 0;
        }
        m_jump[corner] += delta;
    };
    const auto jumpAt = [&](size_t corner) {
        return m_jumpStamp[corner] == jumpGeneration ? m_jump[corner] : 0;
    };
    for (const size_t f : region)
        for (size_t c = 3 * f; c < 3 * f + 3; ++c) {
            const size_t other = m_mesh.oppositeCorner(c);
            if (other == SurfaceMesh::npos)
                continue;
            const int jump = static_cast<int>(std::lround(
                (m_angles[m_mesh.cornerFace(other)] - m_angles[f] - m_connection[c]) / (M_PI / 2.0)));
            if (0 != jump)
                addJump(c, jump);
        }
    const int carried = (((chargesBefore[first] + 1) % 4) + 4) % 4 - 1;
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        const size_t corner = cornerAlongEdge(path[i], path[i + 1]);
        if (corner == SurfaceMesh::npos)
            return false;
        const size_t other = m_mesh.oppositeCorner(corner);
        addJump(corner, -carried);
        addJump(other, carried);
    }

    for (size_t iteration = 0; iteration < std::max<size_t>(256, 40 * radius * radius); ++iteration) {
        double maxChange = 0;
        for (const size_t f : freeFaces) {
            double sum = 0, sw = 0;
            for (size_t c = 3 * f; c < 3 * f + 3; ++c) {
                const size_t other = m_mesh.oppositeCorner(c);
                if (other == SurfaceMesh::npos)
                    continue;
                const size_t g = m_mesh.cornerFace(other);
                const double w = m_mesh.edgeVector(c).length();
                sum += w * (m_angles[g] - m_connection[c] - jumpAt(c) * (M_PI / 2.0));
                sw += w;
            }
            if (sw > 0) {
                const double value = sum / sw;
                maxChange = std::max(maxChange, std::fabs(value - m_angles[f]));
                m_angles[f] = value;
            }
        }
        if (maxChange < 1e-10)
            break;
    }
    updateMismatches(region);
    const std::vector<int>& chargesAfter = chargesCached();
    size_t after = 0;
    for (const size_t vertex : affectedVertices)
        if (0 != chargesAfter[vertex])
            ++after;
    if (chargesAfter[first] != 0 || chargesAfter[second] != 0 || after + 2 > before) {
        for (size_t i = 0; i < freeFaces.size(); ++i)
            m_angles[freeFaces[i]] = saved[i];
        updateMismatches(region);
        return false;
    }
    for (const size_t f : freeFaces)
        (*m_field)[f] = std::cos(m_angles[f]) * m_frameU[f] + std::sin(m_angles[f]) * m_frameV[f];
    return true;
}

void SingularitySimplifier::simplify()
{
    if (nullptr == m_field || m_field->empty())
        return;
    buildFramesAndConnection();
    m_cancelledPairCount = 0;
    const auto initial = vertexCharges();
    m_singularityCountBefore = 0;
    for (int i : initial)
        if (i)
            m_singularityCountBefore++;
    m_singularityCountAfter = m_singularityCountBefore;
    for (size_t round = 0; round < m_maximumRounds; ++round) {
        const auto charges = vertexCharges();
        std::vector<size_t> singular;
        for (size_t v = 0; v < charges.size(); ++v)
            if (charges[v])
                singular.push_back(v);
        if (singular.size() < 2)
            break;
        std::vector<size_t> owner(m_mesh.faceCount(), SurfaceMesh::npos), distance(m_mesh.faceCount(), 0);
        std::deque<size_t> queue;
        std::vector<std::pair<size_t, size_t>> encounters;
        for (size_t i = 0; i < singular.size(); ++i)
            for (size_t f : facetsAroundVertex(singular[i])) {
                if (owner[f] == SurfaceMesh::npos) {
                    owner[f] = i;
                    queue.push_back(f);
                } else
                    encounters.push_back({ owner[f], i });
            }
        struct Candidate {
            size_t a, b, hops;
        };
        std::vector<Candidate> candidates;
        std::set<std::pair<size_t, size_t>> seen;
        auto consider = [&](size_t a, size_t b, size_t d) {if(a==b||d>m_maximumPairDistance||(charges[singular[a]]+charges[singular[b]])%4)return;auto key=std::minmax(a,b);if(seen.insert(key).second)candidates.push_back({key.first,key.second,d}); };
        for (const auto& e : encounters)
            consider(e.first, e.second, 0);
        while (!queue.empty()) {
            const size_t f = queue.front();
            queue.pop_front();
            for (size_t c = 3 * f; c < 3 * f + 3; ++c) {
                const size_t other = m_mesh.oppositeCorner(c);
                if (other == SurfaceMesh::npos)
                    continue;
                const size_t g = m_mesh.cornerFace(other);
                if (owner[g] == SurfaceMesh::npos) {
                    if (distance[f] + 1 > m_maximumPairDistance)
                        continue;
                    owner[g] = owner[f];
                    distance[g] = distance[f] + 1;
                    queue.push_back(g);
                } else
                    consider(owner[f], owner[g], distance[f] + distance[g]);
            }
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& x, const Candidate& y) { return x.hops < y.hops; });
        std::vector<char> used(singular.size(), 0);
        size_t cancelled = 0;
        for (const Candidate& c : candidates)
            if (!used[c.a] && !used[c.b] && cancelPair(singular[c.a], singular[c.b], c.hops)) {
                used[c.a] = used[c.b] = 1;
                ++cancelled;
            }
        if (!cancelled)
            break;
        m_cancelledPairCount += cancelled;
    }
    m_singularityCountAfter = singularityCount();
}
}
