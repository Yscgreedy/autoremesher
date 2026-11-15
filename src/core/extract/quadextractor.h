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
#ifndef AUTO_REMESHER_QUAD_EXTRACTOR_H
#define AUTO_REMESHER_QUAD_EXTRACTOR_H
#include <AutoRemesher/Progress>
#include <AutoRemesher/Vector2>
#include <AutoRemesher/Vector3>
#include <axisalignedboundingbox.h>
#include <axisalignedboundingboxtree.h>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace AutoRemesher {

class QuadExtractor {
public:
    QuadExtractor(const std::vector<Vector3>* vertices,
        const std::vector<std::vector<size_t>>* triangles,
        const std::vector<std::vector<Vector2>>* triangleUvs)
        : m_vertices(vertices)
        , m_triangles(triangles)
        , m_triangleUvs(triangleUvs)
    {
    }

    const std::vector<Vector3>& remeshedVertices()
    {
        return m_remeshedVertices;
    }

    // One UV per remeshed vertex, in the same order as remeshedVertices(); empty
    // when any pass could not keep it in step.
    const std::vector<Vector2>& remeshedVertexUvs() const
    {
        return m_remeshedVertexUvs;
    }

    const std::vector<std::vector<size_t>>& remeshedQuads()
    {
        return m_remeshedPolygons;
    }

    // What the last extract() run did, counted on the mesh that run ended with
    // rather than on the sum of its internal passes.  fixHoles() runs twice and
    // the second, post-cleanup pass walks the same still-open loops again, so:
    //   - holesFilled / holeFacesAdded only grow when a pass really appended
    //     faces (a capped loop is gone by the next pass, nothing is counted
    //     twice), and they accumulate over both passes because both caps are part
    //     of the mesh that comes out;
    //   - holesIgnored is reset by every pass, so it reports the holes that were
    //     still open when the last pass returned.
    struct ExtractionStats {
        size_t holesFilled = 0; // holes (loops) actually filled on the final mesh
        size_t holesIgnored = 0; // holes skipped (too long / no projected caps)
        // Loops sealed because the input was closed (setSealClosedInputHoles):
        // counted per pass over the whole extraction like holeFacesAdded, since a
        // loop a first pass sealed is gone by the second one anyway.
        size_t holesSealedClosedInput = 0;
        size_t holeFacesAdded = 0; // faces created by hole filling
        size_t patchesKept = 0; // extracted patches kept for this island
        size_t patchesDropped = 0; // patches thrown away
        size_t boundaryLoops = 0; // output boundary loops, if cheap to compute
        size_t openEdges = 0; // output open (boundary) edges
        size_t nonManifoldEdges = 0; // output non-manifold edges
        // The final validate pass.  The cleanup passes above reconnect faces one
        // defect at a time and can leave a face that already exists; a caller
        // that counts faces per edge reads that as a non-manifold edge, so the
        // counts are worth reporting rather than silently fixing.
        size_t duplicateFacesRemoved = 0;
        size_t degenerateFacesRemoved = 0;
        size_t weldedVertices = 0;
        size_t nonManifoldFacesRemoved = 0;
        // The aspect clamp, counted on the run that produced this mesh:
        //   - aspectClampedEdges: collapses it applied, one per face per round;
        //   - aspectClampedFaces: faces it had to act on;
        //   - aspectRejectedFaces: over-long faces it left alone because merging
        //     their longest side would have given some edge a third face;
        //   - facesTooLongRemaining: quads still over the threshold when it gave up.
        // A clamp turns a quad into a triangle (or drops a face it flattens), so
        // quad_ratio moves even when nothing is rejected: the two read differently
        // on purpose.
        size_t aspectClampedEdges = 0;
        size_t aspectClampedFaces = 0;
        size_t aspectRejectedFaces = 0;
        size_t facesTooLongRemaining = 0;
    };

