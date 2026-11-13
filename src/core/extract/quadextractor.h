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
#include <memory>
#include <map>
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

    const std::vector<std::vector<size_t>>& remeshedQuads()
    {
        return m_remeshedPolygons;
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

    // Local topology optimisation: merges triangle pairs into quads and splits
    // the larger faces, judged by one global energy (face size + vertex valence)
    // instead of the per pass heuristics.
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
    bool m_localRemesh = false;
    ProgressHandler m_progressHandler;
    // Ear clipping over the loop's best fit plane followed by quad pairing; adds
    // the faces it can and returns how many it added.
    size_t fillHoleGraded(const std::vector<size_t>& hole);
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
        std::set<std::pair<size_t, size_t>>* connections);
    void holdSingularLines(std::vector<Vector3>* crossPoints,
        std::vector<size_t>* sourceTriangles,
        std::set<std::pair<size_t, size_t>>* connections);
    void extractEdges(const std::set<std::pair<size_t, size_t>>& connections,
        std::unordered_map<size_t, std::unordered_set<size_t>>* edgeConnectMap);
    bool collapseShortEdges(std::vector<Vector3>* crossPoints,
        std::unordered_map<size_t, std::unordered_set<size_t>>* edgeConnectMap);
    bool collapseTriangles(std::vector<Vector3>* crossPoints,
        std::unordered_map<size_t, std::unordered_set<size_t>>* edgeConnectMap);
    bool removeSingleEndpoints(std::vector<Vector3>* crossPoints,
        std::unordered_map<size_t, std::unordered_set<size_t>>* edgeConnectMap);
    void collapseEdge(std::vector<Vector3>* crossPoints,
        std::unordered_map<size_t, std::unordered_set<size_t>>* edgeConnectMap,
        const std::pair<size_t, size_t>& edge);
    void extractMesh(std::vector<Vector3>& points,
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
    void rebuildHalfEdges();
    void fixHoles();
};

}

#endif
