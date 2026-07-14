#pragma once

#include "types.h"

#include <cstdint>
#include <vector>

namespace axslcc::cross
{

// ============= Cross Compilation =============

std::string cross_compile(const Target& target, const std::vector<uint32_t>& spirv,
                          const Options& options,
                          std::vector<UniformBlockNameOverride>* uniform_block_names = nullptr);

} // namespace axslcc::cross
