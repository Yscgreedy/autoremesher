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
#ifndef AUTO_REMESHER_LOGGING_H
#define AUTO_REMESHER_LOGGING_H
#include <string>

namespace AutoRemesher {

// The core library writes no diagnostics of its own: the application installs a
// sink and picks a level, and everything the library has to say (phase report,
// per-hole notes, debug traces) goes through Log::write().  A library used
// without a sink stays silent instead of spraying a host's stderr, which is what
// the layering gate R4 asks for.
namespace Log {

enum Level {
    Silent = 0,
    Normal = 1,
    Debug = 2,
};

// One function per message; level tells the sink how loud the line is.
typedef void (*Sink)(int level, const char* message);

inline Sink& sink()
{
    static Sink value = nullptr;
    return value;
}

inline int& level()
{
    static int value = Normal;
    return value;
}

inline void setSink(Sink value)
{
    sink() = value;
}

inline void setLevel(int value)
{
    level() = value;
}

inline bool enabled(int neededLevel)
{
    return nullptr != sink() && level() >= neededLevel;
}

inline void write(int neededLevel, const std::string& message)
{
    if (enabled(neededLevel))
        sink()(neededLevel, message.c_str());
}

} // namespace Log

}

#endif