    // Statistics of the mesh extract() produced; updated by extract() itself, so
    // they are only meaningful once it has run.
    const ExtractionStats& extractionStats() const
    {
        return m_extractionStats;
    }

    // The raw connections produced by extractConnections(), before graph cleanup.
    const std::vector<std::pair<Vector3, Vector3>>& extractedConnections() const
    {
        return m_extractedConnections;
    }

    void setOriginalTriangleUvs(const std::vector<std::vector<Vector2>>* originalTriangleUvs)
    {
        m_originalTriangleUvs = originalTriangleUvs;
    }

    void setSingularVertices(const std::vector<size_t>* singularVertices)
    {
        m_singularVertices = singularVertices;
    }

    void setProgressHandler(ProgressHandler progressHandler)
    {
        m_progressHandler = std::move(progressHandler);
    }

    // Quantisation step for the cross point weld; 0 keeps the historical fixed
    // 1e-5.  AutoRemesher passes one relative to the island's voxel size so a
    // model authored in metres, millimetres and "hundreds of units" all weld at
    // the same rate instead of the small ones collapsing into themselves.
    void setPositionTolerance(double tolerance)
    {
        m_positionTolerance = tolerance;
    }

    // Legacy behaviour keeps only the largest edge-connected patch of the
    // extracted polygons.  A positive fraction keeps every patch whose area is
    // at least that fraction of the total, then welds coincident vertices and
    // drops duplicate/degenerate faces so the extra patches do not turn into
    // non-manifold edges.
    void setIslandAreaFraction(double fraction)
    {
        m_islandAreaFraction = fraction;
    }

    // Runs the coincident-vertex weld and duplicate-face removal even when only
    // the largest patch is kept; the cleanup can leave the same face twice.
    void setWeldPatches(bool weld)
    {
        m_weldPatches = weld;
    }

    // Seals every boundary loop by ear clipping instead of giving up when the
    // greedy quad pass runs out of score or the loop is longer than 65 vertices.
    void setGradedHoleFilling(bool graded)
    {
        m_gradedHoleFilling = graded;
    }

    // Large holes (>65 vertices) get a two-ring cap whose apex and mid ring are
    // projected onto the source surface, instead of being left open.
    void setProjectedHoleCaps(bool projected)
    {
        m_projectedHoleCaps = projected;
    }

    // Seal every boundary loop, whatever its perimeter, because the input this
    // mesh came from had no boundary at all: a hole here is what the extraction
    // failed to cover, not the shape.  Without it the 8/65 edge thresholds apply
    // and a low quad budget - where the cover leaves a few hundred edges open -
    // hands the caller a mesh with 658 boundary edges and two shells.
    // Large loops are capped with the projected two-ring cap in this mode, since
    // the source surface is known to exist underneath them.
    // Off by default: an open input keeps the historical thresholds.
    void setSealClosedInputHoles(bool seal)
    {
        m_sealClosedInputHoles = seal;
    }

    // Hole filling decides by edge count, and an edge count only describes a
    // physical hole when the output carries the edge length the remesher aimed at.
    // These two numbers are therefore read as "hole perimeter in target edges":
    // 8 and 65 reproduce the historical fixed counts exactly while
    // setHoleTargetEdgeLength() is left at 0, and follow the mesh density once a
    // caller states the length it planned for.
    void setHoleLengthThresholds(double smallEdges, double largeEdges)
    {
        m_holeSmallEdges = smallEdges > 0.0 ? smallEdges : 0.0;
        m_holeLargeEdges = largeEdges > 0.0 ? largeEdges : 0.0;
    }

    // The edge length the remesher was aiming at, in the model's own units (the
    // IslandRemesher voxel size).  Zero keeps the thresholds as raw edge counts;
    // a positive length turns them into a length that survives a model authored in
    // other units, because the ratio below is unitless.
    void setHoleTargetEdgeLength(double targetEdgeLength)
    {
        m_holeTargetEdgeLength = targetEdgeLength > 0.0 ? targetEdgeLength : 0.0;
    }

