/*
 * AutoRemesher headless CLI.
 *
 * The remeshing core (src/AutoRemesher) has no Qt dependency.  This executable is
 * the only entry point: it loads an OBJ, runs one remesh, writes a quad OBJ, and
 * optionally speaks the JSON-lines protocol documented in docs/cli-protocol.md so
 * the Go GUI can drive it without any Qt code in the process.
 */
#include <AutoRemesher/AutoRemesher>
#include <AutoRemesher/Vector2>
#include <AutoRemesher/Vector3>
#include <core/base/logging.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#ifndef AR_VERSION
#define AR_VERSION "1.2.0"
#endif

// TBB 2017 (the vendored copy) has tbb::task_scheduler_init; oneAPI TBB removed it
// in favour of global_control.  Pick whichever header the toolchain provides.
#if defined(__has_include)
#if __has_include(<tbb/task_scheduler_init.h>)
#include <tbb/task_scheduler_init.h>
#define AR_TBB_LEGACY_INIT 1
#elif __has_include(<oneapi/tbb/global_control.h>)
#include <oneapi/tbb/global_control.h>
#elif __has_include(<tbb/global_control.h>)
#include <tbb/global_control.h>
#endif
#else
#include <tbb/task_scheduler_init.h>
#define AR_TBB_LEGACY_INIT 1
#endif

namespace {

// ---------------------------------------------------------------- JSON output

std::string jsonEscape(const std::string& text)
{
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buffer[8];
                snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(c));
                out += buffer;
            } else {
                out += c;
            }
        }
    }
    return out;
}

