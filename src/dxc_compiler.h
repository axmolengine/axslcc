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
    std::vector<uint8_t> refl;  // DXC_OUT_REFLECTION container blob
};

DxcResult compile_hlsl(const Options& options);

std::vector<uint8_t> build_reflection(const DxcResult& result, ShaderStage stage,
                                       const fs::path& input);

} // namespace axslcc::dxc

#endif // _WIN32
