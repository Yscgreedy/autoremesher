/*
 * AutoRemesher headless CLI.
 *
 * The remeshing core (src/AutoRemesher) has no Qt dependency.  This executable is
 * the only entry point: it loads an OBJ, runs one remesh, writes a quad OBJ, and
 * optionally speaks the JSON-lines protocol documented in docs/cli-protocol.md so
 * the Go GUI can drive it without any Qt code in the process.
 */
#include <AutoRemesher/AutoRemesher>
#include <AutoRemesher/UvCharts>
#include <AutoRemesher/Vector2>
#include <AutoRemesher/Vector3>
#include <core/base/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <set>
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
    bool measureDeviation = false;
    std::string uvMode = "corner";
    int symmetryAxis = 0; // 0 off, 1 x, 2 y, 3 z
    // What the symmetry flag should do.  "mirror" is the historical behaviour
    // (cut, weld, mirror); "check" only measures and "snap" only moves near-plane
    // vertices, because a single-view reconstruction is not symmetric and
    // cutting it is destructive.
    std::string symmetryMode = "mirror";
    bool symmetryKeepPositive = true;
    double symmetryPlane = 0.0;
    bool symmetryPlaneSet = false;
    double symmetryTolerance = 0.002;
    // Input inspection only: analyse, decide, exit.  No output file, no remesh.
    bool checkOnly = false;
    // A JSON file that overrides every knob the command line did not name.
    std::string presetFile;
    // Only affects the reported units (edge lengths, snap distances); the
    // geometry and every threshold stay in model units.
    std::string unitHint;
    // Every flag the command line actually named, so a preset file can fill in
    // the rest without silently overriding an explicit choice.
    std::set<std::string> explicitFlags;
    // Chart-aware UV packing; --no-uv-pack restores the historical strip layout.
    bool uvPacking = true;
    // Degenerate cleanup: zero length edges, degenerate and duplicate faces.
    bool cleanDegenerate = true;
    // The consumer contract asks for quad_aspect_max <= --max-aspect with 20 as
    // the default, so 20 is the default here; --max-aspect 0 keeps every sliver.
    double maxAspect = 20.0;
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

const char* kUsage = "AutoRemesher " AR_VERSION " - automatic quad remeshing (headless)\n"
                     "\n"
                     "Usage:\n"
                     "  autoremesher --input MODEL.obj --output OUT.obj --target-quads 20000 [options]\n"
                     "\n"
                     "Input / output:\n"
                     "  -i, --input PATH            input OBJ (polygons are triangulated)\n"
                     "  -o, --output PATH           output OBJ of quads (default: <input>_remeshed.obj)\n"
                     "      --no-uv                 do not write vt lines\n"
                     "      --no-normals            do not write vn lines\n"
                     "      --uv-mode M             corner | vertex | hybrid | seam (default corner):\n"
                     "                              corner lays one UV per face corner from the cover\n"
                     "                              chart, vertex shares the extractor's per-vertex\n"
                     "                              UVs, hybrid uses corners only for faces the vertex\n"
                     "                              UVs would stretch, seam cuts the mesh along the\n"
                     "                              chart boundaries so per-vertex UVs can describe\n"
                     "                              them exactly\n"
                     "      --symmetry AXIS         x | y | z (mirror), or check | snap | mirror-half:\n"
                     "                              plane where both sides agree, and count where\n"
                     "                              they do not. check only measures, snap only moves\n"
                     "                              near-plane vertices (no cut), mirror-half keeps one\n"
                     "                              side and mirrors it (default off)\n"
                     "      --symmetry-plane F      coordinate of the mirror plane along the axis\n"
                     "                              (default 0, the coordinate plane)\n"
                     "      --symmetry-tolerance F  seam tolerance as a fraction of the bbox\n"
                     "                              diagonal (default 0.002)\n"
                     "      --symmetry-mode M       check | snap | mirror-half: the same choice spelled out,\n"
                     "                              for scripts that pass the axis separately\n"
                     "      --symmetry-keep S       positive | negative: which side mirror-half keeps\n"
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
                     "      --clean-degenerate      weld zero length edges and drop degenerate or\n"
                     "                              duplicate faces at the end of extraction (default)\n"
                     "      --no-clean-degenerate   keep whatever the topology passes produced\n"
                     "      --max-aspect F          collapse sliver quads whose max/min edge ratio\n"
                     "                              exceeds F (default 20; 0 keeps every sliver)\n"
                     "      --threads N             pin the TBB worker count (0 = all cores)\n"
                     "\n"
                     "Reporting:\n"
                     "      --json                  JSON-lines events on stdout (see docs/cli-protocol.md)\n"
                     "      --check-only            analyse the input and exit: area/volume/bbox/unit\n"
                     "                              guess/shell count and parameter advice, no output\n"
                     "                              file and no remesh (result carries checkOnly)\n"
                     "      --preset-file PATH      JSON file of knob overrides, applied after the preset\n"
                     "      --unit-hint U           mm | cm | m: only changes the units the report prints\n"
                     "                              result carries topology, holes, patches, phases\n"
                     "      --progress              progress on stderr\n"
                     "      --verbose               per-phase report at the end\n"
                     "      --deviation             measure how far the output sits from the input\n"
                     "                              surface (mean/p95/max, exact point-triangle)\n"
                     "      --log-level L           quiet | normal | debug (default normal);\n"
                     "                              debug adds the per-step and per-hole traces\n"
                     "      --quiet                 no human output except errors\n"
                     "  -h, --help                  this text\n"
                     "  -v, --version               version\n"
                     "\n"
                     "Exit codes: 0 success, 1 usage/input error, 2 remeshing failed, 3 output failed.\n";

