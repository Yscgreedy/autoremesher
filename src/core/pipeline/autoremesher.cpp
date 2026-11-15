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
#include <AutoRemesher/AutoRemesher>
#include <AutoRemesher/IsotropicRemesher>
#include <AutoRemesher/MeshSeparator>
#include <AutoRemesher/Parameterizer>
#include <AutoRemesher/QuadExtractor>
#include <AutoRemesher/UvCharts>
#include <AutoRemesher/UvTransfer>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <core/base/logging.h>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <queue>
#include <sstream>
// Qt defines `emit` as a macro, which collides with TBB profiling.h's `void emit()`.
// macOS `<mach/mach.h>` also defines `emit`. Undefine before including TBB headers.
#if defined(__APPLE__) || defined(emit)
#undef emit
#endif

// oneAPI TBB (2021+) moved headers under <oneapi/tbb/>. Use __has_include where
// available (clang + GCC) to pick the right path, falling back to the legacy path.
#if defined(__has_include)
#if __has_include(<oneapi/tbb/blocked_range.h>)
#include <oneapi/tbb/blocked_range.h>
#else
#include <tbb/blocked_range.h>
#endif
#if __has_include(<oneapi/tbb/mutex.h>)
#include <oneapi/tbb/mutex.h>
#else
#include <tbb/mutex.h>
#endif
#if __has_include(<oneapi/tbb/parallel_for.h>)
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/parallel_sort.h>
#else
#include <tbb/parallel_for.h>
#include <tbb/parallel_sort.h>
#endif
#else
#include <tbb/blocked_range.h>
#include <tbb/mutex.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_sort.h>
#endif
#include <cfloat>
#include <meshoptimizer.h>
#include <unordered_map>
#include <unordered_set>

namespace AutoRemesher {

namespace {
    const float parallelPhaseBegin = 0.03f;
    const float parallelPhaseEnd = 0.95f;

    // How an island's own 0..1 progress splits across its three stages, from the
    // measured cost of each on a typical model.  The phase report prints the
    // real accumulated times, so these can be re-checked against a run.
    const float islandResampleEnd = 0.17f;
    const float islandParameterizeEnd = 0.50f;
    // Quad extraction runs from islandParameterizeEnd to 1.0.

    const double decimateTriggerRatio = 8.0;
    const double decimateTargetRatio = 4.0;

    void markSharpEdgeVertices(const std::vector<Vector3>& vertices,
        const std::vector<unsigned int>& indices,
        double sharpEdgeRadians,
        std::vector<unsigned char>& vertexLock)
    {
        const size_t faceCount = indices.size() / 3;

        std::vector<Vector3> faceNormals(faceCount);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, faceCount),
            [&](const tbb::blocked_range<size_t>& range) {
                for (size_t i = range.begin(); i != range.end(); ++i) {
                    faceNormals[i] = Vector3::normal(vertices[indices[i * 3 + 0]],
                        vertices[indices[i * 3 + 1]],
                        vertices[indices[i * 3 + 2]]);
                }
            });

        std::vector<std::pair<uint64_t, unsigned int>> edges(faceCount * 3);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, faceCount),
            [&](const tbb::blocked_range<size_t>& range) {
                for (size_t i = range.begin(); i != range.end(); ++i) {
                    for (size_t j = 0; j < 3; ++j) {
                        unsigned int first = indices[i * 3 + j];
                        unsigned int second = indices[i * 3 + (j + 1) % 3];
                        if (first > second)
                            std::swap(first, second);
                        edges[i * 3 + j] = { ((uint64_t)first << 32) | second, (unsigned int)i };
                    }
                }
            });
        // Every (edge, face) pair is distinct, so the parallel sort produces the
        // same order the serial one did.
        tbb::parallel_sort(edges.begin(), edges.end());

        for (size_t i = 0; i + 1 < edges.size(); ++i) {
            if (edges[i].first != edges[i + 1].first)
                continue;
            if (Vector3::angle(faceNormals[edges[i].second],
                    faceNormals[edges[i + 1].second])
                < sharpEdgeRadians)
                continue;
            vertexLock[(size_t)(edges[i].first >> 32)] |= meshopt_SimplifyVertex_Priority;
            vertexLock[(size_t)(edges[i].first & 0xffffffffu)] |= meshopt_SimplifyVertex_Priority;
        }
    }
}

const double AutoRemesher::m_defaultSharpEdgeDegrees = 90;

double AutoRemesher::calculateAverageEdgeLength(const std::vector<Vector3>& vertices,
    const std::vector<std::vector<size_t>>& faces)
{
    double sumOfLength = 0.0;
    size_t edgeCount = 0;
    for (const auto& face : faces) {
        for (size_t i = 0; i < face.size(); ++i) {
            size_t j = (i + 1) % face.size();
            sumOfLength += (vertices[face[i]] - vertices[face[j]]).length();
            ++edgeCount;
        }
    }
    if (0 == edgeCount)
        return 0.0;
    return sumOfLength / edgeCount;
}

void AutoRemesher::initializeVoxelSize()
{
    double area = calculateMeshArea(m_vertices, m_triangles);
    double triangleArea = area / m_targetTriangleCount;
    m_voxelSize = std::sqrt(triangleArea / (0.86602540378 * 0.5));
#if AUTO_REMESHER_DEBUG
    std::cerr << "Area: " << area << " voxelSize: " << m_voxelSize << std::endl;
#endif
}

double AutoRemesher::calculateMeshArea(const std::vector<Vector3>& vertices,
    const std::vector<std::vector<size_t>>& triangles)
{
    double area = 0.0;
    for (const auto& it : triangles) {
        area += Vector3::area(vertices[it[0]], vertices[it[1]], vertices[it[2]]);
    }
    return area;
}

bool AutoRemesher::decimateIfTooDense(std::vector<Vector3>& vertices,
    std::vector<std::vector<size_t>>& triangles,
    double voxelSize,
    double sharpEdgeDegrees,
    size_t islandIndex,
    DecimationStats* stats)
{
    if (nullptr != stats)
        ++stats->islandsConsidered;

    if (vertices.empty() || triangles.empty() || voxelSize <= 0.0)
        return false;

    if (vertices.size() > (size_t)std::numeric_limits<unsigned int>::max())
        return false;

    const double targetTriangleArea = voxelSize * voxelSize * 0.86602540378 * 0.5;
    if (targetTriangleArea <= 0.0)
        return false;
    const double islandTargetTriangleCount = calculateMeshArea(vertices, triangles) / targetTriangleArea;
    if (islandTargetTriangleCount < 1.0)
        return false;

    if ((double)triangles.size() < islandTargetTriangleCount * decimateTriggerRatio)
        return false;

    const size_t decimateTriangleCount = (size_t)(islandTargetTriangleCount * decimateTargetRatio);

    std::vector<unsigned int> indices;
    indices.reserve(triangles.size() * 3);
    for (const auto& triangle : triangles) {
        if (3 != triangle.size())
            return false;
        for (size_t i = 0; i < 3; ++i)
            indices.push_back((unsigned int)triangle[i]);
    }

    Vector3 lowerBound = vertices.front();
    Vector3 upperBound = vertices.front();
    for (const auto& position : vertices) {
        for (size_t i = 0; i < 3; ++i) {
            lowerBound[i] = std::min(lowerBound[i], position[i]);
            upperBound[i] = std::max(upperBound[i], position[i]);
        }
    }
    const Vector3 center = (lowerBound + upperBound) * 0.5;

    std::vector<float> positions;
    positions.reserve(vertices.size() * 3);
    for (const auto& position : vertices) {
        positions.push_back((float)(position.x() - center.x()));
        positions.push_back((float)(position.y() - center.y()));
        positions.push_back((float)(position.z() - center.z()));
    }

    std::vector<unsigned int> remap(vertices.size());
    const size_t weldedVertexCount = meshopt_generateVertexRemap(remap.data(),
        indices.data(), indices.size(),
        positions.data(), vertices.size(), sizeof(float) * 3);
    std::vector<Vector3> weldedVertices(weldedVertexCount);
    std::vector<float> weldedPositions(weldedVertexCount * 3);
    meshopt_remapIndexBuffer(indices.data(), indices.data(), indices.size(), remap.data());
    meshopt_remapVertexBuffer(weldedPositions.data(), positions.data(),
        vertices.size(), sizeof(float) * 3, remap.data());
    for (size_t i = 0; i < vertices.size(); ++i) {
        if (~0u != remap[i])
            weldedVertices[remap[i]] = vertices[i];
    }

    std::vector<unsigned char> vertexLock;
    if (sharpEdgeDegrees > 0.0) {
        vertexLock.assign(weldedVertexCount, 0);
        markSharpEdgeVertices(weldedVertices, indices,
            sharpEdgeDegrees * (M_PI / 180.0), vertexLock);
    }

    std::vector<unsigned int> decimated(indices.size());
    float resultError = 0.0f;
    decimated.resize(meshopt_simplifyWithAttributes(decimated.data(),
        indices.data(), indices.size(),
        weldedPositions.data(), weldedVertexCount, sizeof(float) * 3,
        nullptr, 0, nullptr, 0,
        vertexLock.empty() ? nullptr : vertexLock.data(),
        decimateTriangleCount * 3, FLT_MAX, meshopt_SimplifyRegularize, &resultError));

    if (decimated.size() < 3 || decimated.size() >= indices.size())
        return false;

    std::vector<size_t> outputIndexOfWelded(weldedVertexCount, std::numeric_limits<size_t>::max());
    std::vector<Vector3> decimatedVertices;
    std::vector<std::vector<size_t>> decimatedTriangles;
    decimatedTriangles.reserve(decimated.size() / 3);
    for (size_t i = 0; i + 2 < decimated.size(); i += 3) {
        std::vector<size_t> triangle(3);
        for (size_t j = 0; j < 3; ++j) {
            const unsigned int weldedIndex = decimated[i + j];
            if (std::numeric_limits<size_t>::max() == outputIndexOfWelded[weldedIndex]) {
                outputIndexOfWelded[weldedIndex] = decimatedVertices.size();
                decimatedVertices.push_back(weldedVertices[weldedIndex]);
            }
            triangle[j] = outputIndexOfWelded[weldedIndex];
        }
        decimatedTriangles.push_back(triangle);
    }

    if (nullptr != stats) {
        ++stats->islandsDecimated;
        stats->trianglesBefore += triangles.size();
        stats->trianglesAfter += decimatedTriangles.size();
    }

#if AUTO_REMESHER_DEBUG
    std::cerr << "Island[" << islandIndex << "]: Decimated " << triangles.size()
              << " triangles to " << decimatedTriangles.size()
              << " (target " << decimateTriangleCount
              << ", island target " << (size_t)islandTargetTriangleCount
              << "), normalized error: " << resultError << std::endl;
#else
    (void)islandIndex;
    (void)resultError;
#endif

    vertices = std::move(decimatedVertices);
    triangles = std::move(decimatedTriangles);
    return true;
}

