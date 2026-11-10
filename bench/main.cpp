// Headless benchmark harness for the AutoRemesher core (no Qt).
// Loads an OBJ, runs one remesh, prints the phase report and result counts.

#include <AutoRemesher/AutoRemesher>
#include <AutoRemesher/Vector3>

#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

bool loadObj(const char* path,
             std::vector<AutoRemesher::Vector3>* vertices,
             std::vector<std::vector<size_t>>* triangles)
{
    FILE* fp = fopen(path, "rb");
    if (nullptr == fp)
        return false;
    fseek(fp, 0, SEEK_END);
    const long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    std::string text;
    text.resize((size_t)size);
    if (size > 0 && fread(&text[0], 1, (size_t)size, fp) != (size_t)size) {
        fclose(fp);
        return false;
    }
    fclose(fp);

    vertices->clear();
    triangles->clear();
    const char* p = text.c_str();
    const char* end = p + text.size();
    while (p < end) {
        const char* lineEnd = static_cast<const char*>(memchr(p, '\n', (size_t)(end - p)));
        if (nullptr == lineEnd)
            lineEnd = end;
        if (p + 1 < lineEnd && p[0] == 'v' && p[1] == ' ') {
            char* cursor = const_cast<char*>(p + 2);
            const double x = strtod(cursor, &cursor);
            const double y = strtod(cursor, &cursor);
            const double z = strtod(cursor, &cursor);
            vertices->push_back(AutoRemesher::Vector3(x, y, z));
        } else if (p + 1 < lineEnd && p[0] == 'f' && p[1] == ' ') {
            std::vector<size_t> face;
            char* cursor = const_cast<char*>(p + 2);
            while (cursor < lineEnd) {
                while (cursor < lineEnd && (*cursor == ' ' || *cursor == '\t'))
                    ++cursor;
                if (cursor >= lineEnd)
                    break;
                char* next = cursor;
                long index = strtol(cursor, &next, 10);
                if (next == cursor)
                    break;
                cursor = next;
                while (cursor < lineEnd && *cursor != ' ' && *cursor != '\t')
                    ++cursor;
                if (index > 0)
                    face.push_back((size_t)(index - 1));
                else if (index < 0)
                    face.push_back(vertices->size() - (size_t)(-index));
            }
            for (size_t i = 2; i < face.size(); ++i)
                triangles->push_back({ face[0], face[i - 1], face[i] });
        }
        p = lineEnd + 1;
    }
    return !vertices->empty();
}

LONG WINAPI reportCrash(EXCEPTION_POINTERS* info)
{
    fprintf(stderr, "\n*** crash: code 0x%08lx at %p ***\n",
        info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    if (!SymInitialize(process, nullptr, TRUE)) {
        fprintf(stderr, "SymInitialize failed\n");
        return EXCEPTION_EXECUTE_HANDLER;
    }
    CONTEXT context = *info->ContextRecord;
    STACKFRAME64 frame = {};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    for (int depth = 0; depth < 32; ++depth) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context,
                nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            break;
        if (0 == frame.AddrPC.Offset)
            break;
        char buffer[sizeof(SYMBOL_INFO) + 256] = {};
        SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        const char* name = SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol)
            ? symbol->Name
            : "?";
        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisplacement = 0;
        if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisplacement, &line))
            fprintf(stderr, "  #%02d %s + 0x%llx  (%s:%lu)\n", depth, name,
                (unsigned long long)displacement, line.FileName, line.LineNumber);
        else
            fprintf(stderr, "  #%02d %s + 0x%llx\n", depth, name, (unsigned long long)displacement);
    }
    SymCleanup(process);
    return EXCEPTION_EXECUTE_HANDLER;
}

void traceProgress(void* tag, float progress, const char* status)
{
    (void)tag;
    if (nullptr != status && '\0' != status[0])
        fprintf(stderr, "[trace %.3f] %s\n", progress, status);
}

struct Sample {
    double seconds = 0.0;
    size_t quads = 0;
    size_t nonQuads = 0;
    size_t vertices = 0;
};

