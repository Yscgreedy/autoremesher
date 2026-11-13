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
#include <AutoRemesher/PositionKey>

namespace AutoRemesher {

thread_local double PositionKey::m_tolerance = 1e-5;

void PositionKey::setTolerance(double tolerance)
{
    m_tolerance = tolerance > 0.0 ? tolerance : 1e-5;
}

double PositionKey::tolerance()
{
    return m_tolerance;
}

// Truncation towards zero matches the historical (long)(x * 1e5) mapping, so a
// unit-scale mesh keeps its exact keys.  Values saturate instead of wrapping: a
// coordinate beyond 9e15 steps is meaningless as a key, and the previous 32-bit
// long silently overflowed on Windows for anything past 21474 units.
long long PositionKey::quantize(double value)
{
    const double scaled = value / m_tolerance;
    if (scaled >= 9.0e15)
        return 9000000000000000LL;
    if (scaled <= -9.0e15)
        return -9000000000000000LL;
    return (long long)scaled;
}

PositionKey::PositionKey(const Vector3& v)
    : PositionKey(v.x(), v.y(), v.z())
{
}

PositionKey::PositionKey(double x, double y, double z)
{
    m_position.setX(x);
    m_position.setY(y);
    m_position.setZ(z);

    m_intX = quantize(x);
    m_intY = quantize(y);
    m_intZ = quantize(z);
}

const Vector3& PositionKey::position() const
{
    return m_position;
}

bool PositionKey::operator<(const PositionKey& right) const
{
    if (m_intX < right.m_intX)
        return true;
    if (m_intX > right.m_intX)
        return false;
    if (m_intY < right.m_intY)
        return true;
    if (m_intY > right.m_intY)
        return false;
    if (m_intZ < right.m_intZ)
        return true;
    if (m_intZ > right.m_intZ)
        return false;
    return false;
}

bool PositionKey::operator==(const PositionKey& right) const
{
    return m_intX == right.m_intX && m_intY == right.m_intY && m_intZ == right.m_intZ;
}

}
