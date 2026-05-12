#pragma once

#include "types.h"

#include <filesystem>
#include <cstdint>
#include <vector>

namespace fs = std::filesystem;

namespace axslcc::reflection
{

// ============= Reflection Data Generation =============

std::vector<uint8_t> build_reflection(const Target& target, const std::vector<uint32_t>& spirv,
    ShaderStage stage, const fs::path& input);

} // namespace axslcc::reflection
