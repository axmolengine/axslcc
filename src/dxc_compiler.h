#pragma once

#ifdef _WIN32

#include "types.h"

#include <vector>
#include <cstdint>

namespace axslcc::dxc
{

struct DxcResult
{
    std::vector<uint8_t> dxil;
    std::vector<uint8_t> refl;
};

// Compile original .hlsl file to DXIL
DxcResult compile_hlsl(const Options& options);

// Compile in-memory HLSL source to DXIL (for SPIRV-Cross output)
DxcResult compile_source(const std::string& hlsl, ShaderStage stage,
                          const std::vector<fs::path>& includeDirs,
                          const std::vector<std::string>& defines);

std::vector<uint8_t> build_reflection(const DxcResult& result, ShaderStage stage,
                                       const fs::path& input);

} // namespace axslcc::dxc

#endif // _WIN32