void emitLine(const std::string& line)
{
    fputs(line.c_str(), stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

// Paths are shown with forward slashes: readable on every platform and free of
// backslash escapes inside the JSON events.
std::string displayPath(const std::string& path)
{
    std::string out = path;
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
}

// -------------------------------------------------------------------- options

struct Options {
    std::string input;
    std::string output;
    std::string preset = "organic";
    size_t targetQuads = 20000;
    double adaptivity = 0.5;
    double anisotropy = 0.0;
    double sharpEdgeDegrees = 60.0;
    double smoothNormalDegrees = 0.0;
    double edgeScaling = 1.0;
    // A preset fills in the knobs the command line did not name, so an explicit
    // --sharp-edge always wins no matter where it sits relative to --preset.
    bool adaptivitySet = false;
    bool anisotropySet = false;
    bool sharpEdgeSet = false;
    bool smoothNormalSet = false;
    bool edgeScalingSet = false;
    // Solving the cover scale from the budget is the default: a slider that says
    // 20k and lands at 14.7k reads as a bug.  --legacy-target-quads restores the
    // historical average-edge-length path, which is what old scripts pinned.
    bool exactTargetQuads = true;
    bool relativeTolerance = true;
    double keepIslandFraction = 0.0;
    bool weldPatches = false;
    bool gradedHoles = true;
    bool projectedCaps = false;
    bool localRemesh = false;
    int threads = 0;
    bool writeUvs = true;
    bool writeNormals = true;
    int symmetryAxis = 0; // 0 off, 1 x, 2 y, 3 z
    double symmetryPlane = 0.0;
    double symmetryTolerance = 0.002;
    int logLevel = 1; // 0 = silent, 1 = normal, 2 = debug (core Log::Level order)
    bool json = false;
    bool progress = false;
    bool verbose = false;
    bool quiet = false;
};

// A preset is a name for one point in the four-knob space, because "organic"
// and "hard-surface" are what a user actually thinks in.  The organic point is
// the historical default, so a plain run without --preset keeps producing the
// same quads byte for byte.
struct Preset {
    const char* name;
    double sharpEdgeDegrees;
    double smoothNormalDegrees;
    double adaptivity;
    double anisotropy;
    double edgeScaling;
};

const Preset kPresets[] = {
    { "organic", 60.0, 0.0, 0.5, 0.0, 1.0 },
    // Hard surface: keep creases (a lower sharp threshold marks more edges
    // sharp), smooth normals only across shallow blends, and let the density
    // follow curvature less, because flat panels should stay evenly dense.
    { "hard-surface", 40.0, 30.0, 0.2, 0.0, 1.0 },
};

const Preset* findPreset(const std::string& name)
{
    for (const Preset& preset : kPresets) {
        if (name == preset.name)
            return &preset;
    }
    return nullptr;
}

void applyPreset(Options* options)
{
    const Preset* preset = findPreset(options->preset);
    if (nullptr == preset)
        return;
    if (!options->sharpEdgeSet)
        options->sharpEdgeDegrees = preset->sharpEdgeDegrees;
    if (!options->smoothNormalSet)
        options->smoothNormalDegrees = preset->smoothNormalDegrees;
    if (!options->adaptivitySet)
        options->adaptivity = preset->adaptivity;
    if (!options->anisotropySet)
        options->anisotropy = preset->anisotropy;
    if (!options->edgeScalingSet)
        options->edgeScaling = preset->edgeScaling;
}

const char* kUsage =
    "AutoRemesher " AR_VERSION " - automatic quad remeshing (headless)\n"
    "\n"
    "Usage:\n"
    "  autoremesher --input MODEL.obj --output OUT.obj --target-quads 20000 [options]\n"
    "\n"
    "Input / output:\n"
    "  -i, --input PATH            input OBJ (polygons are triangulated)\n"
    "  -o, --output PATH           output OBJ of quads (default: <input>_remeshed.obj)\n"
    "      --no-uv                 do not write vt lines\n"
    "      --no-normals            do not write vn lines\n"
    "      --symmetry AXIS         x | y | z: snap the output onto its own mirror\n"
    "                              plane where both sides agree, and count where\n"
    "                              they do not (default off)\n"
    "      --symmetry-plane F      coordinate of the mirror plane along the axis\n"
    "                              (default 0, the coordinate plane)\n"
    "      --symmetry-tolerance F  seam tolerance as a fraction of the bbox\n"
    "                              diagonal (default 0.002)\n"
    "\n"
    "Remeshing:\n"
    "  -q, --target-quads N        requested quad count (default 20000); the\n"
    "                              cover scale is solved from this budget, so the\n"
    "                              result lands within about +/-5%\n"
    "      --adaptivity F          gradient adaptivity 0..1 (default 0.5)\n"
    "      --anisotropy F          anisotropy 0..1 (default 0.0)\n"
    "      --sharp-edge DEG        sharp edge threshold (default 60)\n"
    "      --smooth-normal DEG     smooth normal threshold (default 0)\n"
    "      --edge-scaling F        edge scale multiplier (default 1.0)\n"
    "      --preset P              organic | hard-surface: fills every knob the\n"
    "                              command line did not set (default organic)\n"
    "      --model-type T          alias of --preset, kept for old scripts\n"
    "      --exact-target-quads    solve the cover scale from the quad budget\n"
    "                              (default; kept so old scripts still parse)\n"
    "      --legacy-target-quads   take the scale from the average edge length\n"
    "                              instead; the count then lands around 0.7N..1.05N\n"
    "      --absolute-tolerance    legacy fixed 1e-5 weld (default: relative)\n"
    "      --keep-island-fraction F  keep patches above this area share (default 0)\n"
    "      --keep-all-islands      keep every extracted patch\n"
    "      --weld-patches          weld coincident patch vertices\n"
    "      --graded-holes          graded hole filling (default)\n"
    "      --legacy-holes          fixed fan hole filling\n"
    "      --projected-caps        seal large holes with projected caps\n"
    "      --local-remesh          energy guided topology pass\n"
    "      --threads N             pin the TBB worker count (0 = all cores)\n"
    "\n"
    "Reporting:\n"
    "      --json                  JSON-lines events on stdout (see docs/cli-protocol.md)\n"
    "                              result carries topology, holes, patches, phases\n"
    "      --progress              progress on stderr\n"
    "      --verbose               per-phase report at the end\n"
    "      --log-level L           quiet | normal | debug (default normal);\n"
    "                              debug adds the per-step and per-hole traces\n"
    "      --quiet                 no human output except errors\n"
    "  -h, --help                  this text\n"
    "  -v, --version               version\n"
    "\n"
    "Exit codes: 0 success, 1 usage/input error, 2 remeshing failed, 3 output failed.\n";

bool parseArgs(int argc, char** argv, Options* options, int* exitCode)
{
    bool seenInput = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 < argc)
                return argv[++i];
            return std::string();
        };
        const auto number = [&]() -> double {
            const std::string text = value();
            if (text.empty()) {
                fprintf(stderr, "missing value for %s\n", arg.c_str());
                *exitCode = 1;
                return 0.0;
            }
            char* end = nullptr;
            const double parsed = strtod(text.c_str(), &end);
            if (end == text.c_str()) {
                fprintf(stderr, "invalid number '%s' for %s\n", text.c_str(), arg.c_str());
                *exitCode = 1;
                return 0.0;
            }
            return parsed;
        };

        if (arg == "-h" || arg == "--help") {
            fputs(kUsage, stdout);
            *exitCode = 0;
            return false;
        }
        if (arg == "-v" || arg == "--version") {
            printf("AutoRemesher %s\n", AR_VERSION);
            *exitCode = 0;
            return false;
        }
        if (arg == "-i" || arg == "--input") {
            options->input = value();
            seenInput = !options->input.empty();
        } else if (arg == "-o" || arg == "--output") {
            options->output = value();
        } else if (arg == "-q" || arg == "--target-quads") {
            const double parsed = number();
            if (*exitCode)
                return false;
            options->targetQuads = static_cast<size_t>(std::max(1.0, parsed));
        } else if (arg == "--adaptivity") {
            options->adaptivity = number();
            options->adaptivitySet = true;
        } else if (arg == "--anisotropy") {
            options->anisotropy = number();
            options->anisotropySet = true;
        } else if (arg == "--sharp-edge") {
            options->sharpEdgeDegrees = number();
            options->sharpEdgeSet = true;
        } else if (arg == "--smooth-normal") {
            options->smoothNormalDegrees = number();
            options->smoothNormalSet = true;
        } else if (arg == "--edge-scaling") {
            options->edgeScaling = number();
            options->edgeScalingSet = true;
        } else if (arg == "--preset" || arg == "--model-type") {
            std::string type = value();
            if (type.empty())
                type = "organic";
            if (type == "hardsurface" || type == "hard")
                type = "hard-surface";
            if (nullptr == findPreset(type)) {
                fprintf(stderr, "unknown preset '%s' (known:", type.c_str());
                for (const Preset& preset : kPresets)
                    fprintf(stderr, " %s", preset.name);
                fprintf(stderr, ")\n");
                *exitCode = 1;
                return false;
            }
            options->preset = type;
        } else if (arg == "--exact-target-quads") {
            options->exactTargetQuads = true;
        } else if (arg == "--legacy-target-quads") {
            options->exactTargetQuads = false;
        } else if (arg == "--absolute-tolerance") {
            options->relativeTolerance = false;
        } else if (arg == "--relative-tolerance") {
            options->relativeTolerance = true;
        } else if (arg == "--keep-island-fraction") {
            options->keepIslandFraction = number();
        } else if (arg == "--keep-all-islands") {
            options->keepIslandFraction = 1e-9;
        } else if (arg == "--weld-patches") {
            options->weldPatches = true;
        } else if (arg == "--graded-holes") {
            options->gradedHoles = true;
        } else if (arg == "--legacy-holes") {
            options->gradedHoles = false;
        } else if (arg == "--projected-caps") {
            options->projectedCaps = true;
        } else if (arg == "--local-remesh") {
            options->localRemesh = true;
        } else if (arg == "--threads") {
            options->threads = static_cast<int>(number());
        } else if (arg == "--symmetry") {
            const std::string axis = value();
            if (axis == "x" || axis == "X")
                options->symmetryAxis = 1;
            else if (axis == "y" || axis == "Y")
                options->symmetryAxis = 2;
            else if (axis == "z" || axis == "Z")
                options->symmetryAxis = 3;
            else if (axis.empty() || axis == "none" || axis == "off")
                options->symmetryAxis = 0;
            else {
                fprintf(stderr, "unknown symmetry axis '%s' (x|y|z|none)\n", axis.c_str());
                *exitCode = 1;
                return false;
            }
        } else if (arg == "--symmetry-plane") {
            options->symmetryPlane = number();
        } else if (arg == "--symmetry-tolerance") {
            options->symmetryTolerance = number();
        } else if (arg == "--no-uv") {
            options->writeUvs = false;
        } else if (arg == "--no-normals") {
            options->writeNormals = false;
        } else if (arg == "--json") {
            options->json = true;
        } else if (arg == "--progress") {
            options->progress = true;
        } else if (arg == "--verbose") {
            options->verbose = true;
        } else if (arg == "--quiet") {
            options->quiet = true;
            options->logLevel = 0;
        } else if (arg == "--log-level") {
            const std::string level = value();
            if (level == "quiet" || level == "silent") {
                options->quiet = true;
                options->logLevel = 0;
            } else if (level == "normal" || level.empty()) {
                options->quiet = false;
                options->logLevel = 1;
            } else if (level == "debug" || level == "verbose") {
                options->quiet = false;
                options->logLevel = 2;
            } else {
                fprintf(stderr, "unknown log level '%s' (quiet|normal|debug)\n", level.c_str());
                *exitCode = 1;
                return false;
            }
        } else if (!arg.empty() && arg[0] == '-') {
            fprintf(stderr, "unknown option '%s'\n", arg.c_str());
            *exitCode = 1;
            return false;
        } else if (!seenInput) {
            // Allow the bare "autoremesher model.obj" spelling.
            options->input = arg;
            seenInput = true;
        } else {
            fprintf(stderr, "unexpected argument '%s'\n", arg.c_str());
            *exitCode = 1;
            return false;
        }
        if (*exitCode)
            return false;
    }
    if (options->input.empty()) {
        fputs(kUsage, stderr);
        *exitCode = 1;
        return false;
    }
    // Presets are resolved last so an explicit knob always beats the preset,
    // whatever order the flags appeared in.
    applyPreset(options);
    return true;
}

