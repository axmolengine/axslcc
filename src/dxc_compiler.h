#pragma once

#ifdef _WIN32

#include "types.h"

#include <vector>
#include <cstdint>

struct ID3D12ShaderReflection;

namespace axslcc::dxc
{

struct DxcResult
{
    tlx::byte_buffer dxil;
    tlx::byte_buffer refl;
};

// Compile in-memory HLSL source to DXIL
// profile: 60 for SM 6.0
DxcResult compile_source(const std::string& hlsl, ShaderStage stage,
                          const std::vector<fs::path>& includeDirs,
                          const std::vector<std::string>& defines,
                          int profile = 60,
                          struct ID3D12ShaderReflection** outReflection = nullptr,
                          const fs::path& sourceName = {});

tlx::byte_buffer build_reflection(struct ID3D12ShaderReflection* shaderRefl, ShaderStage stage,
                                       const fs::path& input);

} // namespace axslcc::dxc

#endif // _WIN32
