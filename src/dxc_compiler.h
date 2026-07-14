#pragma once

#include "types.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace axslcc::dxc
{

struct DxcResult
{
    std::string object;
};

// Compile in-memory HLSL source to DXIL
// profile: 60 for SM 6.0
DxcResult compile_source(std::string_view hlsl, const Options& options, const Target& target);
DxcResult compile_spirv(std::string_view hlsl, const Options& options, const Target& target);

} // namespace axslcc::dxc
