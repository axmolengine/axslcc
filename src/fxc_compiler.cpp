#ifdef _WIN32

#include "fxc_compiler.h"

#include <windows.h>
#include <d3dcompiler.h>

#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <vector>

namespace axslcc::fxc
{

namespace
{

class DirIncludeHandler : public ID3DInclude
{
    std::vector<fs::path> _dirs;
    fs::path _sourceDir;
public:
    DirIncludeHandler(const std::vector<fs::path>& dirs, const fs::path& sourcePath)
        : _dirs(dirs)
    {
        if (!sourcePath.empty())
            _dirs.push_back(sourcePath.parent_path());
    }

    HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE /*includeType*/, LPCSTR pFileName,
                                    LPCVOID /*pParentData*/, LPCVOID* ppData, UINT* pBytes) override
    {
        for (const auto& dir : _dirs)
        {
            fs::path full = dir / pFileName;
            std::ifstream f(full, std::ios::binary | std::ios::ate);
            if (!f)
                continue;
            auto size = f.tellg();
            f.seekg(0, std::ios::beg);
            char* buf = (char*)malloc(static_cast<size_t>(size));
            f.read(buf, size);
            *ppData = buf;
            *pBytes = static_cast<UINT>(size);
            return S_OK;
        }
        return E_FAIL;
    }

    HRESULT STDMETHODCALLTYPE Close(LPCVOID pData) override
    {
        free((void*)pData);
        return S_OK;
    }
};

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

} // namespace

FxcResult compile_hlsl(const std::string& hlsl, ShaderStage stage,
                        const std::vector<fs::path>& includeDirs,
                        const std::vector<std::string>& defines,
                        int profile, int opt_level,
                        const fs::path& sourceName)
{
    std::vector<D3D_SHADER_MACRO> macros;
    for (const auto& def : defines)
    {
        auto eq = def.find('=');
        std::string name = def.substr(0, eq);
        std::string value = (eq != std::string::npos) ? def.substr(eq + 1) : "1";
        macros.push_back({_strdup(name.c_str()), _strdup(value.c_str())});
    }
    macros.push_back({nullptr, nullptr});

    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
    switch (opt_level) {
    case 0: flags |= D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG; break;
    case 1: flags |= D3DCOMPILE_OPTIMIZATION_LEVEL1; break;
    case 2: flags |= D3DCOMPILE_OPTIMIZATION_LEVEL2; break;
    case 3: flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3; break;
    }

    DirIncludeHandler includeHandler(includeDirs, sourceName);

    ID3DBlob* blob = nullptr;
    ID3DBlob* errorBlob = nullptr;

    std::string srcName = sourceName.empty() ? "shader.hlsl" : sourceName.string();

    HRESULT hr = D3DCompile(
        hlsl.data(), hlsl.size(),
        srcName.c_str(),
        macros.data(),
        &includeHandler,
        "main",
        profile_for_stage(stage, profile),
        flags, 0,
        &blob, &errorBlob);

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
