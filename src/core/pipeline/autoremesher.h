/*
 *  Copyright (c) 2020 Jeremy HU <jeremy-at-dust3d dot org>. All rights reserved. 
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:

 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.

 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 */
#ifndef AUTO_REMESHER_AUTO_REMESHER_H
#define AUTO_REMESHER_AUTO_REMESHER_H
#include <AutoRemesher/Progress>
#include <AutoRemesher/TopologyStats>
#include <AutoRemesher/Vector3>
#include <core/geom/topologystats.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace AutoRemesher {

class IsotropicRemesher;

typedef void (*AutoRemesherProgressHandler)(void* tag, float progress, const char* status);

// What hole filling and patch selection actually did, summed over the islands.
// Both are decisions the extractor used to make silently: a hole longer than the
// graded limit is left open, and every extracted patch but the largest is
// dropped unless the caller asked to keep it.
struct HoleStats {
    size_t filled = 0;
    size_t ignored = 0;
    size_t facesAdded = 0;
};

struct PatchStats {
    size_t kept = 0;
    size_t dropped = 0;
};

// One entry of the phase report, machine readable.  accumulated figures are
// summed over islands, so accumulated / wall clock is the parallelism a phase
// actually reached.
struct PhaseTiming {
    std::string name;
    double milliseconds = 0.0;
    bool accumulated = false;
};

class AutoRemesher {
public:
    AutoRemesher(const std::vector<Vector3>& vertices,
        const std::vector<std::vector<size_t>>& triangles)
        : m_vertices(vertices)
        , m_triangles(triangles)
    {
    }

    void setTargetTriangleCount(size_t targetTriangleCount)
    {
        m_targetTriangleCount = targetTriangleCount;
    }

    void setScaling(double scaling)
    {
        m_scaling = scaling;
    }

    void setProgressHandler(AutoRemesherProgressHandler progressHandler)
    {
        m_progressHandler = progressHandler;
    }

    void setTag(void* tag)
    {
        m_tag = tag;
    }

    void setGradientAdaptivity(double adaptivity)
    {
        m_adaptivity = adaptivity;
    }

    void setSharpEdgeDegrees(double degrees)
    {
        m_sharpEdgeDegrees = degrees;
    }

    void setAnisotropy(double anisotropy)
    {
        m_anisotropy = anisotropy;
    }

    void setSmoothNormalDegrees(double degrees)
    {
        m_smoothNormalDegrees = degrees;
    }

    // When enabled, each island's quad cover scale is solved from the requested
    // quad budget instead of being taken from the average edge length.  The
    // adaptive scaling field can leave the requested count off by 2x, so the
    // closed loop is what makes "target quads" an actual promise.
    void setExactTargetQuads(bool exact)
    {
        m_exactTargetQuads = exact;
    }

    // The cross point weld step follows the island voxel size (1e-3 of it) when
    // enabled, so models authored in any unit weld at the same rate.  Disabling
    // it restores the historical fixed 1e-5, which collapses sub-millimetre
    // models and over-merges very large ones.
    void setRelativePositionTolerance(bool relative)
    {
        m_relativePositionTolerance = relative;
    }

    // 0 keeps only the largest extracted patch per island (historical).  A
    // positive fraction keeps every patch above that share of the island area
    // and welds their coincident vertices, so small parts are not thrown away.
    void setIslandAreaFraction(double fraction)
    {
        m_islandAreaFraction = fraction;
    }

    void setWeldPatches(bool weld)
    {
        m_weldPatches = weld;
    }

    void setGradedHoleFilling(bool graded)
    {
        m_gradedHoleFilling = graded;
    }

    void setProjectedHoleCaps(bool projected)
    {
        m_projectedHoleCaps = projected;
    }

    void setLocalRemesh(bool enabled)
    {
        m_localRemesh = enabled;
    }

    const std::vector<Vector3>& remeshedVertices()
    {
        return m_remeshedVertices;
    }

    // One UV pair per corner of every remeshed quad, in the same order as
    // remeshedQuads(); empty when the transfer could not cover every island.
    // A face takes its chart from its own centroid, so a cover seam falls between
    // faces instead of through them; islands are laid out side by side, so the
    // UV space does not overlap itself.
    const std::vector<std::vector<Vector2>>& remeshedFaceUvs()
    {
        return m_remeshedFaceUvs;
    }

