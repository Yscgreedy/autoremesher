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
#ifndef AUTO_REMESHER_POSITION_KEY_H
#define AUTO_REMESHER_POSITION_KEY_H
#include <AutoRemesher/Vector3>
#include <cstddef>
#include <cstdint>

namespace AutoRemesher {

class PositionKey {
public:
    PositionKey(const Vector3& v);
    PositionKey(double x, double y, double z);
    const Vector3& position() const;
    bool operator<(const PositionKey& right) const;
    bool operator==(const PositionKey& right) const;
    long long intX() const { return m_intX; }
    long long intY() const { return m_intY; }
    long long intZ() const { return m_intZ; }

    // Quantisation step used to collapse positions that differ only by floating
    // point noise.  The historical behaviour is an absolute 1e-5, which silently
    // becomes coarser than the mesh itself once the model is modelled in units
    // far from 1 (a 1e-3 model would weld its surface into a handful of points).
    // Callers with an arbitrary model scale set a step relative to their own
    // voxel size; QuadExtractor does exactly that.  The value is per thread
    // because mesh islands are extracted in parallel.
    static void setTolerance(double tolerance);
    static double tolerance();

private:
    long long m_intX = 0;
    long long m_intY = 0;
    long long m_intZ = 0;
    Vector3 m_position;

    static long long quantize(double value);
    static thread_local double m_tolerance;
};

// The quantized integers are the whole identity of a key, so hashing them is
// exact and lets the extractor use hash maps on its hottest lookup path.
struct PositionKeyHash {
    size_t operator()(const PositionKey& key) const
    {
        uint64_t hash = 1469598103934665603ull;
        const long long values[3] = { key.intX(), key.intY(), key.intZ() };
        for (int i = 0; i < 3; ++i) {
            hash ^= (uint64_t)(values[i] + 0x9e3779b97f4a7c15ull);
            hash *= 1099511628211ull;
            hash ^= hash >> 29;
        }
        return (size_t)hash;
    }
};

}

#endif