// ---------------------------------------------------------------- OBJ support

bool loadObj(const std::string& path,
    std::vector<AutoRemesher::Vector3>* vertices,
    std::vector<std::vector<size_t>>* triangles,
    std::string* error)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        *error = "failed to open " + displayPath(path);
        return false;
    }
    std::string line;
    size_t lineNumber = 0;
    size_t skippedFaces = 0;
    while (std::getline(stream, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.size() < 2)
            continue;
        if (line[0] == 'v' && (line[1] == ' ' || line[1] == '\t')) {
            const char* cursor = line.c_str() + 2;
            char* end = nullptr;
            double coordinates[3] = { 0.0, 0.0, 0.0 };
            bool ok = true;
            for (int axis = 0; axis < 3; ++axis) {
                coordinates[axis] = strtod(cursor, &end);
                if (end == cursor) {
                    ok = false;
                    break;
                }
                cursor = end;
            }
            if (ok)
                vertices->push_back(AutoRemesher::Vector3(coordinates[0], coordinates[1], coordinates[2]));
        } else if (line[0] == 'f' && (line[1] == ' ' || line[1] == '\t')) {
            const size_t vertexCount = vertices->size();
            std::vector<size_t> face;
            const char* cursor = line.c_str() + 2;
            while (*cursor) {
                while (*cursor == ' ' || *cursor == '\t')
                    ++cursor;
                if (!*cursor)
                    break;
                char* end = nullptr;
                const long index = strtol(cursor, &end, 10);
                if (end == cursor)
                    break;
                cursor = end;
                // Skip the texture / normal suffixes and the rest of the token.
                while (*cursor && *cursor != ' ' && *cursor != '\t')
                    ++cursor;
                if (0 == index)
                    continue;
                size_t resolved = 0;
                if (index > 0) {
                    resolved = static_cast<size_t>(index - 1);
                } else {
                    const size_t back = static_cast<size_t>(-index);
                    if (back > vertexCount)
                        continue;
                    resolved = vertexCount - back;
                }
                if (resolved >= vertexCount)
                    continue;
                face.push_back(resolved);
            }
            if (face.size() >= 3) {
                for (size_t i = 2; i < face.size(); ++i)
                    triangles->push_back({ face[0], face[i - 1], face[i] });
            } else {
                ++skippedFaces;
            }
        }
    }
    if (vertices->empty() || triangles->empty()) {
        std::ostringstream message;
        message << "no triangle geometry in " << displayPath(path);
        if (skippedFaces)
            message << " (" << skippedFaces << " degenerate faces skipped)";
        *error = message.str();
        return false;
    }
    return true;
}

