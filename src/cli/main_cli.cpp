/*
 * Copyright (c) 2020 Jeremy HU <jeremy-at-dust3d dot org>. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

// Qt-free command line front end for the AutoRemesher core.
//
// The options, their defaults, the OBJ import rules, the OBJ export format and
// both report formats are taken from the headless path of the Qt application
// (src/main.cpp -> MainWindow::runHeadless/saveMeshToFile/loadObj) so that
// results from the two front ends can be compared directly.  Progress and the
// per-stage phase report go to stderr, the summary goes to stdout, and the
// process exit code reports what actually happened.

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

#include <AutoRemesher/AutoRemesher>
#include <AutoRemesher/Progress>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

const char* const APP_NAME = "AutoRemesher";
const char* const APP_HUMAN_VER = "1.2.0";
const char* const APP_HOMEPAGE_URL = "https://autoremesher.dust3d.org/";

// Exit codes.
const int EXIT_OK = 0;
const int EXIT_USAGE = 1;
const int EXIT_LOAD_FAILED = 2;
const int EXIT_REMESH_FAILED = 3;

struct HeadlessParams {
    std::string inputPath;
    std::string outputPath;
    std::string reportPath;
    int targetQuads = 50000;
    // The Qt front end stores these as float after parsing; keep the same
    // narrowing so the numbers handed to the pipeline are bit-identical.
    double edgeScaling = 1.0;
    double sharpEdgeDegrees = 90.0;
    double smoothNormalDegrees = 0.0;
    double adaptivity = 1.0;
    double anisotropy = 1.0;
    // Behaviour switches introduced by the architecture refactor.  All three
    // default off so that a run without them reproduces the historical result.
    bool keepAllIslands = false;
    bool relativePositionTolerance = false;
    bool triangulate = false;
    // Emit machine readable NDJSON events on stderr for the optional Go front
    // end (ui-go).  Off by default so the historical output stays byte for byte
    // the same.
    bool eventsJson = false;
};

// QTextStream prints floating point numbers with 6 significant digits by
// default.  Keeping that here means the CLI writes the same text as
// MainWindow::saveMeshToFile and the same report lines as main.cpp.
std::string formatNumber(double value)
{
    std::ostringstream stream;
    stream << std::setprecision(6) << std::defaultfloat << value;
    return stream.str();
}

void printUsage(std::ostream& out)
{
    out << "Usage: autoremesher_cli [options]\n"
        << "\n"
        << "AutoRemesher - Automatic quad remeshing tool\n"
        << "\n"
        << "Options:\n"
        << "  -i, --input <file.obj>     Input .obj file to remesh\n"
        << "  -o, --output <output.obj>  Output .obj file path for the remeshed result\n"
        << "  --report <report.txt>      Path to write a report file with stats\n"
        << "                             (quads, non-quads, vertices, time). If omitted,\n"
        << "                             no report file is written.\n"
        << "  --target-quads <count>     Target quad count (default: 50000)\n"
        << "  --edge-scaling <factor>    Edge scaling factor (default: 1.0, range: 1.0-4.0)\n"
        << "  --sharp-edge <degrees>     Sharp edge dihedral angle threshold in degrees\n"
        << "                             (default: 90.0, range: 30.0-180.0)\n"
        << "  --smooth-normal <degrees>  Smooth normal angle threshold in degrees\n"
        << "                             (default: 0.0, range: 0.0-180.0)\n"
        << "  --adaptivity <value>       Curvature-adaptive quad density\n"
        << "                             (default: 1.0, range: 0.0-1.0)\n"
        << "  --anisotropy <value>       Curvature-adaptive quad elongation\n"
        << "                             (default: 1.0, range: 0.0-1.0)\n"
        << "\n"
        << "Behaviour switches (default off = historical behaviour):\n"
        << "  --keep-all-islands         Keep every extracted quad patch instead of\n"
        << "                             only the largest patch of each island\n"
        << "  --relative-tolerance       Weld cross points with a step relative to the\n"
        << "                             island voxel size instead of the fixed 1e-5\n"
        << "  --triangulate              Fan-triangulate n-gon faces on import instead\n"
        << "                             of reading them as if they were triangles\n"
        << "\n"
        << "  --events-json              Emit NDJSON progress/phase/done events on\n"
        << "                             stderr for programmatic front ends\n"
        << "\n"
        << "  -h, --help                 Print this help\n"
        << "  -v, --version              Print the version\n"
        << "\n"
        << "Exit codes: 0 success, 1 bad usage, 2 input could not be loaded,\n"
        << "            3 remeshing produced no result.\n";
}

bool parseOptions(int argc, char** argv, HeadlessParams& params, int& exitCode)
{
    auto takeValue = [&](int& index, const std::string& option, const char* inlineValue) -> const char* {
        if (nullptr != inlineValue)
            return inlineValue;
        if (index + 1 >= argc) {
            std::cerr << "Error: missing value for " << option << std::endl;
            return nullptr;
        }
        return argv[++index];
    };

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::string name = arg;
        const char* inlineValue = nullptr;
        if (arg.size() > 2 && 0 == arg.compare(0, 2, "--")) {
            auto equal = arg.find('=');
            if (std::string::npos != equal) {
                name = arg.substr(0, equal);
                inlineValue = argv[i] + equal;
                // Rebuild the value string so it stays valid: point into argv.
                inlineValue = argv[i] + equal + 1;
            }
        }

        if (name == "-h" || name == "--help") {
            printUsage(std::cout);
            exitCode = EXIT_OK;
            return false;
        }
        if (name == "-v" || name == "--version") {
            std::cout << APP_NAME << " " << APP_HUMAN_VER << std::endl;
            exitCode = EXIT_OK;
            return false;
        }

        // Options that take a value.
        const char* value = nullptr;
        if (name == "-i" || name == "--input") {
            value = takeValue(i, name, inlineValue);
            if (nullptr == value)
                return false;
            params.inputPath = value;
        } else if (name == "-o" || name == "--output") {
            value = takeValue(i, name, inlineValue);
            if (nullptr == value)
                return false;
            params.outputPath = value;
        } else if (name == "--report") {
            value = takeValue(i, name, inlineValue);
            if (nullptr == value)
                return false;
            params.reportPath = value;
        } else if (name == "--target-quads") {
            value = takeValue(i, name, inlineValue);
            if (nullptr == value)
                return false;
            params.targetQuads = std::atoi(value);
        } else if (name == "--edge-scaling") {
            value = takeValue(i, name, inlineValue);
            if (nullptr == value)
                return false;
            params.edgeScaling = std::atof(value);
        } else if (name == "--sharp-edge") {
            value = takeValue(i, name, inlineValue);
            if (nullptr == value)
                return false;
            params.sharpEdgeDegrees = std::atof(value);
        } else if (name == "--smooth-normal") {
            value = takeValue(i, name, inlineValue);
            if (nullptr == value)
                return false;
            params.smoothNormalDegrees = std::atof(value);
        } else if (name == "--adaptivity") {
            value = takeValue(i, name, inlineValue);
            if (nullptr == value)
                return false;
            params.adaptivity = std::atof(value);
        } else if (name == "--anisotropy") {
            value = takeValue(i, name, inlineValue);
            if (nullptr == value)
                return false;
            params.anisotropy = std::atof(value);
        } else if (name == "--keep-all-islands") {
            params.keepAllIslands = true;
        } else if (name == "--relative-tolerance") {
            params.relativePositionTolerance = true;
        } else if (name == "--triangulate") {
            params.triangulate = true;
        } else if (name == "--events-json") {
            params.eventsJson = true;
        } else {
            std::cerr << "Error: unknown option " << arg << std::endl;
            printUsage(std::cerr);
            return false;
        }
    }
    return true;
}

// ObjLoader: the same subset MainWindow::loadObj uses — positions plus
// triangles, no smoothing groups, no materials.
bool loadObj(const std::string& filename,
    std::vector<AutoRemesher::Vector3>& vertices,
    std::vector<std::vector<size_t>>& triangles,
    bool triangulate)
{
    tinyobj::attrib_t attributes;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    bool loadSuccess = tinyobj::LoadObj(&attributes, &shapes, &materials,
        &warn, &err, filename.c_str());

    if (!warn.empty())
        std::cerr << "WARN: " << warn << std::endl;
    if (!err.empty())
        std::cerr << err << std::endl;
    if (!loadSuccess)
        return false;

    vertices.resize(attributes.vertices.size() / 3);
    for (size_t i = 0, j = 0; i < vertices.size(); ++i) {
        auto& dest = vertices[i];
        dest.setX(attributes.vertices[j++]);
        dest.setY(attributes.vertices[j++]);
        dest.setZ(attributes.vertices[j++]);
    }

    triangles.clear();
    size_t cursor = 0;
    for (const auto& shape : shapes) {
        for (size_t face = 0; face < shape.mesh.num_face_vertices.size(); ++face) {
            const size_t cornerCount = shape.mesh.num_face_vertices[face];
            if (cursor + cornerCount > shape.mesh.indices.size())
                break;
            if (!triangulate) {
                // Historical rule: every three indices form one triangle, whatever
                // the face actually is.  Kept as the default for parity.
                for (size_t i = 0; i + 2 < cornerCount; i += 3) {
                    triangles.push_back(std::vector<size_t> {
                        (size_t)shape.mesh.indices[cursor + i + 0].vertex_index,
                        (size_t)shape.mesh.indices[cursor + i + 1].vertex_index,
                        (size_t)shape.mesh.indices[cursor + i + 2].vertex_index });
                }
            } else if (cornerCount == 3) {
                triangles.push_back(std::vector<size_t> {
                    (size_t)shape.mesh.indices[cursor + 0].vertex_index,
                    (size_t)shape.mesh.indices[cursor + 1].vertex_index,
                    (size_t)shape.mesh.indices[cursor + 2].vertex_index });
            } else if (cornerCount > 3) {
                // Fan triangulation: the remeshing core only accepts triangles.
                for (size_t i = 1; i + 1 < cornerCount; ++i) {
                    triangles.push_back(std::vector<size_t> {
                        (size_t)shape.mesh.indices[cursor + 0].vertex_index,
                        (size_t)shape.mesh.indices[cursor + i].vertex_index,
                        (size_t)shape.mesh.indices[cursor + i + 1].vertex_index });
                }
            }
            cursor += cornerCount;
        }
    }

    std::cerr << "Loaded " << vertices.size() << " vertices, "
              << triangles.size() << " triangles" << std::endl;
    return !vertices.empty();
}

bool saveObj(const std::string& filename,
    const std::vector<AutoRemesher::Vector3>& vertices,
    const std::vector<std::vector<size_t>>& faces)
{
    std::ofstream file(filename.c_str(), std::ios::out | std::ios::trunc);
    if (!file.is_open())
        return false;

    file << "# " << APP_NAME << " " << APP_HUMAN_VER << "\n";
    file << "# " << APP_HOMEPAGE_URL << "\n";
    for (const auto& vertex : vertices)
        file << "v " << formatNumber(vertex.x()) << " "
             << formatNumber(vertex.y()) << " "
             << formatNumber(vertex.z()) << "\n";
    for (const auto& face : faces) {
        file << "f";
        for (const auto& index : face)
            file << " " << (1 + index);
        file << "\n";
    }
    file.flush();
    return file.good();
}

std::string jsonEscape(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (char character : value) {
        switch (character) {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped += character;
            break;
        }
    }
    return escaped;
}

void emitEvent(const char* type, const std::string& payload)
{
    std::fprintf(stderr, "{\"schema_version\":1,\"type\":\"%s\"%s}\n", type, payload.c_str());
    std::fflush(stderr);
}

struct ProgressPrinter {
    std::mutex mutex;
    int lastPercent = -1;
    std::string lastStatus;
    bool json = false;

    static void handler(void* tag, float progress, const char* status)
    {
        ProgressPrinter* printer = static_cast<ProgressPrinter*>(tag);
        printer->print(progress, status ? status : "");
    }

    // Same de-duplication and line format as QuadMeshGenerator::printProgress.
    void print(float progress, const std::string& status)
    {
        const int percent = (int)(progress * 100);
        std::lock_guard<std::mutex> lock(mutex);
        if (percent == lastPercent && status == lastStatus)
            return;
        lastPercent = percent;
        lastStatus = status;
        if (json) {
            emitEvent("progress",
                ",\"percent\":" + std::to_string(percent)
                    + ",\"status\":\"" + jsonEscape(status) + "\"");
        } else if (status.empty()) {
            std::fprintf(stderr, "%d%% done.\n", percent);
        } else {
            std::fprintf(stderr, "%d%% done. %s\n", percent, status.c_str());
        }
        std::fflush(stderr);
    }
};

} // namespace

int main(int argc, char** argv)
{
    HeadlessParams params;
    int exitCode = EXIT_USAGE;
    if (!parseOptions(argc, argv, params, exitCode))
        return exitCode;

    if (params.inputPath.empty()) {
        std::cerr << "Error: --input is required" << std::endl;
        printUsage(std::cerr);
        return EXIT_USAGE;
    }
    if (params.outputPath.empty()) {
        std::cerr << "Error: --output is required when --input is specified" << std::endl;
        return EXIT_USAGE;
    }

    auto startedAt = std::chrono::steady_clock::now();
    auto elapsedSeconds = [&startedAt]() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - startedAt).count();
    };

    std::vector<AutoRemesher::Vector3> vertices;
    std::vector<std::vector<size_t>> triangles;
    if (!loadObj(params.inputPath, vertices, triangles, params.triangulate)) {
        std::cerr << "Error: Failed to load " << params.inputPath << std::endl;
        return EXIT_LOAD_FAILED;
    }

    AutoRemesher::AutoRemesher autoRemesher(vertices, triangles);
    if (params.edgeScaling > 0)
        autoRemesher.setScaling(params.edgeScaling);
    autoRemesher.setTargetTriangleCount((size_t)params.targetQuads * 2);
    autoRemesher.setModelType(AutoRemesher::ModelType::Organic);
    autoRemesher.setGradientAdaptivity(params.adaptivity);
    autoRemesher.setAnisotropy(params.anisotropy);
    autoRemesher.setSharpEdgeDegrees((float)params.sharpEdgeDegrees);
    autoRemesher.setSmoothNormalDegrees((float)params.smoothNormalDegrees);
    autoRemesher.setKeepAllIslands(params.keepAllIslands);
    autoRemesher.setRelativePositionTolerance(params.relativePositionTolerance);
    if (params.keepAllIslands || params.relativePositionTolerance)
        std::cerr << "Behaviour switches: keep-all-islands="
                  << (params.keepAllIslands ? "on" : "off")
                  << " relative-tolerance="
                  << (params.relativePositionTolerance ? "on" : "off") << std::endl;

    ProgressPrinter progressPrinter;
    progressPrinter.json = params.eventsJson;
    autoRemesher.setTag(&progressPrinter);
    autoRemesher.setProgressHandler(&ProgressPrinter::handler);

    bool remeshSucceeded = autoRemesher.remesh();

    if (params.eventsJson) {
        std::string payload = ",\"lines\":[";
        const auto& report = autoRemesher.phaseReport();
        for (size_t i = 0; i < report.size(); ++i) {
            if (i)
                payload += ",";
            payload += "\"" + jsonEscape(report[i]) + "\"";
        }
        payload += "]";
        emitEvent("phase", payload);
    } else {
        for (const auto& line : autoRemesher.phaseReport())
            std::cerr << "  " << line << std::endl;
    }

    if (!remeshSucceeded) {
        std::cerr << "Error: Remeshing failed" << std::endl;
        if (params.eventsJson)
            emitEvent("error", ",\"message\":\"remeshing failed\"");
        return EXIT_REMESH_FAILED;
    }

    const std::vector<AutoRemesher::Vector3>& remeshedVertices = autoRemesher.remeshedVertices();
    const std::vector<std::vector<size_t>>& remeshedQuads = autoRemesher.remeshedQuads();
    if (remeshedVertices.empty() || remeshedQuads.empty()) {
        std::cerr << "Error: Remeshing produced no result" << std::endl;
        if (params.eventsJson)
            emitEvent("error", ",\"message\":\"remeshing produced no result\"");
        return EXIT_REMESH_FAILED;
    }

    size_t quadCount = 0;
    size_t nonQuadCount = 0;
    for (const auto& face : remeshedQuads) {
        if (face.size() == 4)
            ++quadCount;
        else
            ++nonQuadCount;
    }
    size_t vertexCount = remeshedVertices.size();

    if (!saveObj(params.outputPath, remeshedVertices, remeshedQuads)) {
        std::cerr << "Error: Failed to write " << params.outputPath << std::endl;
        return EXIT_REMESH_FAILED;
    }

    const double elapsed = elapsedSeconds();

    if (params.eventsJson) {
        emitEvent("done",
            ",\"quads\":" + std::to_string(quadCount)
                + ",\"non_quads\":" + std::to_string(nonQuadCount)
                + ",\"vertices\":" + std::to_string(vertexCount)
                + ",\"exit\":0");
    }

    std::cout << "=== AutoRemesher Report ===" << std::endl;
    std::cout << "Input: " << params.inputPath << std::endl;
    std::cout << "Output: " << params.outputPath << std::endl;
    std::cout << "Quads: " << quadCount << std::endl;
    std::cout << "Non-quads: " << nonQuadCount << std::endl;
    std::cout << "Vertices: " << vertexCount << std::endl;
    std::cout << "Time: " << formatNumber(elapsed) << " seconds" << std::endl;
    std::cout << "===========================" << std::endl;

    if (!params.reportPath.empty()) {
        std::ofstream reportFile(params.reportPath.c_str(), std::ios::out | std::ios::trunc);
        if (!reportFile.is_open()) {
            std::cerr << "Error: Failed to write " << params.reportPath << std::endl;
            return EXIT_REMESH_FAILED;
        }
        // Byte-for-byte the layout of the report main.cpp writes.
        reportFile << "AutoRemesher Report\n";
        reportFile << "===================\n\n";
        reportFile << "Input file: " << params.inputPath << "\n";
        reportFile << "Output file: " << params.outputPath << "\n";
        reportFile << "Target quads: " << params.targetQuads << "\n";
        reportFile << "Edge scaling: " << formatNumber(params.edgeScaling) << "\n";
        reportFile << "Sharp edge degrees: " << formatNumber(params.sharpEdgeDegrees) << "\n";
        reportFile << "Smooth normal degrees: " << formatNumber(params.smoothNormalDegrees) << "\n";
        reportFile << "Adaptivity: " << formatNumber(params.adaptivity) << "\n";
        reportFile << "Anisotropy: " << formatNumber(params.anisotropy) << "\n\n";
        reportFile << "Results:\n";
        reportFile << "  Quads: " << quadCount << "\n";
        reportFile << "  Non-quads: " << nonQuadCount << "\n";
        reportFile << "  Vertices: " << vertexCount << "\n";
        reportFile << "  Total time: " << formatNumber(elapsed) << " seconds\n";
        reportFile.flush();
        if (!reportFile.good()) {
            std::cerr << "Error: Failed to write " << params.reportPath << std::endl;
            return EXIT_REMESH_FAILED;
        }
    }

    return EXIT_OK;
}
