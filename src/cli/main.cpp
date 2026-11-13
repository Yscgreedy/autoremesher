/*
 * AutoRemesher headless CLI.
 *
 * The remeshing core (src/AutoRemesher) has no Qt dependency.  This executable is
 * the only entry point: it loads an OBJ, runs one remesh, writes a quad OBJ, and
 * optionally speaks the JSON-lines protocol documented in docs/cli-protocol.md so
 * the Go GUI can drive it without any Qt code in the process.
 */
#include <AutoRemesher/AutoRemesher>
#include <AutoRemesher/Vector3>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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
    size_t targetQuads = 20000;
    double adaptivity = 0.5;
    double anisotropy = 0.0;
    double sharpEdgeDegrees = 60.0;
    double smoothNormalDegrees = 0.0;
    double edgeScaling = 1.0;
    bool hardSurface = false;
    bool exactTargetQuads = false;
    bool relativeTolerance = true;
    double keepIslandFraction = 0.0;
    bool weldPatches = false;
    bool gradedHoles = true;
    bool projectedCaps = false;
    bool localRemesh = false;
    int threads = 0;
    bool json = false;
    bool progress = false;
    bool verbose = false;
    bool quiet = false;
};

const char* kUsage =
    "AutoRemesher " AR_VERSION " - automatic quad remeshing (headless)\n"
    "\n"
    "Usage:\n"
    "  autoremesher --input MODEL.obj --output OUT.obj --target-quads 20000 [options]\n"
    "\n"
    "Input / output:\n"
    "  -i, --input PATH            input OBJ (polygons are triangulated)\n"
    "  -o, --output PATH           output OBJ of quads (default: <input>_remeshed.obj)\n"
    "\n"
    "Remeshing:\n"
    "  -q, --target-quads N        requested quad count (default 20000)\n"
    "      --adaptivity F          gradient adaptivity 0..1 (default 0.5)\n"
    "      --anisotropy F          anisotropy 0..1 (default 0.0)\n"
    "      --sharp-edge DEG        sharp edge threshold (default 60)\n"
    "      --smooth-normal DEG     smooth normal threshold (default 0)\n"
    "      --edge-scaling F        edge scale multiplier (default 1.0)\n"
    "      --model-type T          organic | hard-surface (default organic)\n"
    "      --exact-target-quads    solve the cover scale from the quad budget\n"
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
    "      --progress              progress on stderr\n"
    "      --verbose               per-phase report at the end\n"
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
        } else if (arg == "--anisotropy") {
            options->anisotropy = number();
        } else if (arg == "--sharp-edge") {
            options->sharpEdgeDegrees = number();
        } else if (arg == "--smooth-normal") {
            options->smoothNormalDegrees = number();
        } else if (arg == "--edge-scaling") {
            options->edgeScaling = number();
        } else if (arg == "--model-type") {
            const std::string type = value();
            if (type == "hard-surface" || type == "hardsurface" || type == "hard") {
                options->hardSurface = true;
            } else if (type == "organic" || type.empty()) {
                options->hardSurface = false;
            } else {
                fprintf(stderr, "unknown model type '%s'\n", type.c_str());
                *exitCode = 1;
                return false;
            }
        } else if (arg == "--exact-target-quads") {
            options->exactTargetQuads = true;
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
        } else if (arg == "--json") {
            options->json = true;
        } else if (arg == "--progress") {
            options->progress = true;
        } else if (arg == "--verbose") {
            options->verbose = true;
        } else if (arg == "--quiet") {
            options->quiet = true;
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

bool writeObj(const std::string& path,
    const std::vector<AutoRemesher::Vector3>& vertices,
    const std::vector<std::vector<size_t>>& faces,
    std::string* error)
{
    FILE* file = fopen(path.c_str(), "wb");
    if (nullptr == file) {
        *error = "failed to write " + displayPath(path);
        return false;
    }
    fprintf(file, "# AutoRemesher %s\n", AR_VERSION);
    for (const auto& vertex : vertices)
        fprintf(file, "v %.17g %.17g %.17g\n", vertex.x(), vertex.y(), vertex.z());
    for (const auto& face : faces) {
        if (face.size() < 3)
            continue;
        fputc('f', file);
        for (const size_t index : face)
            fprintf(file, " %zu", index + 1);
        fputc('\n', file);
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

} // namespace

int main(int argc, char** argv)
{
    Options options;
    int exitCode = 0;
    if (!parseArgs(argc, argv, &options, &exitCode))
        return exitCode;
    if (options.output.empty())
        options.output = defaultOutputPath(options.input);

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
        fprintf(stderr, "  target quads: %zu\n", options.targetQuads);
    }
    if (options.json) {
        char buffer[1024];
        snprintf(buffer, sizeof(buffer),
            "{\"type\":\"start\",\"version\":\"%s\",\"input\":\"%s\",\"output\":\"%s\","
            "\"targetQuads\":%zu,\"vertices\":%zu,\"triangles\":%zu}",
            AR_VERSION, jsonEscape(displayPath(options.input)).c_str(),
            jsonEscape(displayPath(options.output)).c_str(), options.targetQuads,
            vertices.size(), triangles.size());
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
    remesher.setModelType(options.hardSurface ? AutoRemesher::ModelType::HardSurface
                                              : AutoRemesher::ModelType::Organic);
    if (options.json || options.progress) {
        remesher.setProgressHandler(reportProgress);
        remesher.setTag(&sink);
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
        const std::string message = "remeshing failed";
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

    if (!writeObj(options.output, remesher.remeshedVertices(), remesher.remeshedQuads(), &error)) {
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

    if (options.json) {
        char buffer[1024];
        snprintf(buffer, sizeof(buffer),
            "{\"type\":\"result\",\"output\":\"%s\",\"seconds\":%.3f,\"vertices\":%zu,"
            "\"quads\":%zu,\"nonQuads\":%zu,\"triangles\":%zu}",
            jsonEscape(displayPath(options.output)).c_str(), seconds,
            remesher.remeshedVertices().size(), quads, nonQuads, triangles.size());
        emitLine(buffer);
    } else if (!options.quiet) {
        printf("result: %s\n", displayPath(options.output).c_str());
        printf("  %.3f s, %zu vertices, %zu quads, %zu non-quads\n",
            seconds, remesher.remeshedVertices().size(), quads, nonQuads);
    }
    return 0;
}