    // Local topology optimisation: merges triangle pairs into quads and splits
    // the larger faces, judged by one global energy (face size + vertex valence)
    // instead of the per pass heuristics.
    // The validate pass is on by default: it is what keeps the written file and
    // the reported numbers describing the same mesh.  Turning it off restores the
    // pre-1.3 behaviour for an A/B run.
    void setCleanDegenerate(bool enabled)
    {
        m_cleanDegenerate = enabled;
    }

    // Collapses a quad whose longest side over its shortest exceeds this, along
    // that longest side; 0 keeps every face the topology passes produced.  It is
    // part of the same switch as setCleanDegenerate(): an output the caller asked
    // to be usable rather than merely well connected, so both go off together.
    void setMaximumAspect(double maximumAspect)
    {
        m_maximumAspect = maximumAspect;
    }

    void setLocalRemesh(bool enabled)
    {
        m_localRemesh = enabled;
    }

    // Per connection of extractedConnections(): 0 untouched, 1 on a triangle whose uv
    // was repaired, 2 added by holdSingularLines(). Drives the [Param] preview color.
    const std::vector<uint8_t>& extractedConnectionMoved() const
    {
        return m_extractedConnectionMoved;
    }

    bool extract();

private:
    // The isoline a connection was cut from: which uv coordinate is held constant,
    // which integer value it is held at, and which triangle produced the segment.
    struct ConnectionInfo {
        size_t triangleIndex = 0;
        int coordIndex = 0;
        int integer = 0;
    };

    // Connections are looked up once per candidate edge in the singularity line
    // walk, so the ordered map was replaced by a hash map keyed on the same
    // unordered vertex pair.
    struct EdgePairHash {
        size_t operator()(const std::pair<size_t, size_t>& edge) const
        {
            uint64_t hash = (uint64_t)edge.first * 0x9e3779b97f4a7c15ull
                ^ ((uint64_t)edge.second + 0x165667b19e3779f9ull);
            hash ^= hash >> 31;
            hash *= 0xbf58476d1ce4e5b9ull;
            hash ^= hash >> 27;
            return (size_t)hash;
        }
    };

