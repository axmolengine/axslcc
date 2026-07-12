#pragma once

#ifdef _WIN32

#include "types.h"

#include <vector>
#include <cstdint>

namespace axslcc::dxc
{

struct DxcResult
{
    tlx::byte_buffer dxil;
};

// Compile in-memory HLSL source to DXIL
// profile: 60 for SM 6.0
DxcResult compile_source(const std::string& hlsl, ShaderStage stage,
                          const std::vector<fs::path>& includeDirs,
                          const std::vector<std::string>& defines,
                          int profile, int opt_level,
                          const fs::path& sourceName = {});

} // namespace axslcc::dxc

#endif // _WIN32