void AutoRemesher::resample(std::vector<Vector3>& vertices,
    std::vector<std::vector<size_t>>& triangles,
    double voxelSize,
    double adaptivity,
    double sharpEdgeDegrees,
    double smoothNormalDegrees,
    size_t islandIndex,
    DecimationStats* decimationStats,
    std::atomic<long long>* adaptiveFieldTimeUs,
    const ProgressHandler* progressHandler,
    std::vector<Vector3>* decimatedVerticesOut,
    std::vector<std::vector<size_t>>* decimatedTrianglesOut)
{
    auto t_decimateStart = std::chrono::high_resolution_clock::now();
    decimateIfTooDense(vertices, triangles, voxelSize, sharpEdgeDegrees, islandIndex, decimationStats);
    if (nullptr != decimationStats) {
        decimationStats->timeUs += std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - t_decimateStart)
                                       .count();
    }

    if (nullptr != decimatedVerticesOut)
        *decimatedVerticesOut = vertices;
    if (nullptr != decimatedTrianglesOut)
        *decimatedTrianglesOut = triangles;

    auto t_fieldStart = std::chrono::high_resolution_clock::now();
    std::vector<double> vertexTargetLengths;
    if (adaptivity > 0.0 && !vertices.empty()) {
        // A target-length field redistributes the uniform triangle budget.  The
        // field is deliberately computed on the input mesh: IsotropicRemesher
        // propagates it to vertices created by edge splits.
        const double minRatio = 0.35;
        const double maxRatio = 3.0;
        const double epsilon = 1e-12;

        // Do not add face normals directly from parallel workers: adjacent
        // faces write to the same vertex.  Compute faces in parallel, then do
        // the small accumulation pass serially.
        std::vector<Vector3> faceNormals(triangles.size());
        std::vector<double> faceAreas(triangles.size(), 0.0);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, triangles.size()),
            [&](const tbb::blocked_range<size_t>& range) {
                for (size_t i = range.begin(); i != range.end(); ++i) {
                    const auto& tri = triangles[i];
                    faceAreas[i] = Vector3::area(
                        vertices[tri[0]], vertices[tri[1]], vertices[tri[2]]);
                    if (faceAreas[i] > epsilon)
                        faceNormals[i] = Vector3::normal(
                            vertices[tri[0]], vertices[tri[1]], vertices[tri[2]]);
                }
            });

        std::vector<Vector3> normals(vertices.size());
        std::vector<std::vector<size_t>> neighbors(vertices.size());
        for (size_t i = 0; i < triangles.size(); ++i) {
            const auto& tri = triangles[i];
            if (faceAreas[i] <= epsilon)
                continue;
            const Vector3 weightedNormal = faceNormals[i] * faceAreas[i];
            for (size_t j = 0; j < 3; ++j) {
                normals[tri[j]] += weightedNormal;
                neighbors[tri[j]].push_back(tri[(j + 1) % 3]);
                neighbors[tri[j]].push_back(tri[(j + 2) % 3]);
            }
        }
        tbb::parallel_for(tbb::blocked_range<size_t>(0, normals.size()),
            [&](const tbb::blocked_range<size_t>& range) {
                for (size_t i = range.begin(); i != range.end(); ++i) {
                    normals[i].normalize();
                    auto& ring = neighbors[i];
                    std::sort(ring.begin(), ring.end());
                    ring.erase(std::unique(ring.begin(), ring.end()), ring.end());
                }
            });

        // Mean normal variation per unit length is less sensitive to a single
        // bad triangle than the previous maximum-one-ring estimate.
        std::vector<double> vertexCurvature(vertices.size(), 0.0);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, vertices.size()),
            [&](const tbb::blocked_range<size_t>& range) {
                for (size_t v = range.begin(); v != range.end(); ++v) {
                    const auto& ring = neighbors[v];
                    if (ring.empty() || normals[v].lengthSquared() <= epsilon)
                        continue;
                    double weightedCurvature = 0.0;
                    double totalWeight = 0.0;
                    for (const auto& u : ring) {
                        const double length = (vertices[u] - vertices[v]).length();
                        if (length <= epsilon || normals[u].lengthSquared() <= epsilon)
                            continue;
                        double cosine = Vector3::dotProduct(normals[v], normals[u]);
                        cosine = std::max(-1.0, std::min(1.0, cosine));
                        weightedCurvature += std::acos(cosine);
                        totalWeight += length;
                    }
                    if (totalWeight > epsilon)
                        vertexCurvature[v] = weightedCurvature / totalWeight;
                }
            });

        // A percentile reference prevents a few very sharp/noisy vertices from
        // making the rest of the surface appear flat.
        std::vector<double> nonZeroCurvatures;
        nonZeroCurvatures.reserve(vertexCurvature.size());
        for (double curvature : vertexCurvature) {
            if (curvature > epsilon)
                nonZeroCurvatures.push_back(curvature);
        }
        if (!nonZeroCurvatures.empty()) {
            const size_t referenceIndex = (nonZeroCurvatures.size() - 1) * 3 / 4;
            std::nth_element(nonZeroCurvatures.begin(),
                nonZeroCurvatures.begin() + referenceIndex, nonZeroCurvatures.end());
            const double curvatureReference = nonZeroCurvatures[referenceIndex];
            vertexTargetLengths.resize(vertices.size());
            std::vector<double> importance(vertices.size(), 1.0);
            const double strength = std::min(adaptivity, 2.0) * 7.0;
            tbb::parallel_for(tbb::blocked_range<size_t>(0, vertices.size()),
                [&](const tbb::blocked_range<size_t>& range) {
                    for (size_t v = range.begin(); v != range.end(); ++v) {
                        const double normalized = std::min(4.0,
                            vertexCurvature[v] / std::max(curvatureReference, epsilon));
                        importance[v] += strength * normalized * normalized;
                    }
                });

            // Keep integral(area / h^2) equal to the uniform field, which
            // preserves the budget implied by voxelSize while moving triangles
            // from flat regions to detailed ones.
            double totalArea = 0.0;
            double weightedImportance = 0.0;
            for (size_t i = 0; i < triangles.size(); ++i) {
                const auto& tri = triangles[i];
                totalArea += faceAreas[i];
                weightedImportance += faceAreas[i] * (importance[tri[0]] + importance[tri[1]] + importance[tri[2]]) / 3.0;
            }
            if (totalArea > epsilon) {
                const double averageImportance = weightedImportance / totalArea;
                for (size_t v = 0; v < vertices.size(); ++v) {
                    double multiplier = std::sqrt(averageImportance / importance[v]);
                    multiplier = std::max(minRatio, std::min(maxRatio, multiplier));
                    vertexTargetLengths[v] = voxelSize * multiplier;
                }
            } else {
                vertexTargetLengths.clear();
            }
        }
    }
    if (nullptr != adaptiveFieldTimeUs) {
        *adaptiveFieldTimeUs += std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - t_fieldStart)
                                    .count();
    }