// Area weighted vertex normals: a quad contributes one Newell normal to each of
// its corners, so a vertex on a flat panel is not dragged around by a sliver.
std::vector<AutoRemesher::Vector3> computeVertexNormals(
    const std::vector<AutoRemesher::Vector3>& vertices,
    const std::vector<std::vector<size_t>>& faces)
{
    std::vector<AutoRemesher::Vector3> normals(vertices.size());
    for (const auto& face : faces) {
        if (face.size() < 3)
            continue;
        AutoRemesher::Vector3 normal;
        for (size_t i = 0; i < face.size(); ++i) {
            const size_t next = (i + 1) % face.size();
            if (face[i] >= vertices.size() || face[next] >= vertices.size())
                continue;
            const AutoRemesher::Vector3& current = vertices[face[i]];
            const AutoRemesher::Vector3& following = vertices[face[next]];
            normal = normal + AutoRemesher::Vector3(
                                   (current.y() - following.y()) * (current.z() + following.z()),
                                   (current.z() - following.z()) * (current.x() + following.x()),
                                   (current.x() - following.x()) * (current.y() + following.y()));
        }
        for (const size_t corner : face) {
            if (corner < normals.size())
                normals[corner] = normals[corner] + normal;
        }
    }
    for (auto& normal : normals) {
        const double length = normal.length();
        if (length > 0.0)
            normal = normal * (1.0 / length);
    }
    return normals;
}

bool writeObj(const std::string& path,
    const std::vector<AutoRemesher::Vector3>& vertices,
    const std::vector<std::vector<size_t>>& faces,
    const std::vector<std::vector<AutoRemesher::Vector2>>& faceUvs,
    const std::vector<AutoRemesher::Vector3>& normals,
    std::string* error)
{
    FILE* file = fopen(path.c_str(), "wb");
    if (nullptr == file) {
        *error = "failed to write " + displayPath(path);
        return false;
    }
    // UVs are per corner: a quad takes the chart of its own centroid, so the
    // corners belong to that face's parameterization, not to a shared vertex.
    const bool hasUvs = faceUvs.size() == faces.size() && !faces.empty();
    const bool hasNormals = normals.size() == vertices.size() && !vertices.empty();
    fprintf(file, "# AutoRemesher %s\n", AR_VERSION);
    for (const auto& vertex : vertices)
        fprintf(file, "v %.17g %.17g %.17g\n", vertex.x(), vertex.y(), vertex.z());
    if (hasUvs) {
        for (const auto& quadUvs : faceUvs) {
            for (const auto& uv : quadUvs)
                fprintf(file, "vt %.17g %.17g\n", uv.x(), uv.y());
        }
    }
    if (hasNormals) {
        for (const auto& normal : normals)
            fprintf(file, "vn %.17g %.17g %.17g\n", normal.x(), normal.y(), normal.z());
    }
    size_t nextUv = 1;
    for (size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
        const auto& face = faces[faceIndex];
        if (face.size() < 3)
            continue;
        const bool faceHasUvs = hasUvs && faceUvs[faceIndex].size() == face.size();
        fputc('f', file);
        for (size_t corner = 0; corner < face.size(); ++corner) {
            const size_t vertexIndex = face[corner] + 1;
            if (faceHasUvs && hasNormals)
                fprintf(file, " %zu/%zu/%zu", vertexIndex, nextUv + corner, vertexIndex);
            else if (faceHasUvs)
                fprintf(file, " %zu/%zu", vertexIndex, nextUv + corner);
            else if (hasNormals)
                fprintf(file, " %zu//%zu", vertexIndex, vertexIndex);
            else
                fprintf(file, " %zu", vertexIndex);
        }
        fputc('\n', file);
        if (faceHasUvs)
            nextUv += face.size();
    }
    if (0 != fclose(file)) {
        *error = "failed while writing " + displayPath(path);
        return false;
    }
    return true;
}

