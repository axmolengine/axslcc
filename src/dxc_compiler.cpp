#ifdef _WIN32

#include "dxc_compiler.h"

#include <windows.h>
#include <dxcapi.h>

#include <atlbase.h>
#include <stdexcept>

namespace axslcc::dxc
{

namespace
{

const wchar_t* profile_for_stage(ShaderStage stage, int sm)
{
    switch (stage)
    {
    case ShaderStage::Vertex:
        if (sm == 50) return L"vs_5_0";
        if (sm == 51) return L"vs_5_1";
        return L"vs_6_0";
    case ShaderStage::Fragment:
        if (sm == 50) return L"ps_5_0";
        if (sm == 51) return L"ps_5_1";
        return L"ps_6_0";
    case ShaderStage::Compute:
        if (sm == 50) return L"cs_5_0";
        if (sm == 51) return L"cs_5_1";
        return L"cs_6_0";
    default:
        return L"vs_6_0";
    }
}

} // namespace

DxcResult compile_source(const std::string& hlsl, ShaderStage stage,
                          const std::vector<fs::path>& includeDirs,
                          const std::vector<std::string>& defines,
                          int profile, int opt_level,
                          const fs::path& sourceName)
{
    CComPtr<IDxcLibrary> library;
    CComPtr<IDxcCompiler> compiler;
    CComPtr<IDxcIncludeHandler> includeHandler;

    if (FAILED(DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&library))))
        throw std::runtime_error("Failed to create DXC library instance");

    if (FAILED(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))))
        throw std::runtime_error("Failed to create DXC compiler instance");

    library->CreateIncludeHandler(&includeHandler);

    CComPtr<IDxcBlobEncoding> sourceBlob;
    if (FAILED(library->CreateBlobWithEncodingOnHeapCopy(
            hlsl.data(), static_cast<UINT32>(hlsl.size()), CP_UTF8, &sourceBlob)))
        throw std::runtime_error("Failed to create DXC source blob");

    std::wstring sourceNameW = sourceName.wstring();
    LPCWSTR sourceNamePtr = sourceName.empty() ? nullptr : sourceNameW.c_str();

    std::vector<std::wstring> argStorage;
    std::vector<LPCWSTR> args;

    // Optimization level
    switch (opt_level) {
    case 0: argStorage.push_back(L"/Od"); break;
    case 1: argStorage.push_back(L"/O1"); break;
    case 2: argStorage.push_back(L"/O2"); break;
    case 3: argStorage.push_back(L"/O3"); break;
    }

    for (const auto& inc : includeDirs)
    {
        argStorage.push_back(L"-I");
        argStorage.push_back(inc.wstring());
    }
    for (const auto& def : defines)
    {
        argStorage.push_back(L"-D");
        argStorage.push_back(std::wstring(def.begin(), def.end()));
    }
    for (auto& a : argStorage)
        args.push_back(a.c_str());

    CComPtr<IDxcOperationResult> opResult;
    HRESULT hr = compiler->Compile(
        sourceBlob, sourceNamePtr, L"main", profile_for_stage(stage, profile),
        args.data(), static_cast<UINT>(args.size()), nullptr, 0, includeHandler, &opResult);

    if (FAILED(hr))
        throw std::runtime_error("DXC Compile() call failed");

    HRESULT status;
    opResult->GetStatus(&status);
    if (FAILED(status))
    {
        CComPtr<IDxcBlobEncoding> errors;
        opResult->GetErrorBuffer(&errors);
        std::string msg;
        if (errors)
            msg.assign((const char*)errors->GetBufferPointer(), errors->GetBufferSize());
        throw std::runtime_error(msg);
    }

    CComPtr<IDxcBlob> dxilBlob;
    opResult->GetResult(&dxilBlob);

    DxcResult out;
    out.dxil.assign((uint8_t*)dxilBlob->GetBufferPointer(),
                     (uint8_t*)dxilBlob->GetBufferPointer() + dxilBlob->GetBufferSize());

    return out;
}

} // namespace axslcc::dxc

#endif // _WIN32
