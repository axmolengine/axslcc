#pragma once

#ifdef _WIN32

#include "types.h"

#include <vector>
#include <cstdint>

struct ID3D11ShaderReflection;

namespace axslcc::fxc
{

struct FxcResult
{
    tlx::byte_buffer dxbc;
};

// Compile in-memory HLSL source to DXBC (SM 5.0 / 5.1)
// profile: 50 for SM 5.0, 51 for SM 5.1
FxcResult compile_hlsl(const std::string& hlsl, ShaderStage stage,
                        const std::vector<fs::path>& includeDirs,
                        const std::vector<std::string>& defines,
                        int profile,
                        const fs::path& sourceName = {});

} // namespace axslcc::fxc

#endif // _WIN32