#if AUTO_REMESHER_DEBUG
    std::cerr << "Island[" << islandIndex << "]: Uniformly remeshing on target edge length: " << voxelSize << std::endl;
#endif
    IsotropicRemesher isotropicRemesher(vertices, triangles);
    if (nullptr != progressHandler && *progressHandler)
        isotropicRemesher.setProgressHandler(*progressHandler);
    isotropicRemesher.setTargetEdgeLength(voxelSize);
    if (!vertexTargetLengths.empty())
        isotropicRemesher.setVertexTargetEdgeLengths(&vertexTargetLengths);
    isotropicRemesher.setSharpEdgeDegrees(sharpEdgeDegrees);
    isotropicRemesher.setSmoothNormalDegrees(smoothNormalDegrees);
    isotropicRemesher.remesh();
    vertices = isotropicRemesher.remeshedVertices();
    triangles = isotropicRemesher.remeshedTriangles();
#if AUTO_REMESHER_DEBUG
    std::cerr << "Island[" << islandIndex << "]: Uniformly remesh done, vertex count: " << vertices.size() << " triangle count: " << triangles.size() << std::endl;
#endif
}

void AutoRemesher::updateProgress(size_t threadIndex, float progress, const char* status)
{
    if (nullptr == m_progressHandler)
        return;

    std::lock_guard<std::mutex> lock(m_progressMutex);
    if (threadIndex >= m_threadProgress.size())
        return;
    if (nullptr != status && '\0' != status[0])
        m_threadStatus[threadIndex] = status;
    if (progress > m_threadProgress[threadIndex]) {
        m_progressSum += (double)(progress - m_threadProgress[threadIndex])
            * m_threadProgressWeights[threadIndex];
        m_threadProgress[threadIndex] = progress;
    }

    const double overall = parallelPhaseBegin
        + (parallelPhaseEnd - parallelPhaseBegin) * std::min(1.0, std::max(0.0, m_progressSum));

    // Steps now report many times per island, so only wake the UI when the bar
    // would actually move or the status line would change.
    const int permille = (int)(overall * 1000.0);
    const char* islandStatus = m_threadStatus[threadIndex];
    if (permille == m_reportedPermille && islandStatus == m_reportedStatus)
        return;
    m_reportedPermille = permille;
    m_reportedStatus = islandStatus;

    // With several islands in flight, the run as a whole is only as far along as
    // its slowest island, so that is the step worth naming.  Finding it costs a
    // scan over every island, and a 17,958 island model reports hundreds of
    // thousands of times, so the scan is rate limited: the percentage is always
    // exact, only the label can lag by a frame.
    const auto now = std::chrono::steady_clock::now();
    if (m_slowestIsland == m_threadProgress.size()
        || now - m_lastSlowestScan >= std::chrono::milliseconds(20)) {
        m_lastSlowestScan = now;
        m_slowestIsland = threadIndex;
        for (size_t i = 0; i < m_threadProgress.size(); ++i) {
            if (m_threadProgress[i] < m_threadProgress[m_slowestIsland])
                m_slowestIsland = i;
        }
    }
    const char* name = m_threadStatus[m_slowestIsland];
    m_progressHandler(m_tag, (float)overall, nullptr != name ? name : "");
}

ProgressHandler AutoRemesher::makeStageProgress(size_t islandIndex, float begin, float end, float stageOrder)
{
    return [this, islandIndex, begin, end, stageOrder,
               lastTime = std::chrono::high_resolution_clock::now(),
               lastName = (const char*)nullptr,
               lastOrder = 0.0f](float fraction, const char* name) mutable {
        const auto now = std::chrono::high_resolution_clock::now();
        if (nullptr != lastName) {
            accumulateStageTime(lastName, lastOrder,
                std::chrono::duration_cast<std::chrono::microseconds>(now - lastTime).count());
        }
        lastTime = now;
        lastName = name;
        lastOrder = stageOrder + fraction;
        updateProgress(islandIndex, begin + (end - begin) * fraction, name);
    };
}

void AutoRemesher::accumulateStageTime(const char* name, float order, long long microseconds)
{
    if (nullptr == name || '\0' == name[0])
        return;
    std::lock_guard<std::mutex> lock(m_stageTimingMutex);
    for (auto& it : m_stageTimes) {
        if (it.name == name) {
            it.microseconds += microseconds;
            return;
        }
    }
    m_stageTimes.push_back({ name, order, microseconds });
}

