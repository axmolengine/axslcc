#pragma once

#include "types.h"

#include <cstdint>
#include <vector>

namespace axslcc::cross
{

// ============= Cross Compilation =============

OutputBlob cross_compile(const Target& target, const std::vector<uint32_t>& spirv,
                         const fs::path& input = {});

} // namespace axslcc::cross