    // UV transfer costs one grid build per island; turn it off for a run that
    // only needs the geometry.
    void setExportUvs(bool enabled)
    {
        m_exportUvs = enabled;
    }

    const std::vector<std::vector<size_t>>& remeshedQuads()
    {
        return m_remeshedQuads;
    }

    const std::vector<Vector3>& decimatedVertices()
    {
        return m_decimatedVertices;
    }

    const std::vector<std::vector<size_t>>& decimatedTriangles()
    {
        return m_decimatedTriangles;
    }

    bool decimated()
    {
        return m_decimated;
    }

    const std::vector<Vector3>& isotropicVertices()
    {
        return m_isotropicVertices;
    }

    const std::vector<std::vector<size_t>>& isotropicTriangles()
    {
        return m_isotropicTriangles;
    }

    const std::vector<uint8_t>& isotropicExtractedConnectionMoved()
    {
        return m_isotropicExtractedConnectionMoved;
    }

    const std::vector<std::vector<Vector2>>& isotropicOriginalTriangleUvs()
    {
        return m_isotropicOriginalTriangleUvs;
    }

    const std::vector<std::vector<Vector2>>& isotropicTriangleUvs()
    {
        return m_isotropicTriangleUvs;
    }

    const std::vector<Vector3>& isotropicSingularVertices()
    {
        return m_isotropicSingularVertices;
    }

    const std::vector<std::pair<Vector3, Vector3>>& isotropicExtractedConnections()
    {
        return m_isotropicExtractedConnections;
    }

    bool remesh();

    // `progress` is how far island `threadIndex` has got, 0..1.  `status` names
    // the step it is on, or nullptr to keep the island's current one.  Called
    // from the island worker threads.
    void updateProgress(size_t threadIndex, float progress, const char* status = nullptr);

    // Records how long a named pipeline step took, summed over the islands that
    // ran it.  `order` places the step in the phase report; it is the step's
    // position along the pipeline, so the report reads in execution order no
    // matter which island happened to reach the step first.
    void accumulateStageTime(const char* name, float order, long long microseconds);

    // A handler for one stage of one island: maps the stage's own 0..1 fraction
    // onto [begin, end] of that island's progress, and times each named step on
    // the way through for the phase report.  `stageOrder` is where the stage sits
    // along the pipeline, so the report reads in execution order.  Only the
    // island's own worker thread calls the result.
    ProgressHandler makeStageProgress(size_t islandIndex, float begin, float end, float stageOrder);

    const std::vector<std::string>& phaseReport()
    {
        return m_phaseReport;
    }

    // Machine readable twin of phaseReport().  Same steps, same order, but with
    // the number already parsed out of the string.
    const std::vector<PhaseTiming>& phaseTimings() const
    {
        return m_phaseTimings;
    }

    // Topology of the mesh the caller handed in and of the quads that came out.
    // The input side is what tells a caller "this shell has 5950 boundary edges
    // and 1 hole", which the pipeline used to know nothing about.
    // Count the input topology up front, so an application can warn before a run
    // that takes minutes.  remesh() reuses the result instead of counting twice.
    void analyzeInput();

    const TopologyStats& inputStats() const
    {
        return m_inputStats;
    }

    // Wall clock of the input analysis, whether the application ran it early
    // (analyzeInput) or remesh() had to do it.  The phase report quotes this, so
    // a caller that checks health up front does not see a fake 0.0 ms step.
    double inputAnalysisMilliseconds() const
    {
        return m_inputAnalysisMilliseconds;
    }

    const TopologyStats& outputStats() const
    {
        return m_outputStats;
    }

    size_t islandCount() const
    {
        return m_islandCount;
    }

    double coresBusy() const
    {
        return m_coresBusy;
    }

    const HoleStats& holeStats() const
    {
        return m_holeStats;
    }

    const PatchStats& patchStats() const
    {
        return m_patchStats;
    }

    const std::string& lastError() const
    {
        return m_lastError;
    }

    // The phase report used to go straight to stderr from inside the library.
    // The application owns that decision now: a silent run turns it off, and a
    // caller that wants the report reads phaseReport()/phaseTimings() instead.
    void setReportEnabled(bool enabled)
    {
        m_reportEnabled = enabled;
    }