bool AutoRemesher::remesh()
{
    // Validate inputs before any sizing math. In particular a zero target
    // triangle count would divide by zero in initializeVoxelSize().
    const char* invalidInputReason = nullptr;
    if (m_vertices.empty())
        invalidInputReason = "input mesh has no vertices";
    else if (m_triangles.empty())
        invalidInputReason = "input mesh has no triangles";
    else if (0 == m_targetTriangleCount)
        invalidInputReason = "target triangle count must be greater than zero";
    if (nullptr != invalidInputReason) {
        m_lastError = invalidInputReason;
        Log::write(Log::Normal, std::string("Invalid remesh input: ") + invalidInputReason + "\n");
        if (nullptr != m_progressHandler)
            m_progressHandler(m_tag, 1.0, invalidInputReason);
        return false;
    }
    auto t_start = std::chrono::high_resolution_clock::now();

    // Describe the mesh before touching it.  Boundary edges, non-manifold edges,
    // isolated shells and degenerate faces are exactly what makes a remesh go
    // wrong quietly, so they are counted here and reported by the application
    // instead of being discovered in Blender afterwards.
    analyzeInput();

    // Each label names the step that is about to run, not the one that just
    // finished, so the status line matches what the process is actually doing.
    if (nullptr != m_progressHandler)
        m_progressHandler(m_tag, 0.0f, "Computing voxel size");
    auto t_voxelStart = std::chrono::high_resolution_clock::now();
    initializeVoxelSize();
    auto t_voxelEnd = std::chrono::high_resolution_clock::now();

    if (nullptr != m_progressHandler)
        m_progressHandler(m_tag, 0.01f, "Splitting mesh into islands");
    std::vector<std::vector<std::vector<size_t>>> trianglesIslands;
    auto t_splitStart = std::chrono::high_resolution_clock::now();
    MeshSeparator::splitToIslands(m_triangles, trianglesIslands);
    auto t_afterSplit = std::chrono::high_resolution_clock::now();

    m_islandCount = trianglesIslands.size();

    if (trianglesIslands.empty()) {
        m_lastError = "input mesh produced no islands";
        if (nullptr != m_progressHandler)
            m_progressHandler(m_tag, 1.0, "Input mesh is empty");
        return false;
    }

#if AUTO_REMESHER_DEBUG
    std::cerr << "Split to islands: " << trianglesIslands.size() << std::endl;
#endif

    struct IslandContext {
        std::vector<Vector3> vertices;
        std::vector<std::vector<size_t>> triangles;
        double voxelSize;
        double scaling;
        double adaptivity;
        double anisotropy;
        double sharpEdgeDegrees;
        double smoothNormalDegrees;
        // Share of the requested quad budget, filled in once every island has
        // been resampled; 0 means "keep the legacy scale".
        size_t targetQuads = 0;
    };

    if (nullptr != m_progressHandler)
        m_progressHandler(m_tag, 0.02f, "Building island contexts");
    // Islands are compacted independently of each other, and writing into a
    // pre-sized vector by index keeps them in the original order.
    std::vector<IslandContext> islandContexes(trianglesIslands.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, trianglesIslands.size()),
        [&](const tbb::blocked_range<size_t>& range) {
            for (size_t islandIndex = range.begin(); islandIndex != range.end(); ++islandIndex) {
                const auto& island = trianglesIslands[islandIndex];
                IslandContext& context = islandContexes[islandIndex];
                context.triangles.reserve(island.size());
                std::unordered_map<size_t, size_t> oldToNewVertexMap;
                oldToNewVertexMap.reserve(island.size() * 2);
                for (const auto& face : island) {
                    std::vector<size_t> triangle;
                    triangle.reserve(3);
                    for (size_t i = 0; i < 3; ++i) {
                        auto insertResult = oldToNewVertexMap.insert({ face[i], context.vertices.size() });
                        if (insertResult.second)
                            context.vertices.push_back(m_vertices[face[i]]);
                        triangle.push_back(insertResult.first->second);
                    }
                    context.triangles.push_back(std::move(triangle));
                }

                context.scaling = m_scaling;
                context.voxelSize = m_voxelSize;
                context.adaptivity = m_adaptivity;
                context.anisotropy = m_anisotropy;
                context.sharpEdgeDegrees = m_sharpEdgeDegrees;
                context.smoothNormalDegrees = m_smoothNormalDegrees;
            }
        });
    auto t_buildEnd = std::chrono::high_resolution_clock::now();
    if (nullptr != m_progressHandler)
        m_progressHandler(m_tag, parallelPhaseBegin, "Remeshing uniformly");

    std::atomic<long long> resampleTime(0);
    std::atomic<long long> adaptiveFieldTime(0);
    DecimationStats decimationStats;

    {
        m_threadProgressWeights.assign(islandContexes.size(), 1.0f);
        for (size_t i = 0; i < islandContexes.size(); ++i) {
            if (!m_triangles.empty())
                m_threadProgressWeights[i] = (float)(((double)islandContexes[i].triangles.size() / m_triangles.size()));
        }
        m_threadProgress.assign(islandContexes.size(), 0.0f);
        m_threadStatus.assign(islandContexes.size(), nullptr);
        m_progressSum = 0.0;

        struct IsotropicPhase {
            IsotropicPhase(std::vector<IslandContext>* contexts,
                AutoRemesher* remesher,
                std::atomic<long long>* resampleTime,
                std::atomic<long long>* adaptiveFieldTime,
                DecimationStats* decimationStats,
                std::vector<std::vector<Vector3>>* islandVertices,
                std::vector<std::vector<std::vector<size_t>>>* islandTriangles,
                std::vector<std::vector<Vector3>>* decimatedIslandVertices,
                std::vector<std::vector<std::vector<size_t>>>* decimatedIslandTriangles)
                : m_contexts(contexts)
                , m_remesher(remesher)
                , m_resampleTime(resampleTime)
                , m_adaptiveFieldTime(adaptiveFieldTime)
                , m_decimationStats(decimationStats)
                , m_islandVertices(islandVertices)
                , m_islandTriangles(islandTriangles)
                , m_decimatedIslandVertices(decimatedIslandVertices)
                , m_decimatedIslandTriangles(decimatedIslandTriangles)
            {
            }

            void operator()(const tbb::blocked_range<size_t>& range) const
            {
                for (size_t i = range.begin(); i != range.end(); ++i) {
                    auto& ctx = (*m_contexts)[i];

                    m_remesher->updateProgress(i, 0.0f, "Remeshing uniformly");
                    const ProgressHandler isotropicProgress = m_remesher->makeStageProgress(i,
                        0.0f, islandResampleEnd, -1.0f);

                    auto t0 = std::chrono::high_resolution_clock::now();
                    resample(ctx.vertices, ctx.triangles, ctx.voxelSize, ctx.adaptivity, ctx.sharpEdgeDegrees, ctx.smoothNormalDegrees, i, m_decimationStats,
                        m_adaptiveFieldTime, &isotropicProgress,
                        &(*m_decimatedIslandVertices)[i], &(*m_decimatedIslandTriangles)[i]);
                    auto t1 = std::chrono::high_resolution_clock::now();
                    *m_resampleTime += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

                    (*m_islandVertices)[i] = ctx.vertices;
                    (*m_islandTriangles)[i] = ctx.triangles;

                    m_remesher->updateProgress(i, islandResampleEnd);
                }
            }

        private:
            std::vector<IslandContext>* m_contexts = nullptr;
            AutoRemesher* m_remesher = nullptr;
            std::atomic<long long>* m_resampleTime = nullptr;
            std::atomic<long long>* m_adaptiveFieldTime = nullptr;
            DecimationStats* m_decimationStats = nullptr;
            std::vector<std::vector<Vector3>>* m_islandVertices = nullptr;
            std::vector<std::vector<std::vector<size_t>>>* m_islandTriangles = nullptr;
            std::vector<std::vector<Vector3>>* m_decimatedIslandVertices = nullptr;
            std::vector<std::vector<std::vector<size_t>>>* m_decimatedIslandTriangles = nullptr;
        };

        auto mergeIslands = [](const std::vector<std::vector<Vector3>>& islandVertices,
                                const std::vector<std::vector<std::vector<size_t>>>& islandTriangles,
                                std::vector<Vector3>& mergedVertices,
                                std::vector<std::vector<size_t>>& mergedTriangles) {
            for (size_t i = 0; i < islandVertices.size(); ++i) {
                const size_t vertexOffset = mergedVertices.size();
                mergedVertices.insert(mergedVertices.end(),
                    islandVertices[i].begin(), islandVertices[i].end());
                for (const auto& triangle : islandTriangles[i]) {
                    std::vector<size_t> offsetTriangle;
                    offsetTriangle.reserve(triangle.size());
                    for (const size_t index : triangle)
                        offsetTriangle.push_back(index + vertexOffset);
                    mergedTriangles.push_back(std::move(offsetTriangle));
                }
            }
        };

        m_isotropicVertices.clear();
        m_isotropicTriangles.clear();
        m_decimatedVertices.clear();
        m_decimatedTriangles.clear();
        std::vector<std::vector<Vector3>> isotropicIslandVertices(islandContexes.size());
        std::vector<std::vector<std::vector<size_t>>> isotropicIslandTriangles(islandContexes.size());
        std::vector<std::vector<Vector3>> decimatedIslandVertices(islandContexes.size());
        std::vector<std::vector<std::vector<size_t>>> decimatedIslandTriangles(islandContexes.size());
        tbb::parallel_for(tbb::blocked_range<size_t>(0, islandContexes.size()),
            IsotropicPhase(&islandContexes, this, &resampleTime, &adaptiveFieldTime,
                &decimationStats,
                &isotropicIslandVertices, &isotropicIslandTriangles,
                &decimatedIslandVertices, &decimatedIslandTriangles));
        mergeIslands(isotropicIslandVertices, isotropicIslandTriangles,
            m_isotropicVertices, m_isotropicTriangles);
        m_decimated = decimationStats.islandsDecimated.load() > 0;
        if (m_decimated) {
            mergeIslands(decimatedIslandVertices, decimatedIslandTriangles,
                m_decimatedVertices, m_decimatedTriangles);
        }
    }
    auto t_isotropicEnd = std::chrono::high_resolution_clock::now();

    class ParameterizationThread {
    public:
        size_t islandIndex = 0;
        IslandContext* island = nullptr;
        std::unique_ptr<Parameterizer> parameterizer;
        std::unique_ptr<QuadExtractor> remesher;
        AutoRemesher* autoRemesher = nullptr;
        std::vector<std::vector<Vector2>> capturedUvs;
        std::vector<std::vector<Vector2>> capturedOriginalUvs;
        std::vector<uint8_t> capturedExtractedConnectionMoved;
        std::vector<Vector3> capturedSingularVertices;
        std::vector<size_t> capturedSingularVertexIndices;
        std::vector<std::pair<Vector3, Vector3>> capturedExtractedConnections;
    };

    if (m_exactTargetQuads && m_targetTriangleCount > 0) {
        const size_t totalTargetQuads = std::max<size_t>(1, m_targetTriangleCount / 2);
        std::vector<double> islandAreas(islandContexes.size(), 0.0);
        double totalArea = 0.0;
        for (size_t i = 0; i < islandContexes.size(); ++i) {
            islandAreas[i] = calculateMeshArea(islandContexes[i].vertices, islandContexes[i].triangles);
            totalArea += islandAreas[i];
        }
        if (totalArea > 0.0) {
            for (size_t i = 0; i < islandContexes.size(); ++i) {
                const double share = islandAreas[i] / totalArea;
                size_t target = (size_t)std::llround((double)totalTargetQuads * share);
                // A visible island with a sub-quad share still deserves one
                // quad; without the floor the smallest parts silently vanish.
                if (0 == target && islandAreas[i] > 0.0)
                    target = 1;
                islandContexes[i].targetQuads = target;
            }
        }
    }

    std::vector<ParameterizationThread> parameterizationThreads(islandContexes.size());
    for (size_t i = 0; i < islandContexes.size(); ++i) {
        auto& thread = parameterizationThreads[i];
        auto& context = islandContexes[i];
        thread.islandIndex = i;
        thread.island = &context;
        thread.autoRemesher = this;
    }

    class SurfaceParameterizer {
    public:
        SurfaceParameterizer(std::vector<ParameterizationThread>* parameterizationThreads,
            std::atomic<long long>* parameterizeTime,
            std::atomic<long long>* extractTime)
            : m_parameterizationThreads(parameterizationThreads)
            , m_parameterizeTime(parameterizeTime)
            , m_extractTime(extractTime)
        {
        }

        void operator()(const tbb::blocked_range<size_t>& range) const
        {
            for (size_t i = range.begin(); i != range.end(); ++i) {
                auto& thread = (*m_parameterizationThreads)[i];

                auto t0 = std::chrono::high_resolution_clock::now();

                const auto& vertices = thread.island->vertices;
                const auto& triangles = thread.island->triangles;

                if (vertices.empty() || triangles.empty()) {
                    // Still retire the island, otherwise its share of the bar
                    // is never filled in and the total stalls short of the end.
                    thread.autoRemesher->updateProgress(thread.islandIndex, 1.0f);
                    continue;
                }

                thread.autoRemesher->updateProgress(thread.islandIndex, islandResampleEnd);
                thread.parameterizer = std::make_unique<Parameterizer>(&vertices,
                    &triangles,
                    nullptr);
                thread.parameterizer->setProgressHandler(
                    thread.autoRemesher->makeStageProgress(thread.islandIndex,
                        islandResampleEnd, islandParameterizeEnd, 0.0f));
                if (thread.island->scaling > 0.0)
                    thread.parameterizer->setScaling(thread.island->scaling);
                thread.parameterizer->setGradientAdaptivity(thread.island->adaptivity);
                thread.parameterizer->setAnisotropy(thread.island->anisotropy);
                thread.parameterizer->setSharpEdgeDegrees(thread.island->sharpEdgeDegrees);
                thread.parameterizer->setTargetQuadCount(thread.island->targetQuads);
                bool parameterizeSucceeded = true;
                try {
                    parameterizeSucceeded = thread.parameterizer->parameterize();
                } catch (const std::exception& e) {
                    // A pathological island must not abort the whole remesh,
                    // so log the parameterizer failure and skip its quads.
                    parameterizeSucceeded = false;
                    std::ostringstream message;
                    message << "Island " << (thread.islandIndex + 1)
                            << ": parameterization failed (" << e.what()
                            << "), skipping this island.\n";
                    Log::write(Log::Normal, message.str());
                } catch (...) {
                    parameterizeSucceeded = false;
                    std::ostringstream message;
                    message << "Island " << (thread.islandIndex + 1)
                            << ": parameterization failed (unknown error), skipping this island.\n";
                    Log::write(Log::Normal, message.str());
                }

                auto t1 = std::chrono::high_resolution_clock::now();
                *m_parameterizeTime += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

                if (parameterizeSucceeded) {
                    thread.autoRemesher->updateProgress(thread.islandIndex, islandParameterizeEnd);
                    std::unique_ptr<std::vector<std::vector<Vector2>>> uvs = thread.parameterizer->takeTriangleUvs();
                    if (uvs) {
                        // Save a copy of UVs for the [param] preview overlay
                        thread.capturedUvs = *uvs;
                        thread.capturedOriginalUvs = thread.parameterizer->originalTriangleUvs();
                    }
                    // Capture singular vertex positions for the [param] preview
                    thread.capturedSingularVertices = thread.parameterizer->singularVertexPositions();
                    thread.capturedSingularVertexIndices = thread.parameterizer->singularVertexIndices();
                    thread.remesher = std::make_unique<QuadExtractor>(&vertices,
                        &triangles,
                        uvs.get());
                    thread.remesher->setOriginalTriangleUvs(&thread.capturedOriginalUvs);
                    thread.remesher->setSingularVertices(&thread.capturedSingularVertexIndices);
                    if (thread.autoRemesher->m_islandAreaFraction > 0.0)
                        thread.remesher->setIslandAreaFraction(thread.autoRemesher->m_islandAreaFraction);
                    if (thread.autoRemesher->m_weldPatches)
                        thread.remesher->setWeldPatches(true);
                    if (thread.autoRemesher->m_gradedHoleFilling)
                        thread.remesher->setGradedHoleFilling(true);
                    if (thread.autoRemesher->m_projectedHoleCaps)
                        thread.remesher->setProjectedHoleCaps(true);
                    if (thread.autoRemesher->m_localRemesh)
                        thread.remesher->setLocalRemesh(true);
                    if (thread.autoRemesher->m_relativePositionTolerance) {
                        // One thousandth of the island's target edge length: fine
                        // enough to keep distinct cross points apart, coarse enough
                        // to absorb the last-bit differences between the two
                        // triangles that computed the same intersection.
                        const double tolerance = thread.island->voxelSize * 1e-3;
                        if (tolerance > 0.0)
                            thread.remesher->setPositionTolerance(tolerance);
                    }
                    thread.remesher->setProgressHandler(
                        thread.autoRemesher->makeStageProgress(thread.islandIndex,
                            islandParameterizeEnd, 1.0f, 1.0f));
                    if (!thread.remesher->extract()) {
                        thread.remesher.reset();
                    } else {
                        thread.capturedExtractedConnections = thread.remesher->extractedConnections();
                        thread.capturedExtractedConnectionMoved = thread.remesher->extractedConnectionMoved();
                    }
                }
                thread.autoRemesher->updateProgress(thread.islandIndex, 1.0f);
                auto t2 = std::chrono::high_resolution_clock::now();
                *m_extractTime += std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();
            }
        }

    private:
        std::vector<ParameterizationThread>* m_parameterizationThreads = nullptr;
        std::atomic<long long>* m_parameterizeTime = nullptr;
        std::atomic<long long>* m_extractTime = nullptr;
    };
    std::atomic<long long> parameterizeTimeAccumulated(0);
    std::atomic<long long> extractTimeAccumulated(0);

    tbb::parallel_for(tbb::blocked_range<size_t>(0, parameterizationThreads.size()),
        SurfaceParameterizer(&parameterizationThreads,
            &parameterizeTimeAccumulated,
            &extractTimeAccumulated));
    auto t_parallelEnd = std::chrono::high_resolution_clock::now();

    if (nullptr != m_progressHandler)
        m_progressHandler(m_tag, parallelPhaseEnd, "Merging mesh islands");

    // Merge isotropic UVs from all islands (for [param] preview)
    m_isotropicTriangleUvs.clear();
    m_isotropicOriginalTriangleUvs.clear();
    for (size_t i = 0; i < parameterizationThreads.size(); ++i) {
        auto& thread = parameterizationThreads[i];
        if (thread.capturedUvs.empty())
            continue;
        m_isotropicTriangleUvs.insert(m_isotropicTriangleUvs.end(),
            thread.capturedUvs.begin(), thread.capturedUvs.end());
        m_isotropicOriginalTriangleUvs.insert(m_isotropicOriginalTriangleUvs.end(),
            thread.capturedOriginalUvs.begin(), thread.capturedOriginalUvs.end());
    }

    // Merge singular vertex positions from all islands (for [param] preview)
    m_isotropicSingularVertices.clear();
    for (size_t i = 0; i < parameterizationThreads.size(); ++i) {
        auto& thread = parameterizationThreads[i];
        if (thread.capturedSingularVertices.empty())
            continue;
        m_isotropicSingularVertices.insert(m_isotropicSingularVertices.end(),
            thread.capturedSingularVertices.begin(), thread.capturedSingularVertices.end());
    }

    // Merge the raw quad-extraction connections for the [param] preview.
    m_isotropicExtractedConnections.clear();
    m_isotropicExtractedConnectionMoved.clear();
    for (const auto& thread : parameterizationThreads) {
        m_isotropicExtractedConnections.insert(m_isotropicExtractedConnections.end(),
            thread.capturedExtractedConnections.begin(), thread.capturedExtractedConnections.end());
        m_isotropicExtractedConnectionMoved.resize(m_isotropicExtractedConnections.size()
                - thread.capturedExtractedConnections.size(),
            0);
        m_isotropicExtractedConnectionMoved.insert(m_isotropicExtractedConnectionMoved.end(),
            thread.capturedExtractedConnectionMoved.begin(),
            thread.capturedExtractedConnectionMoved.end());
        m_isotropicExtractedConnectionMoved.resize(m_isotropicExtractedConnections.size(), 0);
    }
    // Signed area of a face in UV space; also the guard that keeps a degenerate
    // chart from collapsing a face.
    const auto polygonUvArea = [](const std::vector<Vector2>& faceUvs) {
        double area = 0.0;
        for (size_t i = 0; i < faceUvs.size(); ++i) {
            const Vector2& current = faceUvs[i];
            const Vector2& next = faceUvs[(i + 1) % faceUvs.size()];
            area += current.x() * next.y() - next.x() * current.y();
        }
        return 0.5 * area;
    };
    // Safety net for a face whose chart is unusable: project it onto its own
    // tangent plane.  Scale is local to the face, so such faces do not tile with
    // the chart, but they are usable instead of zero-area.
    const auto planarFaceUvs = [](const std::vector<Vector3>& vertices, const std::vector<size_t>& face) -> std::vector<Vector2> {
        std::vector<Vector2> result;
        if (face.size() < 3)
            return result;
        Vector3 origin = vertices[face[0]];
        Vector3 normal;
        for (size_t i = 0; i < face.size(); ++i) {
            const Vector3& current = vertices[face[i]];
            const Vector3& following = vertices[face[(i + 1) % face.size()]];
            normal = normal + Vector3((current.y() - following.y()) * (current.z() + following.z()), (current.z() - following.z()) * (current.x() + following.x()), (current.x() - following.x()) * (current.y() + following.y()));
        }
        if (!(normal.lengthSquared() > 0.0))
            return result;
        normal = normal.normalized();
        Vector3 axisU = vertices[face[1]] - origin;
        if (!(axisU.lengthSquared() > 0.0))
            return result;
        axisU = axisU.normalized();
        const Vector3 axisV = Vector3::crossProduct(normal, axisU);
        result.reserve(face.size());
        for (const size_t corner : face) {
            const Vector3 relative = vertices[corner] - origin;
            result.push_back(Vector2(Vector3::dotProduct(relative, axisU),
                Vector3::dotProduct(relative, axisV)));
        }
        return result;
    };

    m_remeshedFaceUvs.clear();
    m_remeshedVertexUvs.clear();
    double uvCursor = 0.0;
    bool uvsComplete = m_exportUvs;

    // Both UV channels are produced: the extractor's per-vertex UVs (continuous
    // across any face that shares a vertex) and the per-face chart transfer
    // (correct where corners come from different charts of the cover).  The writer
    // picks per face, so a seam only exists where the cover really has one.
    bool vertexUvsPossible = m_exportUvs;
    if (vertexUvsPossible) {
        for (const auto& thread : parameterizationThreads) {
            if (nullptr == thread.remesher || thread.remesher->remeshedQuads().empty())
                continue;
            const std::vector<Vector2>& vertexUvs = thread.remesher->remeshedVertexUvs();
            if (vertexUvs.empty() || vertexUvs.size() != thread.remesher->remeshedVertices().size()) {
                vertexUvsPossible = false;
                break;
            }
        }
    }
    bool vertexUvsComplete = vertexUvsPossible;

    for (size_t i = 0; i < parameterizationThreads.size(); ++i) {
        auto& thread = parameterizationThreads[i];
        if (nullptr == thread.remesher)
            continue;
        const auto& quads = thread.remesher->remeshedQuads();
        if (quads.empty())
            continue;
        const auto& vertices = thread.remesher->remeshedVertices();

        std::vector<Vector2> islandVertexUvs;
        if (vertexUvsPossible) {
            islandVertexUvs = thread.remesher->remeshedVertexUvs();
            if (islandVertexUvs.size() != vertices.size())
                vertexUvsComplete = false;
        }

        std::vector<std::vector<Vector2>> islandFaceUvs;
        bool islandUvsOk = false;
        if (m_exportUvs && !thread.capturedUvs.empty()
            && thread.capturedUvs.size() == thread.island->triangles.size()
            && !thread.island->vertices.empty()) {
            UvTransfer transfer(thread.island->vertices, thread.island->triangles, thread.capturedUvs);
            if (transfer.valid()) {
                // (the fallback for an island without a usable transfer is the
                // extractor's per-vertex UVs, handled below by the caller)
                islandUvsOk = true;
                islandFaceUvs.reserve(quads.size());
                for (const auto& quad : quads) {
                    if (quad.size() < 3) {
                        islandUvsOk = false;
                        break;
                    }
                    Vector3 centroid;
                    for (const size_t corner : quad)
                        centroid = centroid + vertices[corner];
                    centroid = centroid * (1.0 / (double)quad.size());
                    size_t triangleIndex = 0;
                    if (!transfer.locateTriangle(centroid, &triangleIndex)) {
                        islandUvsOk = false;
                        break;
                    }
                    std::vector<Vector2> cornerUvs;
                    cornerUvs.reserve(quad.size());
                    for (const size_t corner : quad) {
                        Vector2 uv;
                        if (!transfer.evaluate(vertices[corner], triangleIndex, &uv)) {
                            islandUvsOk = false;
                            break;
                        }
                        cornerUvs.push_back(uv);
                    }
                    if (!islandUvsOk)
                        break;
                    // A degenerate chart (a collapsed or zero-area triangle in the
                    // parameterization) would collapse the face in UV space, so
                    // that face falls back to its own planar frame.
                    if (!(std::fabs(polygonUvArea(cornerUvs)) > 0.0)) {
                        // A degenerate chart on one face must not cost the whole run
                        // its UVs: try the face's own plane, and if even that is
                        // impossible keep the chart values rather than failing.
                        const std::vector<Vector2> planar = planarFaceUvs(vertices, quad);
                        if (!planar.empty())
                            cornerUvs = planar;
                    }
                    islandFaceUvs.push_back(cornerUvs);
                }
            }
        }

        // Chart-driven seam splitting: one vertex cannot carry two UVs, so the
        // mesh is cut where the cover changes chart.  Faces inside a chart keep
        // sharing vertices; only the seam duplicates them.
        std::vector<Vector3> splitVertices;
        std::vector<std::vector<size_t>> splitFaces;
        const std::vector<Vector3>* meshVertices = &vertices;
        const std::vector<std::vector<size_t>>* meshFaces = &quads;
        bool splitDone = false;
        m_uvChartCount = 0;
        // The symmetry pass cuts and re-welds the mesh; splitting first would leave
        // the cut row with duplicated vertices that the weld then collapses into
        // open edges.  Symmetry wins, the UVs stay per-vertex.
        if (m_exportUvs && m_uvSeamSplitting && m_symmetryAxis == 0 && islandUvsOk
            && islandFaceUvs.size() == quads.size() && !quads.empty()) {
            // The per-face UVs come from the cover; the split decision is made by
            // comparing them on shared edges.  The solver's own chart ids
            // (Parameterizer::triangleCharts) are exposed for a future extractor
            // that tags each cross point with its chart; on their own they do not
            // match the per-triangle integer branches the isolines use here.
            ChartSplit split = splitAtChartSeams(vertices, quads, islandFaceUvs);
            if (split.faces.size() == quads.size()
                && split.vertexUvs.size() == split.vertices.size() && !split.vertices.empty()) {
                splitVertices = std::move(split.vertices);
                splitFaces = std::move(split.faces);
                islandVertexUvs = std::move(split.vertexUvs);
                vertexUvsComplete = true;
                meshVertices = &splitVertices;
                meshFaces = &splitFaces;
                splitDone = true;
                m_uvChartCount = split.chartCount;
            }
        }

        // One stripe per island in UV space: their cover solves are independent.
        double minU = 0.0;
        double maxU = 0.0;
        double minV = 0.0;
        double maxV = 0.0;
        bool haveUv = false;
        const auto consider = [&](const Vector2& uv) {
            minU = haveUv ? std::min(minU, uv.x()) : uv.x();
            maxU = haveUv ? std::max(maxU, uv.x()) : uv.x();
            minV = haveUv ? std::min(minV, uv.y()) : uv.y();
            maxV = haveUv ? std::max(maxV, uv.y()) : uv.y();
            haveUv = true;
        };
        if (vertexUvsComplete) {
            for (const auto& uv : islandVertexUvs)
                consider(uv);
        }
        if (islandUvsOk) {
            for (const auto& faceUvs : islandFaceUvs)
                for (const auto& uv : faceUvs)
                    consider(uv);
        }
        double shiftU = 0.0;
        double shiftV = 0.0;
        if (haveUv) {
            shiftU = uvCursor - minU;
            shiftV = -minV;
            uvCursor += (maxU - minU) * 1.1 + 1e-6;
        }
        if (vertexUvsComplete) {
            for (auto& uv : islandVertexUvs) {
                uv.setX(uv.x() + shiftU);
                uv.setY(uv.y() + shiftV);
            }
        }
        if (islandUvsOk) {
            for (auto& faceUvs : islandFaceUvs) {
                for (auto& uv : faceUvs) {
                    uv.setX(uv.x() + shiftU);
                    uv.setY(uv.y() + shiftV);
                }
            }
        }

        size_t vertexStartIndex = m_remeshedVertices.size();
        m_remeshedVertices.reserve(m_remeshedVertices.size() + meshVertices->size());
        for (const auto& it : *meshVertices) {
            m_remeshedVertices.push_back(it);
        }
        for (const auto& it : *meshFaces) {
            std::vector<size_t> quad;
            quad.reserve(it.size());
            for (const auto& v : it)
                quad.push_back(vertexStartIndex + v);
            m_remeshedQuads.push_back(quad);
        }
        if (vertexUvsComplete && islandVertexUvs.size() == meshVertices->size()) {
            m_remeshedVertexUvs.insert(m_remeshedVertexUvs.end(),
                islandVertexUvs.begin(), islandVertexUvs.end());
        }
        if (islandUvsOk && islandFaceUvs.size() == quads.size()) {
            for (const auto& faceUvs : islandFaceUvs)
                m_remeshedFaceUvs.push_back(faceUvs);
        }
        if (m_exportUvs && !islandUvsOk)
            uvsComplete = false;
    }

    if (!vertexUvsComplete || m_remeshedVertexUvs.size() != m_remeshedVertices.size())
        m_remeshedVertexUvs.clear();

    if (!uvsComplete || (m_exportUvs && m_remeshedFaceUvs.size() != m_remeshedQuads.size())) {
        m_remeshedVertexUvs.clear();
        m_remeshedFaceUvs.clear();
    } else {
        // One normalization for both channels, so a face that falls back to the
        // per-corner chart still lands on the same UV scale as its neighbours.
        bool haveUv = false;
        double minU = 0.0;
        double maxU = 0.0;
        double minV = 0.0;
        double maxV = 0.0;
        const auto consider = [&](const Vector2& uv) {
            minU = haveUv ? std::min(minU, uv.x()) : uv.x();
            maxU = haveUv ? std::max(maxU, uv.x()) : uv.x();
            minV = haveUv ? std::min(minV, uv.y()) : uv.y();
            maxV = haveUv ? std::max(maxV, uv.y()) : uv.y();
            haveUv = true;
        };
        for (const auto& uv : m_remeshedVertexUvs)
            consider(uv);
        for (const auto& faceUvs : m_remeshedFaceUvs)
            for (const auto& uv : faceUvs)
                consider(uv);
        const double extent = haveUv ? std::max(maxU - minU, maxV - minV) : 0.0;
        if (extent > 0.0) {
            const auto normalize = [&](Vector2* uv) {
                uv->setX((uv->x() - minU) / extent);
                uv->setY((uv->y() - minV) / extent);
            };
            for (auto& uv : m_remeshedVertexUvs)
                normalize(&uv);
            for (auto& faceUvs : m_remeshedFaceUvs)
                for (auto& uv : faceUvs)
                    normalize(&uv);
        }
    }

    // Symmetry is applied last, on the merged mesh: the cut and the mirror have to
    // see the whole model, not one island at a time.
    m_symmetryStats = SymmetryStats();
    m_symmetryCheck = SymmetryCheck();
    m_symmetrySnapStats = SymmetrySnapStats();
    if (m_symmetryAxis >= 1 && m_symmetryAxis <= 3) {
        // setSymmetryTolerance is a fraction of the model size (it is what the
        // mirror pass has always taken); the measuring and snapping entry points
        // work in model units, so the scale is applied exactly once here.
        Vector3 extentMinimum;
        Vector3 extentMaximum;
        if (!m_remeshedVertices.empty()) {
            extentMinimum = m_remeshedVertices[0];
            extentMaximum = extentMinimum;
            for (const auto& vertex : m_remeshedVertices) {
                for (int i = 0; i < 3; ++i) {
                    extentMinimum[i] = std::min(extentMinimum[i], vertex[i]);
                    extentMaximum[i] = std::max(extentMaximum[i], vertex[i]);
                }
            }
        }
        const double diagonal = (extentMaximum - extentMinimum).length();
        const double tolerance = std::max(1e-12,
            m_symmetryTolerance * (diagonal > 0.0 ? diagonal : 1.0));
        // Measured in every mode, not just check: a mirror on a mesh that has no
        // mirror partner is the case worth warning about, and the caller can only
        // warn if the number is there.
        m_symmetryCheck = evaluateSymmetry(m_remeshedVertices, m_remeshedQuads,
            m_symmetryAxis, m_symmetryPlane, !m_symmetryPlaneKnown, tolerance);
        if (1 == m_symmetryMode) {
            m_symmetryStats.axis = m_symmetryAxis;
            m_symmetryStats.plane = m_symmetryCheck.plane;
            m_symmetryStats.tolerance = tolerance;
            m_symmetryStats.verticesBefore = m_remeshedVertices.size();
            m_symmetryStats.verticesAfter = m_remeshedVertices.size();
        } else if (2 == m_symmetryMode) {
            const double plane = m_symmetryPlaneKnown ? m_symmetryPlane : m_symmetryCheck.plane;
            m_symmetrySnapStats = snapToPlane(&m_remeshedVertices, m_symmetryAxis, plane, tolerance);
            m_symmetryStats.axis = m_symmetryAxis;
            m_symmetryStats.plane = plane;
            m_symmetryStats.tolerance = tolerance;
            m_symmetryStats.verticesBefore = m_remeshedVertices.size();
            m_symmetryStats.verticesAfter = m_remeshedVertices.size();
            m_symmetryStats.seamVertices = m_symmetrySnapStats.snappedVertices;
        } else {
            SymmetryResult mirrored = mirrorHalf(m_remeshedVertices, m_remeshedQuads,
                m_remeshedFaceUvs, m_remeshedVertexUvs,
                m_symmetryAxis, m_symmetryPlane, m_symmetryTolerance, m_symmetryKeepPositive);
            // The check above measured the mesh as it arrived, which is what the
            // "was this symmetric enough to mirror" question is about; the mirror
            // stats describe what the pass then did to it.
            m_symmetryStats = mirrored.stats;
            m_remeshedVertices = std::move(mirrored.vertices);
            m_remeshedQuads = std::move(mirrored.faces);
            m_remeshedFaceUvs = std::move(mirrored.faceUvs);
            m_remeshedVertexUvs = std::move(mirrored.vertexUvs);
        }
    }

    m_deviation = DeviationStats();
    if (m_measureDeviation)
        m_deviation = measureDeviation(m_remeshedVertices, m_vertices, m_triangles);

    // Hole filling and patch selection used to be silent decisions made deep in
    // the extractor.  Sum what each island actually did so the caller can say
    // "3 holes filled, 1 left open, 2 patches dropped" instead of leaving the
    // user to count boundary edges in another tool.
    m_holeStats = HoleStats();
    m_patchStats = PatchStats();
    for (const auto& thread : parameterizationThreads) {
        if (nullptr == thread.remesher)
            continue;
        const QuadExtractor::ExtractionStats& stats = thread.remesher->extractionStats();
        m_holeStats.filled += stats.holesFilled;
        m_holeStats.ignored += stats.holesIgnored;
        m_holeStats.facesAdded += stats.holeFacesAdded;
        m_patchStats.kept += stats.patchesKept;
        m_patchStats.dropped += stats.patchesDropped;
        m_patchStats.weldedVertices += stats.weldedVertices;
    }
    m_outputStats = analyzeTopology(m_remeshedVertices, m_remeshedQuads);

    // Shells are the number a modelling workflow watches, so say whether the count
    // is the design or a weld that did not close.  A weld that ran and still left
    // more than one shell means the boundary vertices are further apart than the
    // position tolerance, which is a fact about the extraction, not about intent.
    m_patchStats.remainingShells = m_outputStats.components;
    m_patchStats.reason.clear();
    if (m_patchStats.remainingShells > 2 && m_weldPatches) {
        m_patchStats.reason = (m_holeStats.ignored > 0) ? "open_holes" : "gap_exceeds_tolerance";
    } else if (m_patchStats.remainingShells > 2 && !m_weldPatches) {
        m_patchStats.reason = "no_weld_requested";
    }

    // Quality needs vertex positions, which the analysis above deliberately does
    // not touch; it is the block a reviewer reads to decide "can this be modelled
    // on" and it used to require a second tool.
    {
        Vector3 extentMinimum;
        Vector3 extentMaximum;
        if (!m_remeshedVertices.empty()) {
            extentMinimum = m_remeshedVertices[0];
            extentMaximum = extentMinimum;
            for (const auto& vertex : m_remeshedVertices) {
                for (int i = 0; i < 3; ++i) {
                    extentMinimum[i] = std::min(extentMinimum[i], vertex[i]);
                    extentMaximum[i] = std::max(extentMaximum[i], vertex[i]);
                }
            }
        }
        m_qualityStats = collectQuality(m_remeshedVertices, m_remeshedQuads,
            (extentMaximum - extentMinimum).length());
    }

    auto t_mergeEnd = std::chrono::high_resolution_clock::now();

    const auto elapsedUs = [](const std::chrono::high_resolution_clock::time_point& from,
                               const std::chrono::high_resolution_clock::time_point& to) {
        return std::chrono::duration_cast<std::chrono::microseconds>(to - from).count();
    };
    const long long t_voxelUs = elapsedUs(t_voxelStart, t_voxelEnd);
    const long long t_splitUs = elapsedUs(t_splitStart, t_afterSplit);
    const long long t_buildUs = elapsedUs(t_afterSplit, t_buildEnd);
    const long long t_isotropicWallUs = elapsedUs(t_buildEnd, t_isotropicEnd);
    const long long t_parameterizeWallUs = elapsedUs(t_isotropicEnd, t_parallelEnd);
    const long long t_parallelWallUs = elapsedUs(t_buildEnd, t_parallelEnd);
    const long long t_mergeUs = elapsedUs(t_parallelEnd, t_mergeEnd);
    const long long t_totalUs = elapsedUs(t_start, t_mergeEnd);
    const long long t_analyzeUs = (long long)(m_inputAnalysisMilliseconds * 1000.0);

    const long long t_decimateUs = decimationStats.timeUs.load();
    const long long t_adaptiveFieldUs = adaptiveFieldTime.load();
    const size_t decimatedIslands = decimationStats.islandsDecimated.load();

    m_phaseReport.clear();
    m_phaseTimings.clear();
    {
        std::ostringstream line;
        // Whole milliseconds hide the per-island steps on a mesh split into many
        // small islands, so keep one decimal place.
        const auto milliseconds = [](long long microseconds) {
            std::ostringstream value;
            value.setf(std::ios::fixed);
            value.precision(1);
            value << (double)microseconds / 1000.0 << " ms";
            return value.str();
        };
        // Every phase lands in two places at once: the line a human reads and the
        // record a script reads.  Keeping them in step here is the only way they
        // cannot drift apart.
        auto phase = [&](const char* name, long long microseconds, bool accumulated = false) {
            line.str(std::string());
            line << name << ": " << milliseconds(microseconds);
            m_phaseReport.push_back(line.str());
            PhaseTiming timing;
            timing.name = name;
            timing.milliseconds = (double)microseconds / 1000.0;
            timing.accumulated = accumulated;
            m_phaseTimings.push_back(timing);
        };

        line.str(std::string());
        line << "Islands: " << islandContexes.size()
             << ", input triangles: " << m_triangles.size();
        m_phaseReport.push_back(line.str());

        phase("Analyze input topology", t_analyzeUs);
        phase("Compute voxel size", t_voxelUs);
        phase("Split into islands", t_splitUs);
        phase("Build island contexts", t_buildUs);

        line.str(std::string());
        if (decimatedIslands > 0) {
            line << "Mesh simplifier: RAN on " << decimatedIslands << " of "
                 << decimationStats.islandsConsidered.load() << " islands, "
                 << decimationStats.trianglesBefore.load() << " -> "
                 << decimationStats.trianglesAfter.load() << " triangles, "
                 << milliseconds(t_decimateUs);
        } else {
            line << "Mesh simplifier: SKIPPED (no island above "
                 << (long long)decimateTriggerRatio << "x target triangle count), "
                 << milliseconds(t_decimateUs);
        }
        m_phaseReport.push_back(line.str());
        {
            PhaseTiming timing;
            timing.name = "Mesh simplifier";
            timing.milliseconds = (double)t_decimateUs / 1000.0;
            timing.accumulated = true;
            m_phaseTimings.push_back(timing);
        }

        // The accumulated figures sum the islands, so on a multi-island mesh they
        // add up to more than the wall clock next to them.  That gap is the point:
        // accumulated / wall is how many cores the phase actually kept busy.
        phase("Adaptive target length field (accumulated)", t_adaptiveFieldUs, true);
        phase("Isotropic remesh (accumulated)",
            resampleTime.load() - t_decimateUs - t_adaptiveFieldUs, true);
        phase("Parameterize (accumulated)", parameterizeTimeAccumulated.load(), true);
        phase("Quad extract (accumulated)", extractTimeAccumulated.load(), true);

        {
            std::lock_guard<std::mutex> lock(m_stageTimingMutex);
            std::sort(m_stageTimes.begin(), m_stageTimes.end(),
                [](const StageTime& first, const StageTime& second) {
                    return first.order < second.order;
                });
            for (const auto& it : m_stageTimes) {
                line.str(std::string());
                line << "    " << it.name << ": " << milliseconds(it.microseconds);
                m_phaseReport.push_back(line.str());
                PhaseTiming timing;
                timing.name = it.name;
                timing.milliseconds = (double)it.microseconds / 1000.0;
                timing.accumulated = true;
                m_phaseTimings.push_back(timing);
            }
        }

        phase("Isotropic phase wall clock", t_isotropicWallUs);
        phase("Parameterize phase wall clock", t_parameterizeWallUs);
        phase("Parallel phase wall clock", t_parallelWallUs);

        {
            const long long accumulated = resampleTime.load()
                + parameterizeTimeAccumulated.load() + extractTimeAccumulated.load();
            line.str(std::string());
            line.setf(std::ios::fixed);
            line.precision(2);
            m_coresBusy = (t_parallelWallUs > 0)
                ? (double)accumulated / (double)t_parallelWallUs
                : 0.0;
            line << "Cores kept busy across the parallel phase: "
                 << m_coresBusy
                 << " (islands are the unit of parallelism)";
            m_phaseReport.push_back(line.str());
            PhaseTiming timing;
            timing.name = "Cores kept busy";
            timing.milliseconds = m_coresBusy;
            m_phaseTimings.push_back(timing);
        }

        phase("Merge islands", t_mergeUs);
        phase("Total", t_totalUs);
    }

    if (m_reportEnabled) {
        for (const auto& line : m_phaseReport)
            Log::write(Log::Normal, line + "\n");
    }

#if AUTO_REMESHER_DEBUG
    Log::write(Log::Debug, "Remesh done\n");
#endif

    if (nullptr != m_progressHandler)
        m_progressHandler(m_tag, 1.0, "Done");

    return true;
}

void AutoRemesher::analyzeInput()
{
    if (m_inputAnalyzed)
        return;
    const auto start = std::chrono::high_resolution_clock::now();
    m_inputStats = analyzeTopology(m_vertices, m_triangles);
    const auto end = std::chrono::high_resolution_clock::now();
    m_inputAnalysisMilliseconds = (double)std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
    m_inputAnalyzed = true;
}

}