std::string defaultOutputPath(const std::string& input)
{
    const size_t slash = input.find_last_of("/\\");
    const size_t dot = input.find_last_of('.');
    const bool hasExtension = dot != std::string::npos && (slash == std::string::npos || dot > slash);
    if (!hasExtension)
        return input + "_remeshed.obj";
    return input.substr(0, dot) + "_remeshed.obj";
}

// -------------------------------------------------------------------- progress

struct ProgressSink {
    std::mutex mutex;
    bool json = false;
    bool human = false;
    double lastValue = -1.0;
    std::string lastStatus;
    std::chrono::steady_clock::time_point lastEmit;
    bool emittedAny = false;
};

void reportProgress(void* tag, float progress, const char* status)
{
    ProgressSink* sink = static_cast<ProgressSink*>(tag);
    if (nullptr == sink)
        return;
    std::lock_guard<std::mutex> lock(sink->mutex);
    double value = static_cast<double>(progress);
    if (!(value >= 0.0))
        value = 0.0;
    if (value > 1.0)
        value = 1.0;
    // Never go backwards: the per-island weights can make the aggregate jitter.
    if (value < sink->lastValue)
        value = sink->lastValue;
    const std::string statusText = (nullptr != status) ? status : sink->lastStatus;
    const auto now = std::chrono::steady_clock::now();
    const bool statusChanged = statusText != sink->lastStatus;
    const bool bigStep = value - sink->lastValue >= 0.005;
    const bool due = !sink->emittedAny
        || now - sink->lastEmit >= std::chrono::milliseconds(40)
        || value >= 1.0;
    if (!(due && (statusChanged || bigStep || value >= 1.0 || !sink->emittedAny)))
        return;
    sink->lastValue = value;
    sink->lastStatus = statusText;
    sink->lastEmit = now;
    sink->emittedAny = true;
    if (sink->json) {
        char buffer[512];
        snprintf(buffer, sizeof(buffer),
            "{\"type\":\"progress\",\"value\":%.4f,\"status\":\"%s\"}",
            value, jsonEscape(statusText).c_str());
        emitLine(buffer);
    }
    if (sink->human) {
        fprintf(stderr, "\r[%5.1f%%] %-40s", value * 100.0, statusText.c_str());
        fflush(stderr);
    }
}

std::string phaseReportJson(const std::vector<std::string>& lines)
{
    std::string out = "{\"type\":\"report\",\"lines\":[";
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i)
            out += ',';
        out += '"';
        out += jsonEscape(lines[i]);
        out += '"';
    }
    out += "]}";
    return out;
}

std::string errorEvent(const std::string& message)
{
    return "{\"type\":\"error\",\"message\":\"" + jsonEscape(message) + "\"}";
}

void stderrLogSink(int level, const char* message)
{
    (void)level;
    if (nullptr == message)
        return;
    fputs(message, stderr);
    fflush(stderr);
}

std::string topologyJson(const AutoRemesher::TopologyStats& stats)
{
    std::ostringstream out;
    out << "{\"vertices\":" << stats.vertices
        << ",\"faces\":" << stats.faces
        << ",\"quads\":" << stats.quads
        << ",\"nonQuads\":" << stats.nonQuads
        << ",\"triangles\":" << stats.triangles
        << ",\"boundaryEdges\":" << stats.boundaryEdges
        << ",\"boundaryLoops\":" << stats.boundaryLoops
        << ",\"nonManifoldEdges\":" << stats.nonManifoldEdges
        << ",\"components\":" << stats.components
        << ",\"degenerateFaces\":" << stats.degenerateFaces
        << "}";
    return out.str();
}

