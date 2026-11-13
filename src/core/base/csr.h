/*
 *  Copyright (c) 2026 Jeremy HU <jeremy-at-dust3d dot org>. All rights reserved.
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
#ifndef AUTO_REMESHER_CORE_BASE_CSR_H
#define AUTO_REMESHER_CORE_BASE_CSR_H

#include <cstddef>
#include <vector>

namespace AutoRemesher {

// Compressed sparse row adjacency.  The pipeline used to store one
// std::vector per vertex, which costs an allocation per row and scatters the
// neighbours across the heap; the flat form keeps the same iteration order
// (rows ascending, and inside a row the insertion order the builder used) while
// costing two contiguous arrays.
class CsrView {
public:
    using value_type = size_t;
    using const_iterator = const size_t*;

    CsrView() = default;
    CsrView(const size_t* first, const size_t* last)
        : m_first(first)
        , m_last(last)
    {
    }

    const_iterator begin() const { return m_first; }
    const_iterator end() const { return m_last; }
    size_t size() const { return (size_t)(m_last - m_first); }
    bool empty() const { return m_first == m_last; }
    const size_t& operator[](size_t index) const { return m_first[index]; }
    const size_t& front() const { return *m_first; }
    const size_t& back() const { return *(m_last - 1); }
    const size_t* data() const { return m_first; }

private:
    const size_t* m_first = nullptr;
    const size_t* m_last = nullptr;
};

class Csr {
public:
    // Allocates the flat array from the per row counts and builds the offsets.
    void reset(const std::vector<size_t>& rowSizes);

    size_t rowCount() const { return m_offsets.empty() ? 0 : m_offsets.size() - 1; }
    CsrView row(size_t index) const { return CsrView(m_indices.data() + m_offsets[index], m_indices.data() + m_offsets[index + 1]); }

    // Write cursor for a row; callers fill rows in ascending row order so the
    // resulting inside-row order matches the push_back order of the old form.
    size_t* rowStart(size_t index) { return m_indices.data() + m_offsets[index]; }
    void set(size_t row, size_t column, size_t value) { m_indices[m_offsets[row] + column] = value; }

    const std::vector<size_t>& offsets() const { return m_offsets; }
    const std::vector<size_t>& indices() const { return m_indices; }

private:
    std::vector<size_t> m_offsets;
    std::vector<size_t> m_indices;
};

}

#endif
