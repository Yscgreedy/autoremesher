#include <AutoRemesher/Csr>

#include <numeric>

namespace AutoRemesher {

void Csr::reset(const std::vector<size_t>& rowSizes)
{
    m_offsets.assign(rowSizes.size() + 1, 0);
    for (size_t row = 0; row < rowSizes.size(); ++row)
        m_offsets[row + 1] = m_offsets[row] + rowSizes[row];
    m_indices.assign(m_offsets.back(), 0);
}

}
