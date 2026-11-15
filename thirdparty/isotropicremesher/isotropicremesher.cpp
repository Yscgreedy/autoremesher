/*
 *  Copyright (c) 2020-2021 Jeremy HU <jeremy-at-dust3d dot org>. All rights reserved. 
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
#include <string>
#include <iostream>
#include <cmath>
#include "isotropicremesher.h"
#include "isotropichalfedgemesh.h"
#if defined(__has_include)
#if __has_include(<oneapi/tbb/blocked_range.h>)
#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_for.h>
#else
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#endif
#else
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#endif
#include <unordered_map>

IsotropicRemesher::~IsotropicRemesher()
{
    delete m_halfedgeMesh;
    delete m_axisAlignedBoundingBoxTree;
    delete m_triangleBoxes;
    delete m_triangleNormals;
}

void IsotropicRemesher::setSharpEdgeIncludedAngle(double degrees)
{
    m_sharpEdgeThresholdRadians = (180 - degrees) * (M_PI / 180.0);
}

void IsotropicRemesher::setTargetEdgeLength(double edgeLength)
{
    m_targetEdgeLength = edgeLength;
}

void IsotropicRemesher::setSmoothNormalDegrees(double degrees)
{
    m_smoothNormalDegrees = degrees;
}

Vector3 IsotropicRemesher::pnTriangleEdgeMidpoint(const Vector3 &p1, const Vector3 &p2,
    const Vector3 &n1, const Vector3 &n2)
{
    double d12 = Vector3::dotProduct(p2 - p1, n1);
    double d21 = Vector3::dotProduct(p1 - p2, n2);
    Vector3 p210 = (2.0 * p1 + p2 - d12 * n1) / 3.0;
    Vector3 p120 = (2.0 * p2 + p1 - d21 * n2) / 3.0;
    return (p1 + p2 + 3.0 * (p210 + p120)) / 8.0;
}

void IsotropicRemesher::setVertexTargetEdgeLengths(const std::vector<double> *targetLengths)
{
    m_vertexTargetEdgeLengths = targetLengths;
}

void IsotropicRemesher::setTargetTriangleCount(size_t triangleCount)
{
    m_targetTriangleCount = triangleCount;
}

void IsotropicRemesher::computeSmoothVertexNormals()
{
    m_smoothVertexNormals.assign(m_vertices->size(), Vector3());

    // Compute face normals and build adjacency
    std::vector<Vector3> faceNormals(m_triangles->size());
    std::vector<std::vector<size_t>> facesAroundVertex(m_vertices->size());
    for (size_t i = 0; i < m_triangles->size(); ++i) {
        const auto &tri = (*m_triangles)[i];
        faceNormals[i] = Vector3::normal((*m_vertices)[tri[0]],
            (*m_vertices)[tri[1]], (*m_vertices)[tri[2]]);
        for (size_t j = 0; j < 3; ++j)
            facesAroundVertex[tri[j]].push_back(i);
    }

    double thresholdRadians = m_smoothNormalDegrees * M_PI / 180.0;

    for (size_t vi = 0; vi < m_vertices->size(); ++vi) {
        const auto &incidentFaces = facesAroundVertex[vi];
        if (incidentFaces.empty())
            continue;

        // Compute angle-area weighted normal from all incident faces
        // within the angle threshold
        Vector3 weightedNormal;
        for (size_t fi : incidentFaces) {
            const auto &tri = (*m_triangles)[fi];
            double area = Vector3::area((*m_vertices)[tri[0]],
                (*m_vertices)[tri[1]], (*m_vertices)[tri[2]]);

            // Find the corner angle at this vertex for this triangle
            double cornerAngle = 0.0;
            if (tri[0] == vi)
                cornerAngle = Vector3::angle((*m_vertices)[tri[1]] - (*m_vertices)[tri[0]],
                    (*m_vertices)[tri[2]] - (*m_vertices)[tri[0]]);
            else if (tri[1] == vi)
                cornerAngle = Vector3::angle((*m_vertices)[tri[0]] - (*m_vertices)[tri[1]],
                    (*m_vertices)[tri[2]] - (*m_vertices)[tri[1]]);
            else
                cornerAngle = Vector3::angle((*m_vertices)[tri[0]] - (*m_vertices)[tri[2]],
                    (*m_vertices)[tri[1]] - (*m_vertices)[tri[2]]);

            weightedNormal += faceNormals[fi] * area * cornerAngle;
        }

        if (!weightedNormal.isZero())
            weightedNormal.normalize();

        // If threshold > 0, refine by excluding faces whose normals deviate
        // too far from the initial weighted average
        if (thresholdRadians > 0.0 && !weightedNormal.isZero()) {
            Vector3 refinedNormal;
            for (size_t fi : incidentFaces) {
                double angle = Vector3::angle(faceNormals[fi], weightedNormal);
                if (angle <= thresholdRadians) {
                    const auto &tri = (*m_triangles)[fi];
                    double area = Vector3::area((*m_vertices)[tri[0]],
                        (*m_vertices)[tri[1]], (*m_vertices)[tri[2]]);
                    double cornerAngle = 0.0;
                    if (tri[0] == vi)
                        cornerAngle = Vector3::angle((*m_vertices)[tri[1]] - (*m_vertices)[tri[0]],
                            (*m_vertices)[tri[2]] - (*m_vertices)[tri[0]]);
                    else if (tri[1] == vi)
                        cornerAngle = Vector3::angle((*m_vertices)[tri[0]] - (*m_vertices)[tri[1]],
                            (*m_vertices)[tri[2]] - (*m_vertices)[tri[1]]);
                    else
                        cornerAngle = Vector3::angle((*m_vertices)[tri[0]] - (*m_vertices)[tri[2]],
                            (*m_vertices)[tri[1]] - (*m_vertices)[tri[2]]);
                    refinedNormal += faceNormals[fi] * area * cornerAngle;
                }
            }
            if (!refinedNormal.isZero())
                m_smoothVertexNormals[vi] = refinedNormal.normalized();
        } else if (!weightedNormal.isZero()) {
            m_smoothVertexNormals[vi] = weightedNormal;
        }
    }
}

void IsotropicRemesher::subdivideMeshWithPNTriangles()
{
    m_smoothVertices.clear();
    m_smoothTriangles.clear();
    m_smoothTriangleNormals.clear();

    if (m_smoothVertexNormals.size() != m_vertices->size())
        return;

    // Reserve capacity: 3 edges + 1 center = 4 new verts per triangle, but
    // edge midpoints are shared between adjacent triangles. For simplicity
    // we create unique vertices per sub-triangle (higher vert count but
    // simpler indexing).
    m_smoothVertices.reserve(m_vertices->size() + m_triangles->size() * 3);
    m_smoothTriangles.reserve(m_triangles->size() * 4);
    m_smoothTriangleNormals.reserve(m_triangles->size() * 4);

    // Copy original vertices as the base set
    m_smoothVertices = *m_vertices;

    for (size_t i = 0; i < m_triangles->size(); ++i) {
        const auto &tri = (*m_triangles)[i];
        size_t i0 = tri[0], i1 = tri[1], i2 = tri[2];

        const Vector3 &p0 = (*m_vertices)[i0];
        const Vector3 &p1 = (*m_vertices)[i1];
        const Vector3 &p2 = (*m_vertices)[i2];
        const Vector3 &n0 = m_smoothVertexNormals[i0];
        const Vector3 &n1 = m_smoothVertexNormals[i1];
        const Vector3 &n2 = m_smoothVertexNormals[i2];

        // Compute PN Triangle edge midpoints
        Vector3 e01 = pnTriangleEdgeMidpoint(p0, p1, n0, n1);
        Vector3 e12 = pnTriangleEdgeMidpoint(p1, p2, n1, n2);
        Vector3 e20 = pnTriangleEdgeMidpoint(p2, p0, n2, n0);

        // Add new vertices
        size_t ie01 = m_smoothVertices.size(); m_smoothVertices.push_back(e01);
        size_t ie12 = m_smoothVertices.size(); m_smoothVertices.push_back(e12);
        size_t ie20 = m_smoothVertices.size(); m_smoothVertices.push_back(e20);

        // Four sub-triangles
        std::vector<std::vector<size_t>> subTris = {
            {i0, ie01, ie20},
            {i1, ie12, ie01},
            {i2, ie20, ie12},
            {ie01, ie12, ie20}
        };

        for (const auto &subTri : subTris) {
            m_smoothTriangles.push_back(subTri);
            m_smoothTriangleNormals.push_back(
                Vector3::normal(m_smoothVertices[subTri[0]],
                    m_smoothVertices[subTri[1]],
                    m_smoothVertices[subTri[2]])
            );
        }
    }
}

void IsotropicRemesher::buildAxisAlignedBoundingBoxTree()
{
    m_triangleBoxes = new std::vector<AxisAlignedBoudingBox>(m_triangles->size());
    
    for (size_t i = 0; i < (*m_triangleBoxes).size(); ++i) {
        addTriagleToAxisAlignedBoundingBox((*m_triangles)[i], &(*m_triangleBoxes)[i]);
        (*m_triangleBoxes)[i].updateCenter();
    }
    
    std::vector<size_t> faceIndices;
    for (size_t i = 0; i < (*m_triangleBoxes).size(); ++i)
        faceIndices.push_back(i);
    
    AxisAlignedBoudingBox groupBox;
    for (const auto &i: faceIndices) {
        addTriagleToAxisAlignedBoundingBox((*m_triangles)[i], &groupBox);
    }
    groupBox.updateCenter();
    
    delete m_axisAlignedBoundingBoxTree;
    m_axisAlignedBoundingBoxTree = new AxisAlignedBoudingBoxTree(m_triangleBoxes, 
        faceIndices, groupBox);
}

IsotropicRemesher::IsotropicRemesher(const std::vector<Vector3> *vertices,
        const std::vector<std::vector<size_t>> *triangles) :
    m_vertices(vertices),
    m_triangles(triangles)
{
    m_halfedgeMesh = new IsotropicHalfedgeMesh(*m_vertices, *m_triangles);
    m_initialAverageEdgeLength = m_halfedgeMesh->averageEdgeLength();
}

double IsotropicRemesher::initialAverageEdgeLength()
{
    return m_initialAverageEdgeLength;
}

void IsotropicRemesher::remesh(size_t iteration)
{
    const auto report = [this](float fraction, const char *name) {
        if (m_progressHandler)
            m_progressHandler(fraction, name);
    };

    report(0.0f, "Building bounding volume tree");
    delete m_triangleNormals;
    m_triangleNormals = new std::vector<Vector3>;
    m_triangleNormals->reserve(m_triangles->size());
    for (const auto &it: *m_triangles) {
        m_triangleNormals->push_back(
            Vector3::normal((*m_vertices)[it[0]],
                (*m_vertices)[it[1]],
                (*m_vertices)[it[2]])
        );
    }

    double targetLength = m_targetEdgeLength > 0 ? m_targetEdgeLength : m_initialAverageEdgeLength;

    if (m_targetTriangleCount > 0) {
        double totalArea = 0.0;
        for (const auto &it: *m_triangles) {
            totalArea += Vector3::area((*m_vertices)[it[0]], (*m_vertices)[it[1]], (*m_vertices)[it[2]]);
        }
        double triangleArea = totalArea / m_targetTriangleCount;
        targetLength = std::sqrt(triangleArea / (0.86602540378 * 0.5));
    }

    double minTargetLength = 4.0 / 5.0 * targetLength;
    double maxTargetLength= 4.0 / 3.0 * targetLength;

    double minTargetLengthSquared = minTargetLength * minTargetLength;
    double maxTargetLengthSquared = maxTargetLength * maxTargetLength;

    // If smooth normal threshold is set, compute smooth normals and
    // subdivide the input mesh using PN Triangle evaluation so that
    // the projection step projects onto a smooth curved surface.
    if (m_smoothNormalDegrees > 0.0) {
        // Compute per-vertex smooth normals on the input mesh
        computeSmoothVertexNormals();

        // Set smooth normals on the halfedge mesh vertices
        IsotropicHalfedgeMesh::Vertex *vertex = m_halfedgeMesh->moveToNextVertex(nullptr);
        for (size_t vi = 0; vertex != nullptr && vi < m_smoothVertexNormals.size();
             vertex = m_halfedgeMesh->moveToNextVertex(vertex), ++vi) {
            vertex->_smoothNormal = m_smoothVertexNormals[vi];
        }

        // Subdivide the input mesh using PN Triangle evaluation
        subdivideMeshWithPNTriangles();

        // Build AABB tree from the subdivided (smooth) mesh
        m_triangleBoxes = new std::vector<AxisAlignedBoudingBox>(m_smoothTriangles.size());
        for (size_t i = 0; i < m_triangleBoxes->size(); ++i) {
            const auto &tri = m_smoothTriangles[i];
            for (size_t j = 0; j < 3; ++j)
                (*m_triangleBoxes)[i].update(m_smoothVertices[tri[j]]);
            (*m_triangleBoxes)[i].updateCenter();
        }
        std::vector<size_t> faceIndices;
        for (size_t i = 0; i < m_triangleBoxes->size(); ++i)
            faceIndices.push_back(i);
        AxisAlignedBoudingBox groupBox;
        for (const auto &i: faceIndices) {
            const auto &tri = m_smoothTriangles[i];
            for (size_t j = 0; j < 3; ++j)
                groupBox.update(m_smoothVertices[tri[j]]);
        }
        groupBox.updateCenter();
        delete m_axisAlignedBoundingBoxTree;
        m_axisAlignedBoundingBoxTree = new AxisAlignedBoudingBoxTree(m_triangleBoxes,
            faceIndices, groupBox);
    } else {
        buildAxisAlignedBoundingBoxTree();
    }
    
    // Apply per-vertex target edge lengths if provided
    if (m_vertexTargetEdgeLengths != nullptr) {
        IsotropicHalfedgeMesh::Vertex *vertex = m_halfedgeMesh->moveToNextVertex(nullptr);
        for (size_t vi = 0; vertex != nullptr && vi < m_vertexTargetEdgeLengths->size();
             vertex = m_halfedgeMesh->moveToNextVertex(vertex), ++vi) {
            vertex->targetEdgeLength = (*m_vertexTargetEdgeLengths)[vi];
        }
    }

    report(0.15f, "Splitting long edges");
    bool skipSplitOnce = true;
    if (m_sharpEdgeThresholdRadians > 0) {
        splitLongEdges(maxTargetLengthSquared);
        m_halfedgeMesh->updateTriangleNormals();
        m_halfedgeMesh->featureEdges(m_sharpEdgeThresholdRadians);
    } else {
        splitLongEdges(maxTargetLengthSquared);
        m_halfedgeMesh->featureBoundaries();
    }
    
    // The five passes below repeat once per iteration, so the remaining 0.25..1
    // splits evenly across all of them.
    const size_t passCount = iteration * 5;
    size_t pass = 0;
    const auto reportPass = [&](const char *name) {
        report(0 == passCount ? 1.0f : 0.25f + 0.75f * (float)pass++ / passCount, name);
    };

    for (size_t i = 0; i < iteration; ++i) {
        //std::cout << "iteration:" << i << std::endl;
        reportPass("Splitting long edges");
        if (skipSplitOnce) {
            skipSplitOnce = false;
        } else {
            //std::cout << "Split long edges" << std::endl;
            splitLongEdges(maxTargetLengthSquared);
        }
        //std::cout << "Collapse short edges" << std::endl;
        reportPass("Collapsing short edges");
        collapseShortEdges(minTargetLengthSquared, maxTargetLengthSquared);
        //std::cout << "Flip edges" << std::endl;
        reportPass("Flipping edges");
        flipEdges();
        //std::cout << "Shift vertices" << std::endl;
        reportPass("Shifting vertices");
        shiftVertices();
        //std::cout << "Project vertices" << std::endl;
        reportPass("Projecting vertices");
        projectVertices();

    }
    report(1.0f, "");
    
    //std::cout << "Done" << std::endl;
}

void IsotropicRemesher::splitLongEdges(double maxEdgeLengthSquared)
{
    for (IsotropicHalfedgeMesh::Face *face = m_halfedgeMesh->moveToNextFace(nullptr);
            nullptr != face;
            ) {
        const auto &startHalfedge = face->halfedge;
        face = m_halfedgeMesh->moveToNextFace(face);
        IsotropicHalfedgeMesh::Halfedge *halfedge = startHalfedge;
        do {
            const auto &nextHalfedge = halfedge->nextHalfedge;
            double lengthSquared = (halfedge->startVertex->position - nextHalfedge->startVertex->position).lengthSquared();
            double edgeMaxLenSq = maxEdgeLengthSquared;
            double t0 = halfedge->startVertex->targetEdgeLength;
            double t1 = nextHalfedge->startVertex->targetEdgeLength;
            if (t0 > 0.0 && t1 > 0.0) {
                double edgeTarget = (t0 + t1) * 0.5;
                edgeMaxLenSq = std::pow(4.0 / 3.0 * edgeTarget, 2);
            }
            if (lengthSquared > edgeMaxLenSq) {
                m_halfedgeMesh->breakEdge(halfedge);
                break;
            }
            halfedge = nextHalfedge;
        } while (halfedge != startHalfedge);
    }
}

IsotropicHalfedgeMesh *IsotropicRemesher::remeshedHalfedgeMesh()
{
    return m_halfedgeMesh;
}

void IsotropicRemesher::collapseShortEdges(double minEdgeLengthSquared, double maxEdgeLengthSquared)
{
    for (IsotropicHalfedgeMesh::Face *face = m_halfedgeMesh->moveToNextFace(nullptr);
            nullptr != face;
            ) {
        if (face->removed) {
            face = m_halfedgeMesh->moveToNextFace(face);
            continue;
        }
        size_t f = face->debugIndex;
        const auto &startHalfedge = face->halfedge;
        face = m_halfedgeMesh->moveToNextFace(face);
        IsotropicHalfedgeMesh::Halfedge *halfedge = startHalfedge;
        size_t loopCount = 0;
        do {
            const auto &nextHalfedge = halfedge->nextHalfedge;
            double lengthSquared = (halfedge->startVertex->position - nextHalfedge->startVertex->position).lengthSquared();
            double edgeMinLenSq = minEdgeLengthSquared;
            double edgeMaxLenSq = maxEdgeLengthSquared;
            double t0 = halfedge->startVertex->targetEdgeLength;
            double t1 = nextHalfedge->startVertex->targetEdgeLength;
            if (t0 > 0.0 && t1 > 0.0) {
                double edgeTarget = (t0 + t1) * 0.5;
                edgeMinLenSq = std::pow(4.0 / 5.0 * edgeTarget, 2);
                edgeMaxLenSq = std::pow(4.0 / 3.0 * edgeTarget, 2);
            }
            if (lengthSquared < edgeMinLenSq) {
                if (!halfedge->startVertex->featured && !nextHalfedge->startVertex->featured) {
                    if (m_halfedgeMesh->collapseEdge(halfedge, edgeMaxLenSq)) {
                        break;
                    }
                }
            }
            halfedge = nextHalfedge;
        } while (halfedge != startHalfedge);
    }
}

void IsotropicRemesher::flipEdges()
{
    for (IsotropicHalfedgeMesh::Face *face = m_halfedgeMesh->moveToNextFace(nullptr); 
            nullptr != face; 
            ) {
        const auto &startHalfedge = face->halfedge;
        face = m_halfedgeMesh->moveToNextFace(face);
        IsotropicHalfedgeMesh::Halfedge *halfedge = startHalfedge;
        do {
            const auto &nextHalfedge = halfedge->nextHalfedge;
            if (nullptr != halfedge->oppositeHalfedge) {
                //if (!halfedge->startVertex->featured && !nextHalfedge->startVertex->featured) {
                    if (m_halfedgeMesh->flipEdge(halfedge)) {
                        break;
                    }
                //}
            }
            halfedge = nextHalfedge;
        } while (halfedge != startHalfedge);
    }
}

void IsotropicRemesher::shiftVertices()
{
    m_halfedgeMesh->updateVertexValences();
    m_halfedgeMesh->updateTriangleNormals();
    m_halfedgeMesh->updateVertexNormals();
    
    for (IsotropicHalfedgeMesh::Vertex *vertex = m_halfedgeMesh->moveToNextVertex(nullptr); 
            nullptr != vertex;
            vertex = m_halfedgeMesh->moveToNextVertex(vertex)) {
        //if (vertex->featured)
        //    continue;
        m_halfedgeMesh->relaxVertex(vertex);
    }
}

void IsotropicRemesher::projectVertices()
{
    // Use subdivided (smooth) mesh data for projection when available
    const std::vector<Vector3> *projVertices = m_smoothVertices.empty() ? m_vertices : &m_smoothVertices;
    const std::vector<std::vector<size_t>> *projTriangles = m_smoothTriangles.empty() ? m_triangles : &m_smoothTriangles;
    const std::vector<Vector3> *projNormals = m_smoothTriangleNormals.empty() ? m_triangleNormals : &m_smoothTriangleNormals;

    // Snapshot first, project after.  The sequential version let a vertex see its
    // neighbours as they were *after* they had been projected, so the result
    // depended on the traversal order; reading a snapshot removes that dependence,
    // which is both what makes the pass thread safe and what makes it deterministic
    // at any thread count.  The one-ring walk reaches neighbours through halfedges,
    // so their positions are looked up by pointer.
    std::vector<IsotropicHalfedgeMesh::Vertex *> vertices;
    std::vector<Vector3> positions;
    // No vertexCount() on the halfedge mesh: the list is built once and the count
    // is not on any hot path, so it is measured rather than stored.
    vertices.reserve(4096);
    positions.reserve(4096);
    for (IsotropicHalfedgeMesh::Vertex *vertex = m_halfedgeMesh->moveToNextVertex(nullptr);
            nullptr != vertex;
            vertex = m_halfedgeMesh->moveToNextVertex(vertex)) {
        vertices.push_back(vertex);
        positions.push_back(vertex->position);
    }
    std::unordered_map<const IsotropicHalfedgeMesh::Vertex *, size_t> indexOf;
    indexOf.reserve(vertices.size() * 2);
    for (size_t i = 0; i < vertices.size(); ++i)
        indexOf[vertices[i]] = i;
    const auto positionOf = [&positions, &indexOf](const IsotropicHalfedgeMesh::Vertex *vertex) {
        const auto found = indexOf.find(vertex);
        return indexOf.end() == found ? Vector3() : positions[found->second];
    };

    const auto projectOne = [&](size_t index) {
        IsotropicHalfedgeMesh::Vertex *vertex = vertices[index];
        if (vertex->featured)
            return;
        const auto &startHalfedge = vertex->firstHalfedge;
        if (nullptr == startHalfedge)
            return;
        const Vector3 vertexPosition = positions[index];

        std::vector<AxisAlignedBoudingBox> rayBox(1);
        auto &box = rayBox[0];
        box.update(vertexPosition);

        IsotropicHalfedgeMesh::Halfedge *loopHalfedge = startHalfedge;
        do {
            box.update(positionOf(loopHalfedge->nextHalfedge->startVertex));
            if (nullptr == loopHalfedge->oppositeHalfedge) {
                loopHalfedge = startHalfedge;
                do {
                    box.update(positionOf(loopHalfedge->previousHalfedge->startVertex));
                    loopHalfedge = loopHalfedge->previousHalfedge->oppositeHalfedge;
                    if (nullptr == loopHalfedge)
                        break;
                } while (loopHalfedge != startHalfedge);
                break;
            }
            loopHalfedge = loopHalfedge->oppositeHalfedge->nextHalfedge;
        } while (loopHalfedge != startHalfedge);

        // One box query, answered by descending the existing tree instead of
        // building a throwaway tree for every vertex.
        std::vector<size_t> candidates;
        m_axisAlignedBoundingBoxTree->query(box, &candidates);

        std::vector<std::pair<Vector3, double>> hits;

        auto boundingBoxSize = box.upperBound() - box.lowerBound();
        Vector3 segment = vertex->_normal * (boundingBoxSize[0] + boundingBoxSize[1] + boundingBoxSize[2]);
        for (const size_t it : candidates) {
            // The segment runs both ways along the normal and reaches well
            // past the one ring, so on thin parts it also hits the surface
            // facing the other way. Landing there tears the neighborhood open.
            if (Vector3::dotProduct((*projNormals)[it], vertex->_normal) <= 0)
                continue;
            const auto &triangle = (*projTriangles)[it];
            std::vector<Vector3> trianglePositions = {
                (*projVertices)[triangle[0]],
                (*projVertices)[triangle[1]],
                (*projVertices)[triangle[2]]
            };
            Vector3 intersection;
            if (Vector3::intersectSegmentAndPlane(vertexPosition - segment, vertexPosition + segment,
                    trianglePositions[0],
                    (*projNormals)[it],
                    &intersection)) {
                std::vector<Vector3> normals;
                for (size_t i = 0; i < 3; ++i) {
                    size_t j = (i + 1) % 3;
                    normals.push_back(Vector3::normal(intersection, trianglePositions[i], trianglePositions[j]));
                }
                if (Vector3::dotProduct(normals[0], normals[1]) > 0 &&
                        Vector3::dotProduct(normals[0], normals[2]) > 0) {
                    hits.push_back({intersection, (vertexPosition - intersection).lengthSquared()});
                }
            }
        }

        if (!hits.empty()) {
            vertex->position = std::min_element(hits.begin(), hits.end(), [](const std::pair<Vector3, double> &first,
                    const std::pair<Vector3, double> &second) {
                return first.second < second.second;
            })->first;
        }
    };

    // Each task writes only the vertex it was handed, and the tree query is const,
    // so the only shared state is the read-only snapshot.
    if (vertices.size() >= 512) {
        tbb::parallel_for(tbb::blocked_range<size_t>(0, vertices.size(), 64),
            [&](const tbb::blocked_range<size_t> &range) {
                for (size_t index = range.begin(); index != range.end(); ++index)
                    projectOne(index);
            });
    } else {
        for (size_t index = 0; index < vertices.size(); ++index)
            projectOne(index);
    }
}