// ------------------------------------------------------------- preset file
// A deliberately small JSON reader: the file is a flat object of knobs, and being
// strict about what it understands is the point.  Silently ignoring an unknown key
// is the worst outcome available - the caller believes its knob took effect and
// every decision it makes afterwards is wrong.
bool applyPresetFile(const std::string& path, Options* options, int* exitCode)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        fprintf(stderr, "error: failed to open preset file %s\n", displayPath(path).c_str());
        *exitCode = 1;
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    const std::string text = buffer.str();

    const auto namedAny = [options](std::initializer_list<const char*> flags) {
        for (const char* flag : flags) {
            if (options->explicitFlags.end() != options->explicitFlags.find(flag))
                return true;
        }
        return false;
    };

    size_t cursor = 0;
    size_t applied = 0;
    bool sawObject = false;
    while (cursor < text.size()) {
        const char c = text[cursor];
        if (c == '{') {
            sawObject = true;
            ++cursor;
            continue;
        }
        if (c == '}' || c == ',' || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++cursor;
            continue;
        }
        if (c != '"') {
            fprintf(stderr, "error: preset file %s is not a flat JSON object at offset %zu\n",
                displayPath(path).c_str(), cursor);
            *exitCode = 1;
            return false;
        }
        size_t keyEnd = text.find('"', cursor + 1);
        if (std::string::npos == keyEnd) {
            *exitCode = 1;
            return false;
        }
        const std::string key = text.substr(cursor + 1, keyEnd - cursor - 1);
        cursor = keyEnd + 1;
        while (cursor < text.size() && text[cursor] != ':')
            ++cursor;
        if (cursor >= text.size()) {
            *exitCode = 1;
            return false;
        }
        ++cursor;
        while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == '\t'))
            ++cursor;
        bool quoted = false;
        std::string value;
        if (cursor < text.size() && text[cursor] == '"') {
            quoted = true;
            const size_t valueEnd = text.find('"', cursor + 1);
            if (std::string::npos == valueEnd) {
                *exitCode = 1;
                return false;
            }
            value = text.substr(cursor + 1, valueEnd - cursor - 1);
            cursor = valueEnd + 1;
        } else {
            while (cursor < text.size() && text[cursor] != ',' && text[cursor] != '}'
                && text[cursor] != ' ' && text[cursor] != '\n' && text[cursor] != '\r') {
                value.push_back(text[cursor]);
                ++cursor;
            }
        }

        char* end = nullptr;
        const double number = quoted ? 0.0 : strtod(value.c_str(), &end);
        const bool numeric = !quoted && end != value.c_str() && *end == '\0';
        const bool flag = !quoted && (value == "true" || value == "false");
        bool handled = true;

        if (key == "targetQuads" && numeric) {
            if (!namedAny({ "--target-quads", "-q" }))
                options->targetQuads = static_cast<size_t>(std::max(1.0, number));
        } else if (key == "adaptivity" && numeric) {
            if (!namedAny({ "--adaptivity" }))
                options->adaptivity = number;
        } else if (key == "anisotropy" && numeric) {
            if (!namedAny({ "--anisotropy" }))
                options->anisotropy = number;
        } else if (key == "sharpEdgeDegrees" && numeric) {
            if (!namedAny({ "--sharp-edge" }))
                options->sharpEdgeDegrees = number;
        } else if (key == "smoothNormalDegrees" && numeric) {
            if (!namedAny({ "--smooth-normal" }))
                options->smoothNormalDegrees = number;
        } else if (key == "edgeScaling" && numeric) {
            if (!namedAny({ "--edge-scaling" }))
                options->edgeScaling = number;
        } else if (key == "keepIslandFraction" && numeric) {
            if (!namedAny({ "--keep-island-fraction", "--keep-all-islands" }))
                options->keepIslandFraction = number;
        } else if (key == "maxAspect" && numeric) {
            if (!namedAny({ "--max-aspect" }))
                options->maxAspect = number;
        } else if (key == "threads" && numeric) {
            if (!namedAny({ "--threads" }))
                options->threads = static_cast<int>(number);
        } else if (key == "symmetryPlane" && numeric) {
            if (!namedAny({ "--symmetry-plane" })) {
                options->symmetryPlane = number;
                options->symmetryPlaneSet = true;
            }
        } else if (key == "symmetryTolerance" && numeric) {
            if (!namedAny({ "--symmetry-tolerance" }))
                options->symmetryTolerance = number;
        } else if (key == "preset" && quoted) {
            if (!namedAny({ "--preset", "--model-type" })) {
                std::string type = value;
                if (type == "hardsurface" || type == "hard")
                    type = "hard-surface";
                if (nullptr != findPreset(type))
                    options->preset = type;
                else
                    handled = false;
            }
        } else if (key == "uvMode" && quoted) {
            if (!namedAny({ "--uv-mode" }) && (value == "corner" || value == "vertex" || value == "hybrid" || value == "seam"))
                options->uvMode = value;
            else if (namedAny({ "--uv-mode" }))
                handled = true;
            else
                handled = false;
        } else if (key == "unitHint" && quoted) {
            if (!namedAny({ "--unit-hint" }) && (value == "mm" || value == "cm" || value == "m"))
                options->unitHint = value;
            else if (!namedAny({ "--unit-hint" }))
                handled = false;
        } else if (key == "symmetryMode" && quoted) {
            if (!namedAny({ "--symmetry-mode", "--symmetry" })
                && (value == "check" || value == "snap" || value == "mirror" || value == "mirror-half"))
                options->symmetryMode = value == "mirror-half" ? "mirror" : value;
            else if (!namedAny({ "--symmetry-mode", "--symmetry" }))
                handled = false;
        } else if (key == "symmetryAxis" && quoted) {
            if (!namedAny({ "--symmetry" })) {
                if (value == "x")
                    options->symmetryAxis = 1;
                else if (value == "y")
                    options->symmetryAxis = 2;
                else if (value == "z")
                    options->symmetryAxis = 3;
                else if (value == "none")
                    options->symmetryAxis = 0;
                else
                    handled = false;
            }
        } else if (key == "exactTargetQuads" && flag) {
            if (!namedAny({ "--exact-target-quads", "--legacy-target-quads" }))
                options->exactTargetQuads = (value == "true");
        } else if (key == "relativeTolerance" && flag) {
            if (!namedAny({ "--relative-tolerance", "--absolute-tolerance" }))
                options->relativeTolerance = (value == "true");
        } else if (key == "weldPatches" && flag) {
            if (!namedAny({ "--weld-patches" }))
                options->weldPatches = (value == "true");
        } else if (key == "gradedHoles" && flag) {
            if (!namedAny({ "--graded-holes", "--legacy-holes" }))
                options->gradedHoles = (value == "true");
        } else if (key == "projectedCaps" && flag) {
            if (!namedAny({ "--projected-caps" }))
                options->projectedCaps = (value == "true");
        } else if (key == "localRemesh" && flag) {
            if (!namedAny({ "--local-remesh" }))
                options->localRemesh = (value == "true");
        } else if (key == "cleanDegenerate" && flag) {
            if (!namedAny({ "--clean-degenerate", "--no-clean-degenerate" }))
                options->cleanDegenerate = (value == "true");
        } else if (key == "writeUvs" && flag) {
            if (!namedAny({ "--no-uv" }))
                options->writeUvs = (value == "true");
        } else if (key == "writeNormals" && flag) {
            if (!namedAny({ "--no-normals" }))
                options->writeNormals = (value == "true");
        } else if (key == "symmetryKeepPositive" && flag) {
            if (!namedAny({ "--symmetry-keep" }))
                options->symmetryKeepPositive = (value == "true");
        } else {
            handled = false;
        }

        if (!handled) {
            // A warning, not a silent skip and not a hard failure: a typo in a
            // shared preset file should be loud without stopping a batch run.
            fprintf(stderr, "warning: preset file %s: ignored unknown or malformed key '%s'\n",
                displayPath(path).c_str(), key.c_str());
        } else {
            ++applied;
        }
    }
    if (!sawObject) {
        fprintf(stderr, "error: preset file %s is not a JSON object\n", displayPath(path).c_str());
        *exitCode = 1;
        return false;
    }
    if (!options->quiet)
        fprintf(stderr, "preset file: %s (%zu knob(s) applied, command line wins)\n",
            displayPath(path).c_str(), applied);
    return true;
}

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
            else if (axis == "check" || axis == "snap") {
                // A mode without an axis still needs one to measure the mirror
                // plane, and x is what a single-view asset is almost always
                // built along.
                options->symmetryMode = axis;
                if (0 == options->symmetryAxis)
                    options->symmetryAxis = 1;
            } else if (axis == "mirror" || axis == "mirror-half") {
                options->symmetryMode = "mirror";
                if (0 == options->symmetryAxis)
                    options->symmetryAxis = 1;
            } else {
                fprintf(stderr, "unknown symmetry axis '%s' (x|y|z|check|snap|mirror-half|none)\n", axis.c_str());
                *exitCode = 1;
                return false;
            }
        } else if (arg == "--symmetry-mode") {
            const std::string mode = value();
            if (mode == "check" || mode == "snap" || mode == "mirror" || mode == "mirror-half") {
                options->symmetryMode = mode == "mirror-half" ? "mirror" : mode;
                // A mode without an axis still needs one; x is what a single-view
                // asset is built along, and silently doing nothing is worse.
                if (0 == options->symmetryAxis)
                    options->symmetryAxis = 1;
            } else {
                fprintf(stderr, "unknown symmetry mode '%s' (check|snap|mirror-half)\n", mode.c_str());
                *exitCode = 1;
                return false;
            }
        } else if (arg == "--symmetry-keep") {
            const std::string side = value();
            if (side == "positive" || side == "+" || side == "plus") {
                options->symmetryKeepPositive = true;
            } else if (side == "negative" || side == "-" || side == "minus") {
                options->symmetryKeepPositive = false;
            } else {
                fprintf(stderr, "unknown symmetry side '%s' (positive|negative)\n", side.c_str());
                *exitCode = 1;
                return false;
            }
        } else if (arg == "--symmetry-plane") {
            options->symmetryPlane = number();
            options->symmetryPlaneSet = true;
        } else if (arg == "--check-only") {
            options->checkOnly = true;
        } else if (arg == "--preset-file") {
            options->presetFile = value();
        } else if (arg == "--unit-hint") {
            const std::string unit = value();
            if (unit == "mm" || unit == "cm" || unit == "m") {
                options->unitHint = unit;
            } else {
                fprintf(stderr, "unknown unit '%s' (mm|cm|m)\n", unit.c_str());
                *exitCode = 1;
                return false;
            }
        } else if (arg == "--no-clean-degenerate") {
            options->cleanDegenerate = false;
        } else if (arg == "--clean-degenerate") {
            options->cleanDegenerate = true;
        } else if (arg == "--max-aspect") {
            options->maxAspect = number();
        } else if (arg == "--symmetry-tolerance") {
            options->symmetryTolerance = number();
        } else if (arg == "--deviation") {
            options->measureDeviation = true;
        } else if (arg == "--uv-mode") {
            const std::string mode = value();
            if (mode == "corner" || mode == "vertex" || mode == "hybrid" || mode == "seam") {
                options->uvMode = mode;
            } else {
                fprintf(stderr, "unknown uv mode '%s' (corner|vertex|hybrid)\n", mode.c_str());
                *exitCode = 1;
                return false;
            }
        } else if (arg == "--no-uv") {
            options->writeUvs = false;
        } else if (arg == "--uv-pack") {
            options->uvPacking = true;
        } else if (arg == "--no-uv-pack") {
            options->uvPacking = false;
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
        if (!arg.empty() && '-' == arg[0])
            options->explicitFlags.insert(arg);
    }
    if (options->input.empty()) {
        fputs(kUsage, stderr);
        *exitCode = 1;
        return false;
    }
    // Presets are resolved last so an explicit knob always beats the preset,
    // whatever order the flags appeared in.
    applyPreset(options);
    // The file fills in what neither the command line nor the preset named.  A
    // file is how a pipeline pins "its" knobs once instead of repeating twelve
    // flags per call, so it must never quietly beat a flag the caller did pass.
    if (!options->presetFile.empty() && !applyPresetFile(options->presetFile, options, exitCode))
        return false;
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