    const std::vector<Vector3>* m_vertices = nullptr;
    const std::vector<std::vector<size_t>>* m_triangles = nullptr;
    const std::vector<std::vector<Vector2>>* m_triangleUvs = nullptr;
    std::vector<Vector3> m_remeshedVertices;
    // Parallel to m_remeshedVertices: the uv the cover gave the cross point each
    // output vertex came from.  Kept in step by the helpers at the end of this
    // class, never by a pass writing it directly.
    std::vector<Vector2> m_remeshedVertexUvs;
    std::vector<std::vector<size_t>> m_remeshedPolygons;
    std::vector<std::pair<Vector3, Vector3>> m_extractedConnections;
    std::vector<uint8_t> m_extractedConnectionMoved;
    const std::vector<std::vector<Vector2>>* m_originalTriangleUvs = nullptr;
    const std::vector<size_t>* m_singularVertices = nullptr;
    double m_positionTolerance = 0.0;
    double m_islandAreaFraction = 0.0;
    bool m_weldPatches = false;
    bool m_gradedHoleFilling = false;
    bool m_projectedHoleCaps = false;
    bool m_sealClosedInputHoles = false;
    bool m_localRemesh = false;
    bool m_cleanDegenerate = true;
    double m_maximumAspect = 0.0;
    double m_holeSmallEdges = 8.0;
    double m_holeLargeEdges = 65.0;
    double m_holeTargetEdgeLength = 0.0;
    // Filled by extract() and its hole/patch passes; see ExtractionStats.
    ExtractionStats m_extractionStats;
    ProgressHandler m_progressHandler;
    // Ear clipping over the loop's best fit plane followed by quad pairing; adds
    // the faces it can and returns how many it added.  largeHoleEdges is the large
    // hole threshold already converted to output edges, so the cap strategy and the
    // decision to cap at all cannot drift apart.
    // allowProjectedRing forces the two-ring projected cap even when the caller did
    // not ask for --projected-caps (setSealClosedInputHoles does that): on a closed
    // input the loop is an artifact and the source surface is known to be there.
    size_t fillHoleGraded(const std::vector<size_t>& hole, double largeHoleEdges,
        bool allowProjectedRing = false);
    // Ratio between the edge length the remesher aimed at and the one the output
    // actually came out with; 1.0 whenever the caller did not state a target, which
    // is what keeps the default hole thresholds counting raw edges.
    double holeEdgeScale() const;
    // Merges vertices that sit on the same point, whether or not they share a face.
    // The validate pass welds along face edges only, so a duplicated cross point
    // that is not adjacent to its original survives it - and shows up as a boundary
    // loop walking through the same vertex twice, which no cap can close.
    // Returns the number of vertices merged away; 0 when nothing was close enough.
    size_t weldCoincidentVertices();
    double remeshedMeanEdgeLength() const;
    // Snaps a point onto the source mesh (the input surface, which is complete
    // even where the output has a hole) so cap vertices follow the real surface.
    bool projectPointToSource(const Vector3& position, double radius, Vector3* projected);
    // Hill climbs a topology energy with face merges and face splits; returns the
    // number of operations applied.
    size_t optimizeLocalTopology(size_t maximumOperations);
    // Cuts a small disk around a non-quad face, drops its interior vertices and
    // quadrangulates the boundary loop; returns the number of patches replaced.
    size_t requadrangulateNonQuadPatches(size_t maximumPatches);
    // Merges vertices that land on the same quantised position and removes the
    // faces that collapse or duplicate as a result; returns the face count kept.
    size_t weldKeptPatches();
    std::unordered_map<std::pair<size_t, size_t>, ConnectionInfo, EdgePairHash> m_connectionInfos;
    // Every cleanup pass ends by pulling its patch back onto the source mesh,
    // and rebuilding the bounding volume tree over that mesh each time was the
    // bulk of those passes.  The source mesh never changes, so one tree is kept.
    std::vector<AxisAlignedBoudingBox> m_projectionBoxes;
    std::vector<size_t> m_projectionIndices;
    std::unique_ptr<AxisAlignedBoudingBoxTree> m_projectionTree;
    AxisAlignedBoudingBoxTree* projectionTree();
    // smoothAroundVertices() runs once per accepted cleanup operation; keeping the
    // vertex to face lists alive lets each run refill them instead of allocating
    // tens of thousands of small vectors.
    std::vector<std::vector<size_t>> m_vertexFaceScratch;
    std::set<std::pair<size_t, size_t>> m_addedConnections;
    // Rebuilt by every cleanup pass and queried per candidate face, so the half
    // edge set is hashed rather than ordered.
    std::unordered_set<std::pair<size_t, size_t>, EdgePairHash> m_halfEdges;

