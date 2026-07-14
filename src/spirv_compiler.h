#pragma once

#include "types.h"

#include <string>
#include <string_view>
#include <vector>

namespace axslcc::spirv
{

// ============= SPIRV Compilation =============

CompileUnit compile_input(const Options& options, const Target& target);

std::string spirv_to_bytes(const std::vector<uint32_t>& spirv);

// Removes non-runtime reflection/debug extensions and validates the module
// using the statically linked SPIRV-Tools library.
std::vector<uint32_t> make_vulkan_runtime_spirv(const std::vector<uint32_t>& spirv);

bool compile_glsl_to_spirv(std::string_view source_text, ShaderStage stage,
                           std::vector<uint32_t>& spirv, int opt_level,
                           std::string& log);

} // namespace axslcc::spirv