// The written OBJ read back as polygons.  loadObj triangulates because the
// remesher wants triangles, and triangulating here would erase exactly what the
// comparison is about: a quad written twice is two identical quads, but four
// identical triangles, and the edge counts differ.
bool loadObjPolygons(const std::string& path,
    std::vector<AutoRemesher::Vector3>* vertices,
    std::vector<std::vector<size_t>>* faces,
    std::vector<std::vector<AutoRemesher::Vector2>>* faceUvs,
    std::string* error)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        *error = "failed to reopen " + displayPath(path);
        return false;
    }
    std::vector<AutoRemesher::Vector2> textureCoordinates;
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.size() < 2)
            continue;
        if (line[0] == 'v' && line[1] == 't') {
            const char* cursor = line.c_str() + 3;
            char* end = nullptr;
            const double u = strtod(cursor, &end);
            double v = 0.0;
            if (end != cursor) {
                cursor = end;
                v = strtod(cursor, &end);
            }
            textureCoordinates.push_back(AutoRemesher::Vector2(u, v));
            continue;
        }
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
            std::vector<size_t> faceUvIndices;
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
                long uvIndex = 0;
                if (*cursor == '/') {
                    ++cursor;
                    if (*cursor != '/') {
                        uvIndex = strtol(cursor, &end, 10);
                        if (end != cursor)
                            cursor = end;
                    }
                    // Skip the normal index: this reader only needs v and vt.
                    while (*cursor && *cursor != ' ' && *cursor != '\t')
                        ++cursor;
                }
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
                if (uvIndex > 0)
                    faceUvIndices.push_back(static_cast<size_t>(uvIndex - 1));
                else
                    faceUvIndices.push_back((size_t)-1);
            }
            if (face.size() >= 3) {
                // The UVs are read from the file, not reconstructed from the core:
                // four modes write four different channels, and "what did I write"
                // is exactly the question this measurement answers.
                std::vector<AutoRemesher::Vector2> cornerUvs;
                if (nullptr != faceUvs) {
                    cornerUvs.resize(face.size());
                    for (size_t corner = 0; corner < face.size(); ++corner) {
                        const size_t uvIndex = faceUvIndices[corner];
                        if ((size_t)-1 != uvIndex && uvIndex < textureCoordinates.size())
                            cornerUvs[corner] = textureCoordinates[uvIndex];
                    }
                }
                faces->push_back(std::move(face));
                if (nullptr != faceUvs)
                    faceUvs->push_back(std::move(cornerUvs));
            }
        }
    }
    return true;
}

struct GeometryExtent {
    double area = 0.0;
    double volume = 0.0;
    AutoRemesher::Vector3 minimum;
    AutoRemesher::Vector3 maximum;
    double diagonal = 0.0;
    size_t degenerateEdges = 0;
};

// Area, signed volume and extent of a triangle soup.  The volume is the signed
// divergence sum, so it is only meaningful for a closed shell; a caller can tell
// by the boundary edge count in the same report.
GeometryExtent measureGeometry(const std::vector<AutoRemesher::Vector3>& vertices,
    const std::vector<std::vector<size_t>>& triangles)
{
    GeometryExtent extent;
    if (vertices.empty())
        return extent;
    extent.minimum = vertices[0];
    extent.maximum = vertices[0];
    for (const auto& vertex : vertices) {
        for (int i = 0; i < 3; ++i) {
            extent.minimum[i] = std::min(extent.minimum[i], vertex[i]);
            extent.maximum[i] = std::max(extent.maximum[i], vertex[i]);
        }
    }
    extent.diagonal = (extent.maximum - extent.minimum).length();
    const double minimumEdge = std::max(1e-12, 1e-6 * (extent.diagonal > 0.0 ? extent.diagonal : 1.0));
    std::set<std::pair<size_t, size_t>> degenerate;
    for (const auto& triangle : triangles) {
        if (triangle.size() < 3)
            continue;
        const AutoRemesher::Vector3& a = vertices[triangle[0]];
        const AutoRemesher::Vector3& b = vertices[triangle[1]];
        const AutoRemesher::Vector3& c = vertices[triangle[2]];
        extent.area += 0.5 * AutoRemesher::Vector3::crossProduct(b - a, c - a).length();
        extent.volume += AutoRemesher::Vector3::dotProduct(a, AutoRemesher::Vector3::crossProduct(b, c)) / 6.0;
        for (size_t i = 0; i < 3; ++i) {
            const size_t first = triangle[i];
            const size_t second = triangle[(i + 1) % 3];
            if ((vertices[first] - vertices[second]).length() > minimumEdge)
                continue;
            degenerate.insert(first < second ? std::make_pair(first, second)
                                             : std::make_pair(second, first));
        }
    }
    extent.degenerateEdges = degenerate.size();
    return extent;
}

