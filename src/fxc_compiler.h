#pragma once

#ifdef _WIN32

#include "types.h"

#include <cstdint>
#include <string_view>
#include <vector>

struct ID3D11ShaderReflection;

namespace axslcc::fxc
{

struct FxcResult
{
    std::string dxbc;
};

// Compile in-memory HLSL source to DXBC (SM 5.0 / 5.1)
// profile: 50 for SM 5.0, 51 for SM 5.1
FxcResult compile_hlsl(std::string_view hlsl, const Options& options, const Target& target);

} // namespace axslcc::fxc

#endif // _WIN32