std::string phasesJson(const std::vector<AutoRemesher::PhaseTiming>& timings)
{
    std::ostringstream out;
    out << '[';
    for (size_t i = 0; i < timings.size(); ++i) {
        if (i)
            out << ',';
        out << "{\"name\":\"" << jsonEscape(timings[i].name) << "\",\"ms\":"
            << std::fixed << std::setprecision(3) << timings[i].milliseconds
            << ",\"accumulated\":" << (timings[i].accumulated ? "true" : "false") << "}";
    }
    out << ']';
    return out.str();
}

// Warnings are the part that used to be missing entirely: the pipeline knew it
// was handed a shell with thousands of boundary edges, and said nothing.
void emitWarning(const Options& options, const char* code, const std::string& message, long long value)
{
    if (options.json) {
        std::ostringstream line;
        line << "{\"type\":\"warning\",\"code\":\"" << jsonEscape(code) << "\",\"value\":"
             << value << ",\"message\":\"" << jsonEscape(message) << "\"}";
        emitLine(line.str());
    }
    if (!options.quiet)
        fprintf(stderr, "warning: %s\n", message.c_str());
}

void reportInputHealth(const Options& options, const AutoRemesher::TopologyStats& stats)
{
    if (stats.boundaryEdges > 0) {
        std::ostringstream message;
        message << "input has " << stats.boundaryEdges << " boundary edge(s) in "
                << stats.boundaryLoops << " loop(s); remeshing does not stitch input boundaries, "
                << "so the output keeps a boundary there";
        emitWarning(options, "input_open_boundary", message.str(), (long long)stats.boundaryEdges);
    }
    if (stats.nonManifoldEdges > 0) {
        std::ostringstream message;
        message << "input has " << stats.nonManifoldEdges
                << " non-manifold edge(s) (more than two faces); faces around them may be dropped";
        emitWarning(options, "input_non_manifold", message.str(), (long long)stats.nonManifoldEdges);
    }
    if (stats.components > 1) {
        std::ostringstream message;
        message << "input has " << stats.components
                << " disconnected component(s); each is remeshed as its own island and dropped "
                << "unless --keep-island-fraction or --keep-all-islands is given";
        emitWarning(options, "input_multiple_components", message.str(), (long long)stats.components);
    }
    if (stats.degenerateFaces > 0) {
        std::ostringstream message;
        message << "input has " << stats.degenerateFaces
                << " degenerate face(s) (repeated vertex index or zero area)";
        emitWarning(options, "input_degenerate_faces", message.str(), (long long)stats.degenerateFaces);
    }
}

const char* symmetryAxisName(int axis)
{
    switch (axis) {
    case 1:
        return "x";
    case 2:
        return "y";
    case 3:
        return "z";
    default:
        return "none";
    }
}