// The bounding box diagonal is the only scale cue a bare mesh carries, so the
// unit is a guess about the file, not a measurement of it.  The bucket
// boundaries are chosen so a 26 cm shoe, a 2.6 m character and a 260 mm machine
// part all land in the expected bucket.
struct UnitGuess {
    const char* name = "unknown";
    double scaleToMm = 1.0;
};

UnitGuess guessUnit(double diagonal)
{
    UnitGuess guess;
    if (!(diagonal > 0.0))
        return guess;
    if (diagonal < 1.0) {
        guess.name = "m";
        guess.scaleToMm = 1000.0;
    } else if (diagonal < 100.0) {
        guess.name = "cm";
        guess.scaleToMm = 10.0;
    } else {
        guess.name = "mm";
        guess.scaleToMm = 1.0;
    }
    return guess;
}

// --unit-hint wins over the guess: it is the caller saying "I know".
UnitGuess resolveUnit(const std::string& hint, double diagonal)
{
    UnitGuess guess = guessUnit(diagonal);
    if (hint == "mm") {
        guess.name = "mm";
        guess.scaleToMm = 1.0;
    } else if (hint == "cm") {
        guess.name = "cm";
        guess.scaleToMm = 10.0;
    } else if (hint == "m") {
        guess.name = "m";
        guess.scaleToMm = 1000.0;
    }
    return guess;
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
            normal = normal + AutoRemesher::Vector3((current.y() - following.y()) * (current.z() + following.z()), (current.z() - following.z()) * (current.x() + following.x()), (current.x() - following.x()) * (current.y() + following.y()));
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
    const std::vector<AutoRemesher::Vector2>& vertexUvs,
    const std::vector<std::vector<AutoRemesher::Vector2>>& faceUvs,
    const std::vector<AutoRemesher::Vector3>& normals,
    const std::string& mode,
    std::string* uvMode,
    size_t* writtenUvs,
    std::string* error)
{
    // Write beside the target and rename once: a cancelled or crashed run must not
    // leave a half-written OBJ that a caller can mistake for a result.  Same
    // directory, so the rename is atomic on every filesystem that matters.
    const std::string temporaryPath = path + ".tmp";
    FILE* file = fopen(temporaryPath.c_str(), "wb");
    if (nullptr == file) {
        *error = "failed to write " + displayPath(path);
        return false;
    }
    const bool wantVertex = mode != "corner";
    const bool wantCorner = mode == "corner" || mode == "hybrid";
    const bool hasVertexUvs = wantVertex && vertexUvs.size() == vertices.size() && !vertices.empty();
    const bool hasFaceUvs = wantCorner && faceUvs.size() == faces.size() && !faces.empty();
    const bool hasNormals = normals.size() == vertices.size() && !vertices.empty();

    // Per face: keep the shared vertex UVs unless this face cannot be described by
    // them.  A face whose corners come from different charts of the cover would
    // stretch across the seam, so it alone takes per-corner UVs.
    const auto faceArea = [](const std::vector<AutoRemesher::Vector2>& source, const std::vector<size_t>& face) {
        double area = 0.0;
        for (size_t i = 0; i < face.size(); ++i) {
            const size_t j = (i + 1) % face.size();
            area += source[face[i]].x() * source[face[j]].y()
                - source[face[j]].x() * source[face[i]].y();
        }
        return std::fabs(0.5 * area);
    };
    std::vector<char> cornerFace(faces.size(), 0);
    if (hasVertexUvs && hasFaceUvs) {
        // Compare the two channels face by face: they describe the same chart, so
        // their areas should agree.  When they do not, the vertex UVs are mixing
        // charts and only the per-corner ones keep that face honest.
        for (size_t f = 0; f < faces.size(); ++f) {
            const auto& face = faces[f];
            if (face.size() < 3 || faceUvs[f].size() != face.size())
                continue;
            std::vector<size_t> local(face.size());
            for (size_t i = 0; i < local.size(); ++i)
                local[i] = i;
            const double vertexArea = faceArea(vertexUvs, face);
            const double cornerArea = faceArea(faceUvs[f], local);
            cornerFace[f] = 1;
            if (vertexArea > 0.0 && cornerArea > 0.0) {
                const double ratio = vertexArea / cornerArea;
                if (ratio > 0.5 && ratio < 2.0)
                    cornerFace[f] = 0;
            }
        }
    }

    // Corner UVs are appended after the vertex ones; remember where each face starts.
    std::vector<size_t> cornerBase(faces.size(), 0);
    bool anyCorner = false;
    size_t cornerCursor = hasVertexUvs ? vertices.size() : 0;
    for (size_t f = 0; f < faces.size(); ++f) {
        const auto& face = faces[f];
        if (face.size() < 3)
            continue;
        const bool wantCorner = hasFaceUvs && faceUvs[f].size() == face.size()
            && (!hasVertexUvs || cornerFace[f]);
        if (!wantCorner)
            continue;
        cornerBase[f] = cornerCursor;
        cornerCursor += face.size();
        anyCorner = true;
    }

    fprintf(file, "# AutoRemesher %s\n", AR_VERSION);
    for (const auto& vertex : vertices)
        fprintf(file, "v %.17g %.17g %.17g\n", vertex.x(), vertex.y(), vertex.z());
    if (hasVertexUvs) {
        for (const auto& uv : vertexUvs)
            fprintf(file, "vt %.17g %.17g\n", uv.x(), uv.y());
    }
    if (anyCorner) {
        for (size_t f = 0; f < faces.size(); ++f) {
            const auto& face = faces[f];
            if (face.size() < 3)
                continue;
            const bool wantCorner = hasFaceUvs && faceUvs[f].size() == face.size()
                && (!hasVertexUvs || cornerFace[f]);
            if (!wantCorner)
                continue;
            for (const auto& uv : faceUvs[f])
                fprintf(file, "vt %.17g %.17g\n", uv.x(), uv.y());
        }
    }
    if (hasNormals) {
        for (const auto& normal : normals)
            fprintf(file, "vn %.17g %.17g %.17g\n", normal.x(), normal.y(), normal.z());
    }

    size_t vertexModeFaces = 0;
    size_t cornerModeFaces = 0;
    for (size_t f = 0; f < faces.size(); ++f) {
        const auto& face = faces[f];
        if (face.size() < 3)
            continue;
        const bool wantCorner = hasFaceUvs && faceUvs[f].size() == face.size()
            && (!hasVertexUvs || cornerFace[f]);
        if (wantCorner)
            ++cornerModeFaces;
        else if (hasVertexUvs)
            ++vertexModeFaces;
        fputc('f', file);
        for (size_t corner = 0; corner < face.size(); ++corner) {
            const size_t vertexIndex = face[corner] + 1;
            if (wantCorner) {
                const size_t uvIndex = cornerBase[f] + corner + 1;
                if (hasNormals)
                    fprintf(file, " %zu/%zu/%zu", vertexIndex, uvIndex, vertexIndex);
                else
                    fprintf(file, " %zu/%zu", vertexIndex, uvIndex);
            } else if (hasVertexUvs) {
                if (hasNormals)
                    fprintf(file, " %zu/%zu/%zu", vertexIndex, vertexIndex, vertexIndex);
                else
                    fprintf(file, " %zu/%zu", vertexIndex, vertexIndex);
            } else if (hasNormals) {
                fprintf(file, " %zu//%zu", vertexIndex, vertexIndex);
            } else {
                fprintf(file, " %zu", vertexIndex);
            }
        }
        fputc('\n', file);
    }

    if (nullptr != uvMode) {
        if (cornerModeFaces > 0 && vertexModeFaces > 0)
            *uvMode = "hybrid";
        else if (cornerModeFaces > 0)
            *uvMode = "corner";
        else if (vertexModeFaces > 0)
            *uvMode = "vertex";
        else
            *uvMode = "none";
    }
    if (nullptr != writtenUvs)
        *writtenUvs = cornerCursor;
    const bool closed = 0 == fclose(file);
    if (!closed) {
        std::remove(temporaryPath.c_str());
        *error = "failed to flush " + displayPath(path);
        return false;
    }
    // The rename is what makes the result appear: until this line the caller's
    // path does not exist, so a cancel in the middle leaves nothing to pick up.
    if (0 != std::rename(temporaryPath.c_str(), path.c_str())) {
        std::remove(temporaryPath.c_str());
        *error = "failed to move the written mesh onto " + displayPath(path);
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

// A cancelled run must not leave a file behind that looks like a result, and it
// must say so on the protocol instead of dying silently.  The flag is set from the
// signal handler (only a store, which is all a handler may do) and acted on at the
// next progress callback, i.e. inside the core, where the expensive work actually
// happens.
std::atomic<bool> g_interrupted(false);

extern "C" void handleInterrupt(int)
{
    g_interrupted.store(true);
}

struct ProgressSink {
    std::mutex mutex;
    bool json = false;
    bool human = false;
    // Where the writer is parked while the mesh is being written, so an abort can
    // remove it: the temporary is not an OBJ a caller should ever pick up.
    std::string temporaryOutput;
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
    if (g_interrupted.load()) {
        // Exit code 130 is the shell's convention for "died on SIGINT", so a batch
        // driver sees a cancel without having to parse stderr.
        if (sink->json)
            emitLine("{\"type\":\"abort\",\"phase\":\"" + jsonEscape(nullptr != status ? status : "") + "\"}");
        if (!sink->temporaryOutput.empty())
            std::remove(sink->temporaryOutput.c_str());
        std::_Exit(130);
    }
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

std::string deviationJson(const AutoRemesher::DeviationStats& stats)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out << "{\"samples\":" << stats.samples
        << ",\"unresolved\":" << stats.unresolved
        << ",\"mean\":" << std::setprecision(6) << stats.mean
        << ",\"p50\":" << stats.p50
        << ",\"p95\":" << stats.p95
        << ",\"max\":" << stats.max
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

// Quality of the output quads.  These are the numbers a retopology reviewer looks
// at, and every one of them used to require a second tool: poles away from the
// border, how far a quad is from a square, and whether an edge has collapsed to
// nothing.  The units are the input's units; unit_scale_to_mm is elsewhere in the
// same report so a caller can print millimetres without guessing.
std::string qualityJson(const AutoRemesher::QualityStats& quality)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out << "{\"poles_pct\":" << std::setprecision(2) << quality.polesPct
        << ",\"pole_count\":" << quality.poleCount
        << ",\"interior_vertices\":" << quality.interiorVertices
        << ",\"valence_max\":" << quality.valenceMax
        << ",\"valence_hist\":{\"3\":" << quality.valence3
        << ",\"4\":" << quality.valence4
        << ",\"5\":" << quality.valence5
        << ",\"gt5\":" << quality.valenceGt5 << "}"
        << ",\"edge_len\":{\"min\":" << std::setprecision(9) << quality.edgeLength.min
        << ",\"p50\":" << quality.edgeLength.p50
        << ",\"p95\":" << quality.edgeLength.p95
        << ",\"max\":" << quality.edgeLength.max << "}"
        << ",\"quad_aspect\":{\"p50\":" << std::setprecision(4) << quality.quadAspect.p50
        << ",\"p95\":" << quality.quadAspect.p95
        << ",\"max\":" << quality.quadAspect.max << "}"
        << ",\"quad_aspect_area\":{\"max\":" << quality.quadAspectAreaMax << "}"
        << ",\"zero_length_edges\":" << quality.zeroLengthEdges
        << ",\"degenerate_faces\":" << quality.degenerateFaces
        << "}";
    return out.str();
}

bool topologiesAgree(const AutoRemesher::TopologyStats& first,
    const AutoRemesher::TopologyStats& second)
{
    return first.vertices == second.vertices
        && first.faces == second.faces
        && first.quads == second.quads
        && first.nonQuads == second.nonQuads
        && first.boundaryEdges == second.boundaryEdges
        && first.boundaryLoops == second.boundaryLoops
        && first.nonManifoldEdges == second.nonManifoldEdges
        && first.components == second.components
        && first.degenerateFaces == second.degenerateFaces;
}

// Whether the UV layout can be baked into a texture without two faces fighting
// for the same texels.  area > 1 means the layout overlaps itself even before
// the per-triangle test runs, so both have to hold.
std::string uvStatsJson(const AutoRemesher::UvStats& stats, const std::string& mode,
    size_t chartCount, const AutoRemesher::UvPackResult& pack)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    const size_t charts = chartCount > 0 ? chartCount : stats.chartCount;
    out << "{\"mode\":\"" << mode << "\""
        << ",\"area\":" << std::setprecision(6) << stats.area
        << ",\"overlap_pct\":" << std::setprecision(3) << stats.overlapPct
        << ",\"chart_count\":" << charts
        << ",\"max_stretch\":" << std::setprecision(4) << stats.maxStretch
        << ",\"median_stretch\":" << stats.medianStretch
        << ",\"stray_triangles\":" << stats.strayTriangles
        << ",\"bakeable\":" << ((stats.area > 1e-12 && stats.area <= 1.0 + 1e-6 && stats.overlapPct < 1.0) ? "true" : "false")
        << ",\"packed\":" << (pack.ok ? "true" : "false")
        << ",\"packed_charts\":" << pack.packedCharts
        << ",\"pack_fill\":" << std::setprecision(4) << pack.fillRatio
        << "}";
    return out.str();
}

// The scale the budget was solved with, next to the count it produced.  Without
// both, "why did 20k come out as 17k" has no answer in the report.
std::string budgetJson(const Options& options, size_t quads, double error,
    const AutoRemesher::TopologyStats& outputStats)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out << "{\"requested\":" << options.targetQuads
        << ",\"actual\":" << quads
        << ",\"error\":" << std::setprecision(4) << error
        << ",\"withinTolerance\":" << (std::fabs(error) <= 0.05 ? "true" : "false")
        << ",\"mode\":\"" << (options.exactTargetQuads ? "solved" : "average-edge-length") << "\""
        << ",\"non_quads\":" << outputStats.nonQuads
        << "}";
    return out.str();
}

