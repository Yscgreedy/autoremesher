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
#ifndef AUTO_REMESHER_QUAD_PARAMETERIZER_H
#define AUTO_REMESHER_QUAD_PARAMETERIZER_H

#include <AutoRemesher/Progress>
#include <AutoRemesher/Vector2>
#include <AutoRemesher/Vector3>
#include <cstddef>
#include <vector>

namespace AutoRemesher {

class QuadParameterizer {
public:
    struct Result {
        std::vector<std::vector<Vector2>> triangleUvs;
        std::vector<Vector3> field;
        std::vector<int> cornerRotations;
        std::vector<size_t> singularVertices;
        // Chart id per triangle: two triangles share a chart when their cover UVs
        // agree on the edge they share.  Across a seam the values differ by whole
        // cover cells, so this is the solver's own answer to "where are the cuts",
        // instead of something a downstream consumer has to guess from the UVs.
        std::vector<int> triangleCharts;
    };

    // One cover solve of the budget search.  Scale drives the MILS rounding as
    // well as the continuous energy, so the UV area answers it in steps instead
    // of following 1/s^2; a single sample therefore says nothing about which way
    // to move, and every sample is kept for the caller to inspect.
    struct CoverBudgetAttempt {
        double scale = 0.0;
        double uvArea = 0.0;
        double boundaryLoss = 0.0;
        double predicted = 0.0;
        double error = 0.0;
        bool used = false;
    };

    struct CoverBudgetReport {
        std::vector<CoverBudgetAttempt> attempts;
        double extractionEfficiency = 0.955;
        double baseScale = 0.0;
        bool diverged = false;
        bool clamped = false;
        size_t targetQuads = 0;
    };

    // How much of the cover's UV area survives extraction is model dependent, so
    // the constant is a calibration knob rather than a property of the algorithm.
    void setExtractionEfficiency(double efficiency);
    double extractionEfficiency() const { return m_extractionEfficiency; }

    void setTargetQuadCount(size_t targetQuads) { m_targetQuadCount = targetQuads; }

    size_t targetQuadCount() const { return m_targetQuadCount; }

    // Result of the last solve run through this instance, empty when the budget
    // path was not taken (targetQuadCount == 0).
    const CoverBudgetReport& budgetReport() const { return m_budgetReport; }

    static bool parameterize(const std::vector<Vector3>& vertices,
        const std::vector<std::vector<size_t>>& triangles,
        const std::vector<Vector3>* guidance, double scaling,
        double hardEdgeDegrees, Result* result,
        const std::vector<double>* faceScaling = nullptr,
        const std::vector<double>* faceScalingU = nullptr,
        const std::vector<double>* faceScalingV = nullptr,
        // 0 keeps the legacy scale (scaling x average edge length); a positive
        // value solves the cover scale so the surface area divided by the quad
        // footprint matches this budget.
        size_t targetQuadCount = 0,
        // Reports 0..1 across the quad cover solve, which is the single longest
        // step of the whole pipeline and would otherwise be one silent block.
        const ProgressHandler* progressHandler = nullptr,
        // Filled with the samples the budget search took, which one was kept and
        // why it stopped; untouched on the legacy path.
        CoverBudgetReport* budgetReport = nullptr,
        double extractionEfficiency = 0.955,
        // 0 keeps the budget loop's own policy; 1 spends a single cover solve per
        // island, which is the speed switch a caller picks when it would rather
        // have the wall clock than the last few percent of count accuracy.
        int maximumCoverAttempts = 0,
        // 0 regularizes both scale unknowns, 1 pins the across-scale one and turns
        // the 3F curl system into 2F (measured 3x faster on the shoe, see
        // applyCurlCorrection for what it costs in worst-case deviation).
        int curlMode = 0);

    // Instance form of the solve, with the budget knobs and the trace kept on the
    // object.  Named apart because the static form above has to keep its exact
    // signature for callers that never instantiate this class.
    bool parameterizeWithBudget(const std::vector<Vector3>& vertices,
        const std::vector<std::vector<size_t>>& triangles,
        const std::vector<Vector3>* guidance, double scaling,
        double hardEdgeDegrees, Result* result,
        const std::vector<double>* faceScaling = nullptr,
        const std::vector<double>* faceScalingU = nullptr,
        const std::vector<double>* faceScalingV = nullptr,
        const ProgressHandler* progressHandler = nullptr);

private:
    double m_extractionEfficiency = 0.955;
    size_t m_targetQuadCount = 0;
    CoverBudgetReport m_budgetReport;
};

}
#endif