    static const double m_defaultSharpEdgeDegrees;

    // Per-island durations are accumulated in microseconds: a mesh split into
    // hundreds of islands spends well under a millisecond on most of them, and
    // truncating each one to whole milliseconds loses the bulk of the total.
    struct DecimationStats {
        std::atomic<long long> timeUs { 0 };
        std::atomic<size_t> islandsDecimated { 0 };
        std::atomic<size_t> islandsConsidered { 0 };
        std::atomic<size_t> trianglesBefore { 0 };
        std::atomic<size_t> trianglesAfter { 0 };
    };

private:
    std::vector<Vector3> m_vertices;
    std::vector<std::vector<size_t>> m_triangles;
    std::vector<Vector3> m_remeshedVertices;
    std::vector<std::vector<Vector2>> m_remeshedFaceUvs;
    std::vector<std::vector<size_t>> m_remeshedQuads;
    std::vector<Vector3> m_decimatedVertices;
    std::vector<std::vector<size_t>> m_decimatedTriangles;
    bool m_decimated = false;
    std::vector<Vector3> m_isotropicVertices;
    std::vector<std::vector<size_t>> m_isotropicTriangles;
    std::vector<std::vector<Vector2>> m_isotropicTriangleUvs;
    std::vector<std::vector<Vector2>> m_isotropicOriginalTriangleUvs;
    std::vector<uint8_t> m_isotropicExtractedConnectionMoved;
    std::vector<Vector3> m_isotropicSingularVertices;
    std::vector<std::pair<Vector3, Vector3>> m_isotropicExtractedConnections;
    std::vector<float> m_threadProgress;
    std::vector<float> m_threadProgressWeights;
    std::vector<const char*> m_threadStatus;
    // The weighted sum of m_threadProgress, kept incrementally so that a
    // fine-grained update stays O(1) rather than a scan of every island.
    double m_progressSum = 0.0;
    int m_reportedPermille = -1;
    const char* m_reportedStatus = nullptr;
    std::vector<std::string> m_phaseReport;
    mutable std::mutex m_progressMutex;
    std::mutex m_stageTimingMutex;
    struct StageTime {
        std::string name;
        float order = 0.0f;
        long long microseconds = 0;
    };
    std::vector<StageTime> m_stageTimes;
    double m_scaling = 0.0;
    size_t m_targetTriangleCount = 0;
    double m_voxelSize = 0.0;
    double m_adaptivity = 1.0;
    double m_anisotropy = 1.0;
    double m_sharpEdgeDegrees = m_defaultSharpEdgeDegrees;
    double m_smoothNormalDegrees = 0.0;
    bool m_exactTargetQuads = false;
    bool m_exportUvs = true;
    bool m_relativePositionTolerance = true;
    double m_islandAreaFraction = 0.0;
    bool m_weldPatches = false;
    bool m_gradedHoleFilling = true;
    bool m_projectedHoleCaps = false;
    bool m_localRemesh = false;
    TopologyStats m_inputStats;
    TopologyStats m_outputStats;
    bool m_inputAnalyzed = false;
    double m_inputAnalysisMilliseconds = 0.0;
    HoleStats m_holeStats;
    PatchStats m_patchStats;
    std::vector<PhaseTiming> m_phaseTimings;
    size_t m_islandCount = 0;
    double m_coresBusy = 0.0;
    std::string m_lastError;
    bool m_reportEnabled = true;
    AutoRemesherProgressHandler m_progressHandler = nullptr;
    void* m_tag = nullptr;

    static double calculateAverageEdgeLength(const std::vector<Vector3>& vertices,
        const std::vector<std::vector<size_t>>& faces);
    void initializeVoxelSize();
    static bool decimateIfTooDense(std::vector<Vector3>& vertices,
        std::vector<std::vector<size_t>>& triangles,
        double voxelSize,
        double sharpEdgeDegrees,
        size_t islandIndex,
        DecimationStats* stats);
    static void resample(std::vector<Vector3>& vertices,
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
        std::vector<std::vector<size_t>>* decimatedTrianglesOut);
    static double calculateMeshArea(const std::vector<Vector3>& vertices,
        const std::vector<std::vector<size_t>>& triangles);
};

}

#endif