// Every knob as the run actually used it: after --preset, after --preset-file,
// after the command line.  Keys keep the CLI spellings so a caller can diff this
// block against the command line it sent instead of reading the source.
std::string effectiveParamsJson(const Options& options)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out << "{\"targetQuads\":" << options.targetQuads
        << ",\"adaptivity\":" << std::setprecision(4) << options.adaptivity
        << ",\"anisotropy\":" << options.anisotropy
        << ",\"sharpEdgeDegrees\":" << options.sharpEdgeDegrees
        << ",\"smoothNormalDegrees\":" << options.smoothNormalDegrees
        << ",\"edgeScaling\":" << options.edgeScaling
        << ",\"preset\":\"" << jsonEscape(options.preset) << "\""
        << ",\"exactTargetQuads\":" << (options.exactTargetQuads ? "true" : "false")
        << ",\"relativeTolerance\":" << (options.relativeTolerance ? "true" : "false")
        << ",\"keepIslandFraction\":" << options.keepIslandFraction
        << ",\"weldPatches\":" << (options.weldPatches ? "true" : "false")
        << ",\"gradedHoles\":" << (options.gradedHoles ? "true" : "false")
        << ",\"projectedCaps\":" << (options.projectedCaps ? "true" : "false")
        << ",\"localRemesh\":" << (options.localRemesh ? "true" : "false")
        << ",\"cleanDegenerate\":" << (options.cleanDegenerate ? "true" : "false")
        << ",\"maxAspect\":" << options.maxAspect
        << ",\"threads\":" << options.threads
        << ",\"uvMode\":\"" << jsonEscape(options.uvMode) << "\""
        << ",\"writeUvs\":" << (options.writeUvs ? "true" : "false")
        << ",\"writeNormals\":" << (options.writeNormals ? "true" : "false")
        << ",\"symmetryMode\":\"" << jsonEscape(options.symmetryMode) << "\""
        << ",\"symmetryKeepPositive\":" << (options.symmetryKeepPositive ? "true" : "false")
        << ",\"symmetryAxis\":\"" << symmetryAxisName(options.symmetryAxis) << "\""
        << ",\"symmetryPlane\":" << std::setprecision(6) << options.symmetryPlane
        << ",\"symmetryTolerance\":" << options.symmetryTolerance
        << ",\"unitHint\":\"" << jsonEscape(options.unitHint.empty() ? "guess" : options.unitHint) << "\""
        << "}";
    return out.str();
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

