#pragma once

#include "types.h"

#include <string>
#include <string_view>
#include <vector>

namespace axslcc::spirv
{

// ============= SPIRV Compilation =============

CompileUnit compile_input(const Options& options, const Target& target);

tlx::byte_buffer spirv_to_bytes(const std::vector<uint32_t>& spirv);

} // namespace axslcc::spirv