void reportTargetHealth(const Options& options, size_t requested, size_t actual,
    const AutoRemesher::HoleStats& holes)
{
    if (holes.ignored > 0) {
        std::ostringstream message;
        message << holes.ignored
                << " hole(s) were left open: a boundary loop longer than 65 edges is only sealed "
                << "with --projected-caps";
        emitWarning(options, "holes_left_open", message.str(), (long long)holes.ignored);
    }
    if (requested > 0) {
        const double error = ((double)actual - (double)requested) / (double)requested;
        if (std::fabs(error) > 0.10) {
            std::ostringstream message;
            message.setf(std::ios::fixed);
            message.precision(1);
            message << "actual quads " << actual << " vs requested " << requested
                    << " (" << (error * 100.0) << "%)";
            if (options.exactTargetQuads)
                message << "; the exact path missed its +/-5% window on this input";
            else
                message << "; --exact-target-quads solves the cover scale from the budget "
                        << "and lands within about +/-5%";
            emitWarning(options, "target_quads_off", message.str(), (long long)actual);
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    Options options;
    int exitCode = 0;
    if (!parseArgs(argc, argv, &options, &exitCode))
        return exitCode;
    if (options.output.empty())
        options.output = defaultOutputPath(options.input);

    // The core library no longer writes to stderr on its own: this sink is what
    // turns its phase report and debug traces into lines the CLI owns, so
    // --quiet can be quiet and --verbose can print exactly once.
    AutoRemesher::Log::setSink(&stderrLogSink);
    AutoRemesher::Log::setLevel(options.quiet ? AutoRemesher::Log::Silent
                                              : (options.logLevel >= 2 ? AutoRemesher::Log::Debug
                                                                       : AutoRemesher::Log::Normal));

    std::vector<AutoRemesher::Vector3> vertices;
    std::vector<std::vector<size_t>> triangles;
    std::string error;
    if (!loadObj(options.input, &vertices, &triangles, &error)) {
        if (options.json)
            emitLine(errorEvent(error));
        if (!options.quiet)
            fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    if (!options.quiet) {
        fprintf(stderr, "AutoRemesher %s\n", AR_VERSION);
        fprintf(stderr, "  input: %s (%zu vertices, %zu triangles)\n",
            displayPath(options.input).c_str(), vertices.size(), triangles.size());
        fprintf(stderr, "  output: %s\n", displayPath(options.output).c_str());
        fprintf(stderr, "  target quads: %zu, preset: %s%s\n", options.targetQuads,
            options.preset.c_str(), options.exactTargetQuads ? ", exact target" : "");
    }
    if (options.json) {
        char buffer[1024];
        snprintf(buffer, sizeof(buffer),
            "{\"type\":\"start\",\"version\":\"%s\",\"input\":\"%s\",\"output\":\"%s\","
            "\"targetQuads\":%zu,\"vertices\":%zu,\"triangles\":%zu,\"preset\":\"%s\","
            "\"exactTargetQuads\":%s}",
            AR_VERSION, jsonEscape(displayPath(options.input)).c_str(),
            jsonEscape(displayPath(options.output)).c_str(), options.targetQuads,
            vertices.size(), triangles.size(), jsonEscape(options.preset).c_str(),
            options.exactTargetQuads ? "true" : "false");
        emitLine(buffer);
    }

    ProgressSink sink;
    sink.json = options.json;
    sink.human = options.progress && !options.json;

    AutoRemesher::AutoRemesher remesher(vertices, triangles);
    remesher.setTargetTriangleCount(options.targetQuads * 2);
    remesher.setScaling(options.edgeScaling);
    remesher.setGradientAdaptivity(options.adaptivity);
    remesher.setAnisotropy(options.anisotropy);
    remesher.setSharpEdgeDegrees(options.sharpEdgeDegrees);
    remesher.setSmoothNormalDegrees(options.smoothNormalDegrees);
    remesher.setExactTargetQuads(options.exactTargetQuads);
    remesher.setRelativePositionTolerance(options.relativeTolerance);
    remesher.setIslandAreaFraction(options.keepIslandFraction);
    remesher.setWeldPatches(options.weldPatches);
    remesher.setGradedHoleFilling(options.gradedHoles);
    remesher.setProjectedHoleCaps(options.projectedCaps);
    remesher.setLocalRemesh(options.localRemesh);
    remesher.setExportUvs(options.writeUvs);
    remesher.setSymmetryAxis(options.symmetryAxis);
    remesher.setSymmetryPlane(options.symmetryPlane);
    remesher.setSymmetryTolerance(options.symmetryTolerance);
    // --verbose prints the report itself below, so the library must not print it
    // a second time; --quiet suppresses it entirely.
    remesher.setReportEnabled(!options.quiet && !options.verbose);
    if (options.json || options.progress) {
        remesher.setProgressHandler(reportProgress);
        remesher.setTag(&sink);
    }

    // Describe the input before spending minutes on it.  A shell with thousands
    // of boundary edges is going to keep a boundary; the run says so up front
    // instead of leaving it to be discovered in another tool afterwards.
    remesher.analyzeInput();
    reportInputHealth(options, remesher.inputStats());
    if (options.json) {
        std::ostringstream event;
        event << "{\"type\":\"input\",\"topology\":" << topologyJson(remesher.inputStats()) << "}";
        emitLine(event.str());
    }

    // One scheduler for the whole run: this is how --threads is pinned.
#if defined(AR_TBB_LEGACY_INIT)
    std::unique_ptr<tbb::task_scheduler_init> scheduler;
    if (options.threads > 0)
        scheduler.reset(new tbb::task_scheduler_init(options.threads));
#else
    std::unique_ptr<tbb::global_control> parallelism;
    if (options.threads > 0) {
        parallelism.reset(new tbb::global_control(tbb::global_control::max_allowed_parallelism,
            static_cast<size_t>(options.threads)));
    }
#endif

    const auto start = std::chrono::steady_clock::now();
    const bool remeshed = remesher.remesh();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    if (options.progress && !options.json)
        fputc('\n', stderr);

    if (!remeshed) {
        const std::string message = remesher.lastError().empty() ? "remeshing failed" : remesher.lastError();
        if (options.json)
            emitLine(errorEvent(message));
        fprintf(stderr, "error: %s\n", message.c_str());
        return 2;
    }

    size_t quads = 0;
    size_t nonQuads = 0;
    for (const auto& face : remesher.remeshedQuads()) {
        if (4 == face.size())
            ++quads;
        else
            ++nonQuads;
    }

    const std::vector<std::vector<AutoRemesher::Vector2>>& faceUvs = remesher.remeshedFaceUvs();
    const bool hasUvs = faceUvs.size() == remesher.remeshedQuads().size() && !faceUvs.empty();
    size_t uvCorners = 0;
    for (const auto& quadUvs : faceUvs)
        uvCorners += quadUvs.size();
    std::vector<AutoRemesher::Vector3> normals;
    if (options.writeNormals)
        normals = computeVertexNormals(remesher.remeshedVertices(), remesher.remeshedQuads());

    if (!writeObj(options.output, remesher.remeshedVertices(), remesher.remeshedQuads(),
            faceUvs, normals, &error)) {
        if (options.json)
            emitLine(errorEvent(error));
        fprintf(stderr, "error: %s\n", error.c_str());
        return 3;
    }

    if (options.verbose) {
        if (options.json) {
            emitLine(phaseReportJson(remesher.phaseReport()));
        } else {
            for (const auto& line : remesher.phaseReport())
                fprintf(stderr, "%s\n", line.c_str());
        }
    }

    reportTargetHealth(options, options.targetQuads, quads, remesher.holeStats());

    const AutoRemesher::SymmetryStats& symmetry = remesher.symmetryStats();

    const double quadsError = (options.targetQuads > 0)
        ? ((double)quads - (double)options.targetQuads) / (double)options.targetQuads
        : 0.0;
    const AutoRemesher::TopologyStats& outputStats = remesher.outputStats();
    const AutoRemesher::HoleStats& holes = remesher.holeStats();
    const AutoRemesher::PatchStats& patches = remesher.patchStats();

    if (options.json) {
        std::ostringstream payload;
        payload.setf(std::ios::fixed);
        payload << "{\"type\":\"result\",\"output\":\"" << jsonEscape(displayPath(options.output)) << "\""
                << ",\"seconds\":" << std::setprecision(3) << seconds
                << ",\"vertices\":" << remesher.remeshedVertices().size()
                << ",\"quads\":" << quads
                << ",\"nonQuads\":" << nonQuads
                << ",\"triangles\":" << triangles.size()
                << ",\"requestedQuads\":" << options.targetQuads
                << ",\"quadsError\":" << std::setprecision(4) << quadsError
                << ",\"exactTargetQuads\":" << (options.exactTargetQuads ? "true" : "false")
                << ",\"preset\":\"" << jsonEscape(options.preset) << "\""
                << ",\"islands\":" << remesher.islandCount()
                << ",\"coresBusy\":" << std::setprecision(2) << remesher.coresBusy()
                << ",\"topology\":{\"input\":" << topologyJson(remesher.inputStats())
                << ",\"output\":" << topologyJson(outputStats) << "}"
                << ",\"holes\":{\"filled\":" << holes.filled
                << ",\"ignored\":" << holes.ignored
                << ",\"facesAdded\":" << holes.facesAdded << "}"
                << ",\"patches\":{\"kept\":" << patches.kept
                << ",\"dropped\":" << patches.dropped << "}"
                << ",\"uvs\":" << (hasUvs ? "true" : "false")
                << ",\"uvCorners\":" << uvCorners
                << ",\"symmetry\":{\"axis\":\"" << symmetryAxisName(options.symmetryAxis) << "\""
                << ",\"seamVertices\":" << symmetry.seamVertices
                << ",\"clippedFaces\":" << symmetry.clippedFaces
                << ",\"mirroredFaces\":" << symmetry.mirroredFaces
                << ",\"verticesBefore\":" << symmetry.verticesBefore
                << ",\"verticesAfter\":" << symmetry.verticesAfter
                << ",\"plane\":" << std::setprecision(6) << symmetry.plane
                << ",\"tolerance\":" << symmetry.tolerance << "}"
                << ",\"normals\":" << (normals.empty() ? "false" : "true")
                << ",\"phases\":" << phasesJson(remesher.phaseTimings())
                << "}";
        emitLine(payload.str());
    } else if (!options.quiet) {
        printf("result: %s\n", displayPath(options.output).c_str());
        printf("  %.3f s, %zu vertices, %zu quads, %zu non-quads (requested %zu, %+.1f%%)\n",
            seconds, remesher.remeshedVertices().size(), quads, nonQuads,
            options.targetQuads, quadsError * 100.0);
        printf("  topology: %zu boundary edge(s) in %zu loop(s), %zu non-manifold edge(s), %zu component(s)\n",
            outputStats.boundaryEdges, outputStats.boundaryLoops,
            outputStats.nonManifoldEdges, outputStats.components);
        printf("  uv: %s (%zu corners), normals: %s\n",
            hasUvs ? "yes" : "none", uvCorners, normals.empty() ? "no" : "yes");
        if (options.symmetryAxis > 0) {
            printf("  symmetry: %s=%.6g, %zu seam vertices, %zu faces cut, %zu mirrored (%zu -> %zu verts)\n",
                symmetryAxisName(options.symmetryAxis), symmetry.plane, symmetry.seamVertices,
                symmetry.clippedFaces, symmetry.mirroredFaces,
                symmetry.verticesBefore, symmetry.verticesAfter);
        }
        if (holes.filled > 0 || holes.ignored > 0 || patches.dropped > 0) {
            printf("  holes: %zu filled, %zu left open (+%zu faces); patches: %zu kept, %zu dropped\n",
                holes.filled, holes.ignored, holes.facesAdded, patches.kept, patches.dropped);
        }
    }
    return 0;
}