// What --check-only recommends, and why.  The point of the advice block is that
// a batch runner does not have to pay for a full remesh to find out that the
// file it was handed needs --projected-caps: every field here is derived from the
// input alone, and the reason string is what a human reads when the advice
// disagrees with their own judgement.
struct Advisory {
    std::string preset = "organic";
    bool weldPatches = false;
    bool projectedCaps = false;
    bool expectHoles = false;
    double sharpEdgeShare = 0.0; // share of edges whose dihedral exceeds 40 deg
    size_t longestBoundaryLoop = 0;
    std::string reason;
};

Advisory buildAdvisory(const std::vector<AutoRemesher::Vector3>& vertices,
    const std::vector<std::vector<size_t>>& triangles,
    const AutoRemesher::TopologyStats& stats)
{
    Advisory advice;
    advice.weldPatches = stats.components > 1;
    advice.expectHoles = stats.boundaryLoops > 0;

    // One pass over the triangle edges: dihedral angle for the crease share, and
    // the boundary edge graph for the longest open loop.
    std::map<std::pair<size_t, size_t>, std::vector<size_t>> edgeTriangles;
    std::vector<AutoRemesher::Vector3> faceNormals(triangles.size());
    for (size_t index = 0; index < triangles.size(); ++index) {
        const auto& triangle = triangles[index];
        if (triangle.size() < 3)
            continue;
        const AutoRemesher::Vector3& a = vertices[triangle[0]];
        const AutoRemesher::Vector3& b = vertices[triangle[1]];
        const AutoRemesher::Vector3& c = vertices[triangle[2]];
        AutoRemesher::Vector3 normal = AutoRemesher::Vector3::crossProduct(b - a, c - a);
        const double length = normal.length();
        faceNormals[index] = length > 0.0 ? normal * (1.0 / length) : AutoRemesher::Vector3();
        for (size_t i = 0; i < 3; ++i) {
            const size_t first = triangle[i];
            const size_t second = triangle[(i + 1) % 3];
            const auto edge = first < second ? std::make_pair(first, second)
                                             : std::make_pair(second, first);
            edgeTriangles[edge].push_back(index);
        }
    }
    size_t measuredEdges = 0;
    size_t sharpEdges = 0;
    std::map<size_t, std::vector<size_t>> boundaryNeighbors;
    for (const auto& it : edgeTriangles) {
        if (it.second.size() > 2)
            continue;
        ++measuredEdges;
        if (2 == it.second.size()) {
            const double cosine = AutoRemesher::Vector3::dotProduct(
                faceNormals[it.second[0]], faceNormals[it.second[1]]);
            const double degrees = std::acos(std::max(-1.0, std::min(1.0, cosine))) * 180.0 / 3.14159265358979323846;
            if (degrees > 40.0)
                ++sharpEdges;
        } else {
            boundaryNeighbors[it.first.first].push_back(it.first.second);
            boundaryNeighbors[it.first.second].push_back(it.first.first);
        }
    }
    advice.sharpEdgeShare = measuredEdges > 0 ? (double)sharpEdges / (double)measuredEdges : 0.0;
    // A crease-heavy model wants the hard-surface point: the preset marks more
    // edges sharp and lets density follow curvature less, which is what keeps flat
    // panels evenly dense.
    advice.preset = advice.sharpEdgeShare > 0.05 ? "hard-surface" : "organic";

    // Longest boundary walk, by degree-2 chaining: the graded hole filler gives up
    // above 65 edges unless projected caps are allowed, so that number is the
    // advice threshold, not a guess.
    std::set<size_t> visited;
    for (const auto& it : boundaryNeighbors) {
        if (visited.end() != visited.find(it.first))
            continue;
        size_t length = 0;
        size_t current = it.first;
        while (visited.end() == visited.find(current)) {
            visited.insert(current);
            ++length;
            size_t next = current;
            bool advanced = false;
            for (const size_t candidate : boundaryNeighbors[current]) {
                if (visited.end() == visited.find(candidate)) {
                    next = candidate;
                    advanced = true;
                    break;
                }
            }
            if (!advanced)
                break;
            current = next;
        }
        advice.longestBoundaryLoop = std::max(advice.longestBoundaryLoop, length);
    }
    advice.projectedCaps = advice.longestBoundaryLoop > 65;

    std::ostringstream reason;
    reason.setf(std::ios::fixed);
    reason.precision(1);
    if (advice.expectHoles)
        reason << stats.boundaryLoops << " open boundary loop(s), longest " << advice.longestBoundaryLoop
               << " edge(s); the output keeps a boundary unless the hole is small enough to fill";
    else
        reason << "closed shell, no boundary to fill";
    if (advice.weldPatches)
        reason << "; " << stats.components
               << " component(s) means each is remeshed as its own island, so --weld-patches keeps them joined";
    if (advice.projectedCaps)
        reason << "; a loop above 65 edges is only sealed with --projected-caps";
    reason << "; " << sharpEdges << " of " << measuredEdges << " edges are creases, preset "
           << advice.preset;
    advice.reason = reason.str();
    return advice;
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
    // A cancel is acted on inside the core's progress callback, which is where the
    // work actually is; the handler itself only sets the flag.
    std::signal(SIGINT, handleInterrupt);
#ifdef SIGTERM
    std::signal(SIGTERM, handleInterrupt);
#endif

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
    // The writer's staging path, so an abort before the rename can clean it up.
    sink.temporaryOutput = options.output + ".tmp";

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
    remesher.setMaximumAspect(options.maxAspect);
    remesher.setExportUvs(options.writeUvs);
    // A per-vertex UV channel can only describe charts that do not share vertices,
    // so packing a vertex-mode layout needs the mesh cut at the chart seams.  Without
    // the cut, two charts average into one UV and the quad that spans them becomes
    // enormous in UV space - which is not a layout, it is a bug you can see.
    const bool wantsVertexChannel = options.uvMode == "vertex" || options.uvMode == "seam";
    const bool seamSplit = wantsVertexChannel && options.uvPacking && options.symmetryAxis == 0;
    remesher.setUvSeamSplitting(seamSplit);
    // Packing rewrites the per-corner channel.  A per-vertex channel cannot be
    // packed chart by chart without first grouping the faces by vertex
    // connectivity, so it keeps the historical layout for now; --uv-mode seam is
    // the mode that is meant to be baked.
    remesher.setUvPacking(options.uvPacking && !(options.uvMode == "vertex" || options.uvMode == "seam"));
    remesher.setMeasureDeviation(options.measureDeviation);
    if (options.uvMode == "seam" && options.symmetryAxis > 0) {
        if (options.json) {
            std::ostringstream line;
            line << "{\"type\":\"warning\",\"code\":\"uv_seam_split_disabled\",\"value\":0"
                 << ",\"message\":\"seam splitting is disabled when --symmetry is on; the UVs\""
                 << "\" stay per-vertex to keep the mirrored mesh watertight\"}";
            emitLine(line.str());
        }
        if (!options.quiet)
            fprintf(stderr, "warning: seam splitting is disabled with --symmetry; UVs stay per-vertex\n");
    }
    remesher.setSymmetryAxis(options.symmetryAxis);
    // Only an explicitly named plane overrides the estimate: a check on a model
    // that is not centred on the origin would otherwise measure against a plane
    // the caller never chose.
    if (options.symmetryPlaneSet)
        remesher.setSymmetryPlane(options.symmetryPlane);
    remesher.setSymmetryTolerance(options.symmetryTolerance);
    remesher.setSymmetryMode(options.symmetryMode == "check" ? 1 : (options.symmetryMode == "snap" ? 2 : 0));
    remesher.setSymmetryKeepPositive(options.symmetryKeepPositive);
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
    const auto analysisStart = std::chrono::steady_clock::now();
    remesher.analyzeInput();
    const double analysisSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - analysisStart)
                                       .count();
    reportInputHealth(options, remesher.inputStats());
    if (options.json) {
        std::ostringstream event;
        event << "{\"type\":\"input\",\"topology\":" << topologyJson(remesher.inputStats()) << "}";
        emitLine(event.str());
    }

    if (options.checkOnly) {
        // Analyse and stop.  Everything below costs seconds to minutes, and a batch
        // runner asks "is this file worth remeshing, and with which flags" twenty
        // times before it asks for one remesh.  Nothing is written: the point of
        // the mode is that it cannot leave a half-described artifact behind.
        const GeometryExtent extent = measureGeometry(vertices, triangles);
        const UnitGuess unit = resolveUnit(options.unitHint, extent.diagonal);
        const AutoRemesher::TopologyStats& inputStats = remesher.inputStats();
        const Advisory advice = buildAdvisory(vertices, triangles, inputStats);
        const bool volumeClosed = 0 == inputStats.boundaryEdges;
        if (options.json) {
            std::ostringstream payload;
            payload.setf(std::ios::fixed);
            payload << "{\"type\":\"result\",\"checkOnly\":true,\"output\":null"
                    << ",\"seconds\":" << std::setprecision(3) << analysisSeconds
                    << ",\"topology\":{\"input\":" << topologyJson(inputStats) << "}"
                    << ",\"vertices\":" << vertices.size()
                    << ",\"triangles\":" << triangles.size()
                    << ",\"area\":" << std::setprecision(9) << extent.area
                    << ",\"volume\":" << std::setprecision(9) << (volumeClosed ? extent.volume : 0.0)
                    << ",\"volume_closed\":" << (volumeClosed ? "true" : "false")
                    << ",\"bbox\":{\"min\":[" << extent.minimum.x() << "," << extent.minimum.y() << "," << extent.minimum.z() << "]"
                    << ",\"max\":[" << extent.maximum.x() << "," << extent.maximum.y() << "," << extent.maximum.z() << "]"
                    << ",\"diagonal\":" << extent.diagonal << "}"
                    << ",\"unit_guess\":\"" << unit.name << "\""
                    << ",\"unit_scale_to_mm\":" << unit.scaleToMm
                    << ",\"boundary_loops\":" << inputStats.boundaryLoops
                    << ",\"boundary_edges\":" << inputStats.boundaryEdges
                    << ",\"degenerate_edges\":" << extent.degenerateEdges
                    << ",\"shell_count\":" << inputStats.components
                    << ",\"advice\":{\"preset\":\"" << advice.preset << "\""
                    << ",\"weld_patches\":" << (advice.weldPatches ? "true" : "false")
                    << ",\"projected_caps\":" << (advice.projectedCaps ? "true" : "false")
                    << ",\"expect_holes\":" << (advice.expectHoles ? "true" : "false")
                    << ",\"sharp_edge_share\":" << std::setprecision(4) << advice.sharpEdgeShare
                    << ",\"longest_boundary_loop\":" << advice.longestBoundaryLoop
                    << ",\"reason\":\"" << jsonEscape(advice.reason) << "\"}"
                    << ",\"effective_params\":" << effectiveParamsJson(options)
                    << "}";
            emitLine(payload.str());
        } else if (!options.quiet) {
            printf("check: %s\n", displayPath(options.input).c_str());
            printf("  %.3f s, %zu vertices, %zu triangles, %.4g %s^2 area, %.4g %s^3 volume%s\n",
                analysisSeconds, vertices.size(), triangles.size(), extent.area, unit.name,
                volumeClosed ? extent.volume : 0.0, unit.name, volumeClosed ? "" : " (open shell)");
            printf("  bbox: %.6g x %.6g x %.6g %s (diagonal %.6g)\n",
                extent.maximum.x() - extent.minimum.x(), extent.maximum.y() - extent.minimum.y(),
                extent.maximum.z() - extent.minimum.z(), unit.name, extent.diagonal);
            printf("  topology: %zu shell(s), %zu boundary edge(s) in %zu loop(s), %zu degenerate edge(s)\n",
                inputStats.components, inputStats.boundaryEdges, inputStats.boundaryLoops,
                extent.degenerateEdges);
            printf("  advice: preset %s%s%s; %s\n", advice.preset.c_str(),
                advice.weldPatches ? ", weld-patches" : "", advice.projectedCaps ? ", projected-caps" : "",
                advice.reason.c_str());
        }
        return 0;
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

    const std::vector<AutoRemesher::Vector2>& vertexUvs = remesher.remeshedVertexUvs();
    const std::vector<std::vector<AutoRemesher::Vector2>>& faceUvs = remesher.remeshedFaceUvs();
    std::vector<AutoRemesher::Vector3> normals;
    if (options.writeNormals)
        normals = computeVertexNormals(remesher.remeshedVertices(), remesher.remeshedQuads());

    std::string uvMode = "none";
    size_t uvCorners = 0;
    const std::string writerMode = options.uvMode == "seam" ? "vertex" : options.uvMode;
    if (!writeObj(options.output, remesher.remeshedVertices(), remesher.remeshedQuads(),
            vertexUvs, faceUvs, normals, writerMode, &uvMode, &uvCorners, &error)) {
        if (options.json)
            emitLine(errorEvent(error));
        fprintf(stderr, "error: %s\n", error.c_str());
        return 3;
    }
    // "seam" writes the same per-vertex channel as "vertex"; keep the requested
    // name in the report so a caller can tell how the UVs were produced.
    if (options.uvMode == "seam" && uvMode == "vertex")
        uvMode = "seam";

    // Re-read what was actually written.  The topology block above describes the
    // mesh in memory, and a caller comparing it against its own measurement of the
    // file is entitled to know the two agree: they did not, once - duplicate faces
    // in the file turned boundary edges into three-face edges, which is exactly
    // what an outside tool reports as non-manifold.
    std::vector<AutoRemesher::Vector3> writtenVertices;
    std::vector<std::vector<size_t>> writtenFaces;
    std::vector<std::vector<AutoRemesher::Vector2>> writtenFaceUvs;
    bool haveExported = false;
    AutoRemesher::TopologyStats exportedStats;
    {
        std::string readError;
        if (loadObjPolygons(options.output, &writtenVertices, &writtenFaces, &writtenFaceUvs, &readError)) {
            exportedStats = AutoRemesher::analyzeTopology(writtenVertices, writtenFaces);
            haveExported = true;
        } else {
            emitWarning(options, "output_reload_failed", readError, 0);
        }
    }
    AutoRemesher::QualityStats quality = remesher.qualityStats();
    AutoRemesher::UvStats uvStats;
    bool haveUvStats = false;
    if (options.writeUvs && uvMode != "none" && haveExported) {
        // Measured from the file, not from the core's arrays: the four UV modes
        // write four different channels, and "is what I wrote bakeable" is a
        // question about the file.  Reading the vt indices back resolves per-corner
        // and per-vertex channels the same way a baker resolves them.
        uvStats = AutoRemesher::collectUvStats(writtenVertices, writtenFaces, writtenFaceUvs,
            std::vector<int>());
        uvStats.chartCount = remesher.uvChartCount();
        haveUvStats = true;
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

    // Symmetry advice, never a veto: on a single-view reconstruction the honest
    // answer is usually "this mesh is not symmetric", and the caller decides
    // whether that matters.  A mirror that had to cut a large share of the model
    // is reported for the same reason - it is the case where the pass does damage.
    if (options.symmetryAxis > 0) {
        const AutoRemesher::SymmetryCheck& check = remesher.symmetryCheck();
        if (check.consideredVertices > 0 && check.matchPct < 80.0) {
            std::ostringstream message;
            message.setf(std::ios::fixed);
            message.precision(1);
            message << "only " << check.matchPct << "% of the vertices have a mirror partner within "
                    << options.symmetryTolerance * 100.0 << "% of the bbox diagonal; mirroring this "
                    << "mesh will cut real geometry";
            emitWarning(options, "symmetry_mismatch", message.str(),
                (long long)check.matchedVertices);
        }
        const size_t facesBeforeMirror = remesher.remeshedQuads().size();
        if (remesher.symmetryStats().clippedFaces > 0 && facesBeforeMirror > 0
            && remesher.symmetryStats().clippedFaces * 50 > facesBeforeMirror) {
            std::ostringstream message;
            message << remesher.symmetryStats().clippedFaces
                    << " face(s) were cut by the mirror plane (more than 2% of the mesh); "
                    << "--symmetry check measures instead, --symmetry snap moves the seam without cutting";
            emitWarning(options, "symmetry_clip_heavy", message.str(),
                (long long)remesher.symmetryStats().clippedFaces);
        }
    }

    const AutoRemesher::SymmetryStats& symmetry = remesher.symmetryStats();

    const double quadsError = (options.targetQuads > 0)
        ? ((double)quads - (double)options.targetQuads) / (double)options.targetQuads
        : 0.0;
    const AutoRemesher::TopologyStats& outputStats = remesher.outputStats();
    const AutoRemesher::HoleStats& holes = remesher.holeStats();
    const AutoRemesher::PatchStats& patches = remesher.patchStats();

    // The report prints edge lengths in model units; unit_guess says which unit the
    // file looks like it is in, and --unit-hint overrides the guess.  Only the
    // printed units move, never a threshold.
    const GeometryExtent outputExtent = measureGeometry(remesher.remeshedVertices(), remesher.remeshedQuads());
    const UnitGuess reportUnit = resolveUnit(options.unitHint, outputExtent.diagonal);

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
                << ",\"internal\":" << topologyJson(outputStats)
                // v1 keeps "output" as an alias of "internal" for one release; the
                // pair below is what a caller should read from now on.
                << ",\"output\":" << topologyJson(outputStats)
                << ",\"exported\":" << (haveExported ? topologyJson(exportedStats) : "null")
                << ",\"same\":" << ((haveExported && topologiesAgree(outputStats, exportedStats)) ? "true" : "false")
                << "}"
                << ",\"quality\":" << qualityJson(quality)
                << ",\"budget\":" << budgetJson(options, quads, quadsError, outputStats)
                << ",\"unit_guess\":\"" << reportUnit.name << "\""
                << ",\"unit_scale_to_mm\":" << reportUnit.scaleToMm
                << ",\"holes\":{\"filled\":" << holes.filled
                << ",\"ignored\":" << holes.ignored
                << ",\"facesAdded\":" << holes.facesAdded << "}"
                << ",\"patches\":{\"kept\":" << patches.kept
                << ",\"dropped\":" << patches.dropped
                << ",\"welded_vertices\":" << patches.weldedVertices
                << ",\"remaining_shells\":" << patches.remainingShells
                << ",\"reason\":\"" << jsonEscape(patches.reason) << "\"}"
                << ",\"uvs\":" << (uvMode != "none" ? "true" : "false")
                << ",\"uvMode\":\"" << uvMode << "\""
                << ",\"uvCorners\":" << uvCorners
                << ",\"uvStats\":" << (haveUvStats ? uvStatsJson(uvStats, uvMode, remesher.uvChartCount(), remesher.uvPackResult()) : "null")
                << ",\"symmetry\":{\"axis\":\"" << symmetryAxisName(options.symmetryAxis) << "\""
                << ",\"mode\":\"" << jsonEscape(options.symmetryMode) << "\""
                << ",\"keep_positive\":" << (options.symmetryKeepPositive ? "true" : "false")
                << ",\"match_pct\":" << std::setprecision(3) << remesher.symmetryCheck().matchPct
                << ",\"max_deviation\":" << std::setprecision(9) << remesher.symmetryCheck().maxDeviation
                << ",\"mean_deviation\":" << remesher.symmetryCheck().meanDeviation
                << ",\"plane_estimate\":" << remesher.symmetryCheck().plane
                << ",\"matched_vertices\":" << remesher.symmetryCheck().matchedVertices
                << ",\"considered_vertices\":" << remesher.symmetryCheck().consideredVertices
                << ",\"snapped_verts\":" << remesher.symmetrySnapStats().snappedVertices
                << ",\"max_snap\":" << remesher.symmetrySnapStats().maxSnap
                << ",\"snap_tolerance\":" << remesher.symmetrySnapStats().minSnap
                << ",\"seamVertices\":" << symmetry.seamVertices
                << ",\"clippedFaces\":" << symmetry.clippedFaces
                << ",\"mirroredFaces\":" << symmetry.mirroredFaces
                << ",\"verticesBefore\":" << symmetry.verticesBefore
                << ",\"verticesAfter\":" << symmetry.verticesAfter
                << ",\"plane\":" << std::setprecision(6) << symmetry.plane
                << ",\"tolerance\":" << symmetry.tolerance << "}"
                << ",\"normals\":" << (normals.empty() ? "false" : "true")
                << ",\"deviation\":" << deviationJson(remesher.deviationStats())
                << ",\"effective_params\":" << effectiveParamsJson(options)
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
        if (options.measureDeviation) {
            printf("  deviation: mean %.6g  p95 %.6g  max %.6g  (%zu samples)\n",
                remesher.deviationStats().mean, remesher.deviationStats().p95,
                remesher.deviationStats().max, remesher.deviationStats().samples);
        }
        printf("  uv: %s (%zu vt, %s), normals: %s\n",
            uvMode != "none" ? "yes" : "none", uvCorners, uvMode.c_str(),
            normals.empty() ? "no" : "yes");
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
