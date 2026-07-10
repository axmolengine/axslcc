#ifdef _WIN32

#include "fxc_compiler.h"

#include <windows.h>
#include <d3dcompiler.h>

#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace axslcc::fxc
{

static const char* profile_for_stage(ShaderStage stage, int sm)
{
    switch (stage)
    {
    case ShaderStage::Vertex:
        return sm == 50 ? "vs_5_0" : "vs_5_1";
    case ShaderStage::Fragment:
        return sm == 50 ? "ps_5_0" : "ps_5_1";
    case ShaderStage::Compute:
        return sm == 50 ? "cs_5_0" : "cs_5_1";
    default:
        return sm == 50 ? "vs_5_0" : "vs_5_1";
    }
}

FxcResult compile_hlsl(const std::string& hlsl, ShaderStage stage,
                        const std::vector<fs::path>& includeDirs,
                        const std::vector<std::string>& defines,
                        int profile,
                        const fs::path& sourceName)
{
    // Build D3D_SHADER_MACRO array
    std::vector<D3D_SHADER_MACRO> macros;
    for (const auto& def : defines)
    {
        auto eq = def.find('=');
        std::string name = def.substr(0, eq);
        std::string value = (eq != std::string::npos) ? def.substr(eq + 1) : "1";
        macros.push_back({_strdup(name.c_str()), _strdup(value.c_str())});
    }
    macros.push_back({nullptr, nullptr});

    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL2 | D3DCOMPILE_ENABLE_STRICTNESS;
#if !defined(NDEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ID3DBlob* blob = nullptr;
    ID3DBlob* errorBlob = nullptr;

    std::string srcName = sourceName.empty() ? "shader.hlsl" : sourceName.string();

    HRESULT hr = D3DCompile(
        hlsl.data(), hlsl.size(),
        srcName.c_str(),
        macros.data(),
        nullptr,        // D3D_COMPILE_STANDARD_FILE_INCLUDE
        "main",
        profile_for_stage(stage, profile),
        flags, 0,
        &blob, &errorBlob);

    // Free macro strings
    for (auto& m : macros)
    {
        if (m.Name) free((void*)m.Name);
        if (m.Definition) free((void*)m.Definition);
    }

    if (FAILED(hr))
    {
        std::string errMsg;
        if (errorBlob)
            errMsg.assign((const char*)errorBlob->GetBufferPointer(), errorBlob->GetBufferSize());
        if (errorBlob) errorBlob->Release();
        throw std::runtime_error(errMsg.empty() ? "FXC compile failed" : errMsg);
    }

    if (errorBlob)
        errorBlob->Release();

    FxcResult out;
    out.dxbc.assign((uint8_t*)blob->GetBufferPointer(),
                     (uint8_t*)blob->GetBufferPointer() + blob->GetBufferSize());
    blob->Release();

    return out;
}

} // namespace axslcc::fxc

#endif // _WIN32
