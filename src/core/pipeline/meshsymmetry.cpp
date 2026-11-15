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
#include <cstdint>
#include <map>
#include <unordered_map>

namespace AutoRemesher {

SymmetryResult mirrorHalf(const std::vector<Vector3>& sourceVertices,
    const std::vector<std::vector<size_t>>& sourceFaces,
    const std::vector<std::vector<Vector2>>& sourceFaceUvs,
    const std::vector<Vector2>& vertexUvs,
    int axisValue, double plane, double tolerance, bool keepPositive)
{
    SymmetryResult result;
    SymmetryStats& stats = result.stats;
    if (axisValue < 1 || axisValue > 3 || sourceVertices.empty())
        return result;
    stats.axis = axisValue;
    stats.plane = plane;
    stats.verticesBefore = sourceVertices.size();
    const size_t axis = (size_t)(axisValue - 1);
    // Which half survives is a sign, not a second copy of this function: the cut
    // below measures the distance to the plane and multiplies it by side, so the
    // kept side always reads as ">= 0" and both halves run the same arithmetic.
    // Multiplication by exactly +1.0 / -1.0 is exact in IEEE 754, so
    // keepPositive == true reproduces the previous implementation bit for bit
    // (that path sits behind a sha regression gate).
    const double side = keepPositive ? 1.0 : -1.0;
    // "Strictly on the kept side", as the two exact comparisons of the original
    // code rather than as side * distance: the difference form would turn an
    // infinite coordinate into NaN and answer differently than before, and this
    // half of the pass is behind a byte-for-byte regression gate.
    const auto isStrictlyOnKeptSide = [&](double coordinate) {
        return keepPositive ? coordinate > plane : coordinate < plane;
    };
    // The quad mesh is cut at the plane and the kept half is mirrored onto the
    // other one, so the result is symmetric by construction instead of by
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
            const double coordinate = (sourceVertices[corner][axis] - plane) * side;
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
            const double di = (sourceVertices[face[i]][axis] - plane) * side;
            const double dj = (sourceVertices[face[j]][axis] - plane) * side;
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
            if (isStrictlyOnKeptSide(vertices[corner][axis])) {
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

SymmetryResult mirrorSymmetrize(const std::vector<Vector3>& sourceVertices,
    const std::vector<std::vector<size_t>>& sourceFaces,
    const std::vector<std::vector<Vector2>>& sourceFaceUvs,
    const std::vector<Vector2>& vertexUvs,
    int axisValue, double plane, double tolerance)
{
    return mirrorHalf(sourceVertices, sourceFaces, sourceFaceUvs, vertexUvs,
        axisValue, plane, tolerance, /*keepPositive=*/true);
}

namespace {

    // Cell keys are exact integers, so any well mixing hash is correct; this
    // splitmix64 finaliser is what keeps the table from degenerating on structured
    // input.  A vertex grid hashes into lattices, and a plain xor or sum of the three
    // coordinates sends whole sheets of cells into the same bucket.
    inline uint64_t mixGridHash(uint64_t value)
    {
        value += 0x9e3779b97f4a7c15ull;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
        return value ^ (value >> 31);
    }

    struct GridCell {
        long long x = 0;
        long long y = 0;
        long long z = 0;

        bool operator==(const GridCell& other) const
        {
            return x == other.x && y == other.y && z == other.z;
        }
    };

    struct GridCellHash {
        size_t operator()(const GridCell& cell) const
        {
            const uint64_t merged = mixGridHash((uint64_t)cell.x)
                ^ mixGridHash((uint64_t)cell.y + 0x9e3779b97f4a7c15ull);
            return (size_t)mixGridHash(merged ^ mixGridHash((uint64_t)cell.z + 0xc2b2ae3d27d4eb4full));
        }
    };

    inline bool isFinitePoint(const Vector3& point)
    {
        return std::isfinite(point[0]) && std::isfinite(point[1]) && std::isfinite(point[2]);
    }

    // Integer cell coordinate.  Anything outside the representable range, NaN
    // included, is clamped instead of cast: converting such a double to long long is
    // undefined behaviour, and a broken vertex must not be able to corrupt the key
    // space on its way to being ignored.
    inline long long gridCoordinate(double value, double origin, double cellSize)
    {
        const double scaled = (value - origin) / cellSize;
        const double range = 4.0e18;
        if (!(scaled > -range))
            return -4000000000000000000LL;
        if (!(scaled < range))
            return 4000000000000000000LL;
        return (long long)std::floor(scaled);
    }

}

SymmetryCheck evaluateSymmetry(const std::vector<Vector3>& vertices,
    const std::vector<std::vector<size_t>>& faces,
    int axis, double plane, bool estimatePlane, double tolerance)
{
    SymmetryCheck check;
    if (axis < 1 || axis > 3 || vertices.empty())
        return check;
    check.axis = axis;
    const size_t axisIndex = (size_t)(axis - 1);
    // faces is deliberately unused: whether a vertex is matched depends on where
    // it sits, and counting only the referenced vertices would quietly hide loose
    // vertices from the ratio.  The parameter stays so all three entry points
    // accept the same mesh without the caller slicing it first.
    (void)faces;
    // A negative or NaN tolerance cannot describe a neighbourhood; clamping it to
    // zero keeps the test one-sided (only coincident points match) instead of
    // silently matching everything through a comparison rewrite.
    const double limit = (tolerance > 0.0) ? tolerance : 0.0;
    check.consideredVertices = vertices.size();
    check.plane = plane;

    if (estimatePlane) {
        // The median, not the mean: a shoe or a foot is heavier on one side, and
        // its mean coordinate sits off the true mirror plane by however lopsided
        // the mass distribution is.  The median ignores that tail.
        std::vector<double> coordinates;
        coordinates.reserve(vertices.size());
        for (const auto& vertex : vertices) {
            if (std::isfinite(vertex[axisIndex]))
                coordinates.push_back(vertex[axisIndex]);
        }
        if (!coordinates.empty()) {
            const size_t middle = coordinates.size() / 2;
            std::nth_element(coordinates.begin(), coordinates.begin() + middle, coordinates.end());
            double median = coordinates[middle];
            if (0 == coordinates.size() % 2) {
                // Even count: average the two middle values, or the estimate jumps
                // by half a vertex spacing whenever a single vertex is added.
                median = 0.5 * (median + *std::max_element(coordinates.begin(), coordinates.begin() + middle));
            }
            check.plane = median;
            check.planeEstimated = true;
        }
        // Nothing finite to measure: keep the caller's plane and leave
        // planeEstimated false, so the caller knows it did not come from the mesh.
    }

    Vector3 minimum;
    Vector3 maximum;
    bool haveBounds = false;
    for (const auto& vertex : vertices) {
        if (!isFinitePoint(vertex))
            continue;
        if (!haveBounds) {
            minimum = vertex;
            maximum = vertex;
            haveBounds = true;
            continue;
        }
        for (size_t i = 0; i < 3; ++i) {
            minimum[i] = std::min(minimum[i], vertex[i]);
            maximum[i] = std::max(maximum[i], vertex[i]);
        }
    }
    if (!haveBounds)
        return check;

    const double diagonal = (maximum - minimum).length();
    // The tolerance is the smallest distance that can still count as a match, so
    // the grid never needs to be finer than that; finer would only cost memory.
    // The other direction is the danger: a tolerance as large as the model puts
    // every vertex into one cell, and every query would then walk the whole mesh.
    // The diagonal cap turns that back into a bounded number of cells.
    const double baseCell = std::max(limit, 1e-9);
    const double capCell = diagonal > 0.0 ? diagonal / 64.0 : baseCell;
    const double cellSize = std::max(std::min(baseCell, capCell), 1e-12);
    // A point within the tolerance is at most ceil(limit / cellSize) cells away,
    // so the ring walk normally stops after ring 1; eight rings is the hard bound
    // that keeps one query at no more than 17^3 cell lookups.
    int ringLimit = (int)std::min(8.0, std::ceil(limit / cellSize) + 1.0);
    if (ringLimit < 1)
        ringLimit = 1;

    std::unordered_map<GridCell, std::vector<size_t>, GridCellHash> grid;
    grid.reserve(vertices.size() + vertices.size() / 4);
    for (size_t i = 0; i < vertices.size(); ++i) {
        const Vector3& vertex = vertices[i];
        if (!isFinitePoint(vertex))
            continue;
        GridCell cell;
        cell.x = gridCoordinate(vertex[0], minimum[0], cellSize);
        cell.y = gridCoordinate(vertex[1], minimum[1], cellSize);
        cell.z = gridCoordinate(vertex[2], minimum[2], cellSize);
        grid[cell].push_back(i);
    }

    std::vector<const std::vector<size_t>*> buckets;
    double deviationSum = 0.0;
    for (size_t i = 0; i < vertices.size(); ++i) {
        const Vector3& vertex = vertices[i];
        if (!isFinitePoint(vertex))
            continue;
        double deviation = 0.0;
        if (!(std::fabs(vertex[axisIndex] - plane) <= limit)) {
            Vector3 mirrored = vertex;
            mirrored[axisIndex] = 2.0 * plane - mirrored[axisIndex];
            // Every vertex lives inside the bounding box, so a mirror point more
            // than the tolerance away from it in any axis has no partner at all.
            // On an asymmetric mesh this is the common case, and it costs six
            // comparisons instead of a few thousand cell lookups.
            if (mirrored[0] < minimum[0] - limit || mirrored[0] > maximum[0] + limit
                || mirrored[1] < minimum[1] - limit || mirrored[1] > maximum[1] + limit
                || mirrored[2] < minimum[2] - limit || mirrored[2] > maximum[2] + limit)
                continue;
            const long long baseX = gridCoordinate(mirrored[0], minimum[0], cellSize);
            const long long baseY = gridCoordinate(mirrored[1], minimum[1], cellSize);
            const long long baseZ = gridCoordinate(mirrored[2], minimum[2], cellSize);
            // Only distances up to the tolerance can match, so the tolerance is
            // also the starting best: anything farther is not a partner at all.
            double best = limit;
            bool found = false;
            // One query may not look at more stored points than this.  A mesh that
            // stacks thousands of vertices on one spot would otherwise make a
            // single cell cost O(n) per query, which is exactly the quadratic case
            // this function exists to avoid; and in a cluster that dense the
            // nearest neighbour turns up long before the budget runs out.
            size_t budget = 4096;
            for (int ring = 0; ring <= ringLimit; ++ring) {
                buckets.clear();
                for (int dx = -ring; dx <= ring; ++dx) {
                    for (int dy = -ring; dy <= ring; ++dy) {
                        for (int dz = -ring; dz <= ring; ++dz) {
                            // Only the shell: the previous rings already visited
                            // the interior cells.
                            if (ring > 0 && dx > -ring && dx < ring
                                && dy > -ring && dy < ring && dz > -ring && dz < ring)
                                continue;
                            GridCell cell;
                            cell.x = baseX + dx;
                            cell.y = baseY + dy;
                            cell.z = baseZ + dz;
                            const auto bucket = grid.find(cell);
                            if (bucket != grid.end())
                                buckets.push_back(&bucket->second);
                        }
                    }
                }
                bool done = false;
                for (const std::vector<size_t>* bucket : buckets) {
                    for (const size_t candidate : *bucket) {
                        if (0 == budget) {
                            done = true;
                            break;
                        }
                        --budget;
                        const double distance = (vertices[candidate] - mirrored).length();
                        if (distance <= best) {
                            best = distance;
                            found = true;
                            // Distance zero cannot be beaten, and stopping here is
                            // what keeps a storm of coincident vertices cheap: the
                            // whole bucket would otherwise be walked for nothing.
                            if (0.0 == best) {
                                done = true;
                                break;
                            }
                        }
                    }
                    if (done)
                        break;
                }
                if (done)
                    break;
                // Everything beyond ring r is at least r * cellSize away from the
                // query point, so a match closer than that cannot be beaten.
                if (found && best <= (double)ring * cellSize)
                    break;
            }
            if (!found)
                continue;
            deviation = best;
        }
        ++check.matchedVertices;
        deviationSum += deviation;
        check.maxDeviation = std::max(check.maxDeviation, deviation);
    }
    check.matchPct = 100.0 * (double)check.matchedVertices / (double)check.consideredVertices;
    if (check.matchedVertices > 0)
        check.meanDeviation = deviationSum / (double)check.matchedVertices;
    return check;
}

SymmetrySnapStats snapToPlane(std::vector<Vector3>* vertices, int axis,
    double plane, double tolerance)
{
    SymmetrySnapStats stats;
    if (nullptr == vertices || vertices->empty() || axis < 1 || axis > 3)
        return stats;
    const size_t axisIndex = (size_t)(axis - 1);
    // A negative or NaN tolerance means "nothing is close enough"; clamping it to
    // zero keeps that meaning instead of turning the comparison into a tautology.
    const double limit = (tolerance > 0.0) ? tolerance : 0.0;
    bool firstSnap = true;
    for (auto& vertex : *vertices) {
        const double distance = std::fabs(vertex[axisIndex] - plane);
        // NaN distances fall out here too, which is what keeps a broken vertex
        // from being written to; the other components are never touched at all.
        if (!(distance <= limit))
            continue;
        vertex[axisIndex] = plane;
        ++stats.snappedVertices;
        ++stats.movedVertices;
        if (firstSnap) {
            stats.maxSnap = distance;
            stats.minSnap = distance;
            firstSnap = false;
        } else {
            stats.maxSnap = std::max(stats.maxSnap, distance);
            stats.minSnap = std::min(stats.minSnap, distance);
        }
    }
    return stats;
}

}