Sample runOnce(const std::vector<AutoRemesher::Vector3>& vertices,
               const std::vector<std::vector<size_t>>& triangles,
               size_t targetTriangleCount, double scaling, double adaptivity,
               double anisotropy, double sharpEdgeDegrees, double smoothNormalDegrees,
               std::vector<std::string>* phaseReport)
{
    AutoRemesher::AutoRemesher remesher(vertices, triangles);
    remesher.setTargetTriangleCount(targetTriangleCount);
    remesher.setScaling(scaling);
    remesher.setGradientAdaptivity(adaptivity);
    remesher.setAnisotropy(anisotropy);
    remesher.setSharpEdgeDegrees(sharpEdgeDegrees);
    remesher.setSmoothNormalDegrees(smoothNormalDegrees);
    if (nullptr != getenv("AR_TRACE"))
        remesher.setProgressHandler(traceProgress);

    const auto start = std::chrono::high_resolution_clock::now();
    const bool ok = remesher.remesh();
    const auto stop = std::chrono::high_resolution_clock::now();

    Sample sample;
    sample.seconds = std::chrono::duration<double>(stop - start).count();
    if (!ok) {
        fprintf(stderr, "remesh() failed\n");
        return sample;
    }
    for (const auto& face : remesher.remeshedQuads()) {
        if (4 == face.size())
            ++sample.quads;
        else
            ++sample.nonQuads;
    }
    sample.vertices = remesher.remeshedVertices().size();
    if (nullptr != phaseReport)
        *phaseReport = remesher.phaseReport();
    return sample;
}

} // namespace

int main(int argc, char** argv)
{
    SetUnhandledExceptionFilter(reportCrash);
    const char* input = nullptr;
    size_t targetQuads = 20000;
    double scaling = 1.0;
    double adaptivity = 0.5;
    double anisotropy = 0.0;
    double sharpEdgeDegrees = 60.0;
    double smoothNormalDegrees = 0.0;
    int repeat = 1;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() { return (i + 1 < argc) ? argv[++i] : ""; };
        if (arg == "--input")
            input = value();
        else if (arg == "--target-quads")
            targetQuads = (size_t)atoll(value());
        else if (arg == "--edge-scaling")
            scaling = atof(value());
        else if (arg == "--adaptivity")
            adaptivity = atof(value());
        else if (arg == "--anisotropy")
            anisotropy = atof(value());
        else if (arg == "--sharp-edge")
            sharpEdgeDegrees = atof(value());
        else if (arg == "--smooth-normal")
            smoothNormalDegrees = atof(value());
        else if (arg == "--repeat")
            repeat = atoi(value());
        else if (arg == "--verbose")
            verbose = true;
    }
    if (nullptr == input) {
        fprintf(stderr, "usage: arbench --input model.obj --target-quads N [--repeat N]\n");
        return 1;
    }

    std::vector<AutoRemesher::Vector3> vertices;
    std::vector<std::vector<size_t>> triangles;
    if (!loadObj(input, &vertices, &triangles)) {
        fprintf(stderr, "failed to load %s\n", input);
        return 1;
    }
    printf("input: %s\n", input);
    printf("vertices: %zu  triangles: %zu  target quads: %zu  threads: %u\n",
        vertices.size(), triangles.size(), targetQuads, std::thread::hardware_concurrency());

    std::vector<double> times;
    Sample last;
    std::vector<std::string> phaseReport;
    for (int i = 0; i < repeat; ++i) {
        last = runOnce(vertices, triangles, targetQuads * 2, scaling, adaptivity,
            anisotropy, sharpEdgeDegrees, smoothNormalDegrees, &phaseReport);
        times.push_back(last.seconds);
        printf("run %d: %.3f s  quads %zu  non-quads %zu  vertices %zu\n",
            i + 1, last.seconds, last.quads, last.nonQuads, last.vertices);
        fflush(stdout);
    }

    std::sort(times.begin(), times.end());
    printf("best: %.3f s  median: %.3f s\n", times.front(), times[times.size() / 2]);

    PROCESS_MEMORY_COUNTERS counters;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        printf("peak working set: %.1f MB\n", counters.PeakWorkingSetSize / (1024.0 * 1024.0));

    if (verbose) {
        for (const auto& line : phaseReport)
            printf("%s\n", line.c_str());
    }
    return 0;
}