    void extractConnections(std::vector<Vector3>* crossPoints,
        std::vector<size_t>* sourceTriangles,
        std::vector<Vector2>* crossPointUvs,
        std::set<std::pair<size_t, size_t>>* connections);
    void holdSingularLines(std::vector<Vector3>* crossPoints,
        std::vector<size_t>* sourceTriangles,
        std::vector<Vector2>* crossPointUvs,
        std::set<std::pair<size_t, size_t>>* connections);
    void extractEdges(const std::set<std::pair<size_t, size_t>>& connections,
        std::unordered_map<size_t, std::unordered_set<size_t>>* edgeConnectMap);
    bool collapseShortEdges(std::vector<Vector3>* crossPoints,
        std::vector<Vector2>* crossPointUvs,
        std::unordered_map<size_t, std::unordered_set<size_t>>* edgeConnectMap);
    bool collapseTriangles(std::vector<Vector3>* crossPoints,
        std::vector<Vector2>* crossPointUvs,
        std::unordered_map<size_t, std::unordered_set<size_t>>* edgeConnectMap);
    bool removeSingleEndpoints(std::vector<Vector3>* crossPoints,
        std::unordered_map<size_t, std::unordered_set<size_t>>* edgeConnectMap);
    void collapseEdge(std::vector<Vector3>* crossPoints,
        std::vector<Vector2>* crossPointUvs,
        std::unordered_map<size_t, std::unordered_set<size_t>>* edgeConnectMap,
        const std::pair<size_t, size_t>& edge);
    void extractMesh(std::vector<Vector3>& points,
        const std::vector<Vector2>& pointUvs,
        const std::vector<size_t>& pointSourceTriangles,
        std::unordered_map<size_t, std::unordered_set<size_t>>& edgeConnectMap,
        std::vector<std::vector<size_t>>* quads);
    void simplifyGraph(std::unordered_map<size_t, std::unordered_set<size_t>>& graph);
    void searchBoundaries(const std::set<std::pair<size_t, size_t>>& halfEdges,
        std::vector<std::vector<size_t>>* loops);
    void fixHoleWithQuads(std::vector<size_t>& hole, bool checkScore = true);
    bool testPointInTriangle(const std::vector<Vector3>& points,
        const std::vector<size_t>& triangle,
        const std::vector<size_t>& testPoints);
    bool removeIsolatedFaces();
    void smoothAndProject(size_t iterations,
        const std::unordered_set<size_t>* movableVertices = nullptr);
    void smoothAroundVertices(const std::unordered_set<size_t>& seedVertices,
        size_t rings, size_t iterations);
    void splitSixEdgeFaces();
    void switchHighValenceEdges();
    void convertTriangleAndFiveEdgeFans();
    void collapseThreeValenceDiagonals();
    void mergeDoubleSharedEdgeQuads();
    void mergeThreeAndFiveValenceTriangles();
    void collapseThreeValenceCorners();
    void splitHighValenceTriangleFans();
    void collapseThreeValenceEdgePairs();
    void cleanupTriangles();
    void splitSevenEdgeFaces();
    // Merges one edge per pass over the whole polygon set, so it reports from
    // inside its round loop instead of running silently to completion.
    void mergeSharedFiveEdgeFaces(const ProgressHandler* progressHandler = nullptr);
    bool removeNonManifoldFaces();
    // Last thing extract() does: weld zero length edges, drop degenerate and
    // duplicate faces, and remove whatever still makes an edge shared by three
    // faces.  Everything downstream (statistics, the OBJ writer, the caller's
    // own measurement) then sees the same mesh.
    void validateRemeshedMesh();
    // Merges the two ends of an over-long side and renumbers the faces that named
    // the absorbed vertex, up to four rounds; runs from validateRemeshedMesh()'s
    // caller so it sees a mesh the validate pass already made manifold.
    void clampQuadAspect(double maximumAspect);
    void rebuildHalfEdges();
    // One hole really got its faces appended: counted, and the faces it added go
    // into the running total.  A pass that only re-walks a loop an earlier pass
    // already capped adds nothing, which is what keeps the counters from doubling.
    void recordFilledHole(size_t facesAdded)
    {
        if (0 == facesAdded)
            return;
        ++m_extractionStats.holesFilled;
        m_extractionStats.holeFacesAdded += facesAdded;
    }
    // --- uv bookkeeping -------------------------------------------------------
    // Every pass that adds, drops or renumbers a remeshed vertex calls one of
    // these in the same place the vertex array changes, so m_remeshedVertexUvs
    // stays index aligned with m_remeshedVertices through the whole extract()
    // run.  A pass that finds the two already out of step clears the uv array:
    // a missing uv degrades to "no uv", a wrong one would cut a seam through the
    // middle of a face.
    Vector2 remeshedVertexUv(size_t vertex) const
    {
        if (vertex >= m_remeshedVertexUvs.size())
            return Vector2();
        return m_remeshedVertexUvs[vertex];
    }
    // Linear uv at a point that lies on the segment between two existing points,
    // taken from where the point falls along that segment.
    static Vector2 uvOnSegment(const std::vector<Vector3>& positions,
        const std::vector<Vector2>& uvs, size_t first, size_t second,
        const Vector3& position)
    {
        Vector2 firstUv;
        Vector2 secondUv;
        if (first < uvs.size())
            firstUv = uvs[first];
        if (second < uvs.size())
            secondUv = uvs[second];
        const Vector3 span = positions[second] - positions[first];
        const double spanSquared = Vector3::dotProduct(span, span);
        double ratio = 0.5;
        if (!Double::isZero(spanSquared))
            ratio = Vector3::dotProduct(position - positions[first], span) / spanSquared;
        if (ratio < 0.0)
            ratio = 0.0;
        else if (ratio > 1.0)
            ratio = 1.0;
        return firstUv * (1.0 - ratio) + secondUv * ratio;
    }
    // Called right after m_remeshedVertices grew by one.
    void pushRemeshedVertexUv(const Vector2& uv)
    {
        if (m_remeshedVertexUvs.size() + 1 != m_remeshedVertices.size()) {
            m_remeshedVertexUvs.clear();
            return;
        }
        m_remeshedVertexUvs.push_back(uv);
    }
    // Called right after m_remeshedVertices lost its last entry.
    void dropRemeshedVertexUv()
    {
        if (m_remeshedVertexUvs.size() != m_remeshedVertices.size() + 1) {
            m_remeshedVertexUvs.clear();
            return;
        }
        m_remeshedVertexUvs.pop_back();
    }
    // Called after m_remeshedVertices was resized down to count.
    void truncateRemeshedVertexUvs(size_t count)
    {
        if (m_remeshedVertexUvs.size() < count) {
            m_remeshedVertexUvs.clear();
            return;
        }
        m_remeshedVertexUvs.resize(count);
    }
    // Overwrites one uv, after a merge moved that vertex onto the average of the
    // pair it absorbed.
    void setRemeshedVertexUv(size_t vertex, const Vector2& uv)
    {
        if (vertex >= m_remeshedVertexUvs.size()) {
            m_remeshedVertexUvs.clear();
            return;
        }
        m_remeshedVertexUvs[vertex] = uv;
    }
    // Applies an old index to new index table (max size_t for a dropped vertex)
    // to the uv array, which still holds the order before the compaction.
    void remapRemeshedVertexUvs(const std::vector<size_t>& oldToNew)
    {
        if (m_remeshedVertexUvs.size() != oldToNew.size()) {
            m_remeshedVertexUvs.clear();
            return;
        }
        std::vector<Vector2> remapped(m_remeshedVertices.size(), Vector2());
        for (size_t old = 0; old < oldToNew.size(); ++old) {
            if (oldToNew[old] >= remapped.size())
                continue;
            remapped[oldToNew[old]] = m_remeshedVertexUvs[old];
        }
        m_remeshedVertexUvs = std::move(remapped);
    }
    // compaction, ascending order: extract() itself drops the vertices no face
    // refers to any more and rewrites its own faces from oldToNew.
    void compactRemeshedVerticesAscending(const std::set<size_t>& usedVertices,
        std::unordered_map<size_t, size_t>* oldToNew)
    {
        std::vector<Vector3> compactedVertices;
        std::vector<Vector2> compactedUvs;
        const bool uvsInStep = m_remeshedVertexUvs.size() == m_remeshedVertices.size();
        compactedVertices.reserve(usedVertices.size());
        if (uvsInStep)
            compactedUvs.reserve(usedVertices.size());
        for (const auto& oldIndex : usedVertices) {
            (*oldToNew)[oldIndex] = compactedVertices.size();
            compactedVertices.push_back(m_remeshedVertices[oldIndex]);
            if (uvsInStep)
                compactedUvs.push_back(m_remeshedVertexUvs[oldIndex]);
        }
        m_remeshedVertices = std::move(compactedVertices);
        if (uvsInStep)
            m_remeshedVertexUvs = std::move(compactedUvs);
        else
            m_remeshedVertexUvs.clear();
    }
    // Applies the ascending used-mask remap the patch pass builds to the uv
    // array, after it replaced m_remeshedVertices with the compacted vertices.
    void remapRemeshedVertexUvs(const std::vector<char>& used,
        const std::vector<size_t>& oldToNew)
    {
        if (m_remeshedVertexUvs.size() != used.size()) {
            m_remeshedVertexUvs.clear();
            return;
        }
        std::vector<Vector2> remapped(m_remeshedVertices.size(), Vector2());
        for (size_t old = 0; old < used.size(); ++old) {
            if (!used[old])
                continue;
            remapped[oldToNew[old]] = m_remeshedVertexUvs[old];
        }
        m_remeshedVertexUvs = std::move(remapped);
    }
    // compaction, first-use order: the four cleanup passes that keep using the
    // table afterwards all share this one.
    std::map<size_t, size_t> compactRemeshedVertices()
    {
        std::map<size_t, size_t> oldToNew;
        std::vector<Vector3> compactedVertices;
        std::vector<Vector2> compactedUvs;
        const bool uvsInStep = m_remeshedVertexUvs.size() == m_remeshedVertices.size();
        compactedVertices.reserve(m_remeshedVertices.size());
        if (uvsInStep)
            compactedUvs.reserve(m_remeshedVertices.size());
        for (const auto& face : m_remeshedPolygons) {
            for (const auto vertex : face) {
                if (oldToNew.end() != oldToNew.find(vertex))
                    continue;
                oldToNew[vertex] = compactedVertices.size();
                compactedVertices.push_back(m_remeshedVertices[vertex]);
                if (uvsInStep)
                    compactedUvs.push_back(m_remeshedVertexUvs[vertex]);
            }
        }
        for (auto& face : m_remeshedPolygons) {
            for (auto& vertex : face)
                vertex = oldToNew.at(vertex);
        }
        m_remeshedVertices = std::move(compactedVertices);
        if (uvsInStep)
            m_remeshedVertexUvs = std::move(compactedUvs);
        else
            m_remeshedVertexUvs.clear();
        return oldToNew;
    }
    // compaction, first-use order, reporting the per old index table (max size_t
    // for a vertex that went) for the passes that remap their own vertex lists.
    std::vector<size_t> compactRemeshedVerticesIndexed()
    {
        const size_t absent = std::numeric_limits<size_t>::max();
        std::vector<size_t> oldToNew(m_remeshedVertices.size(), absent);
        std::vector<Vector3> compactedVertices;
        std::vector<Vector2> compactedUvs;
        const bool uvsInStep = m_remeshedVertexUvs.size() == m_remeshedVertices.size();
        compactedVertices.reserve(m_remeshedVertices.size());
        if (uvsInStep)
            compactedUvs.reserve(m_remeshedVertices.size());
        for (const auto& face : m_remeshedPolygons) {
            for (const auto vertex : face) {
                if (absent != oldToNew[vertex])
                    continue;
                oldToNew[vertex] = compactedVertices.size();
                compactedVertices.push_back(m_remeshedVertices[vertex]);
                if (uvsInStep)
                    compactedUvs.push_back(m_remeshedVertexUvs[vertex]);
            }
        }
        for (auto& face : m_remeshedPolygons)
            for (auto& vertex : face)
                vertex = oldToNew[vertex];
        m_remeshedVertices = std::move(compactedVertices);
        if (uvsInStep)
            m_remeshedVertexUvs = std::move(compactedUvs);
        else
            m_remeshedVertexUvs.clear();
        return oldToNew;
    }
    void fixHoles();
};

}

#endif
