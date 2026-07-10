#ifdef _WIN32

#include "dxc_compiler.h"

#include <windows.h>
#include <dxcapi.h>
#include <d3d12shader.h>

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
    case ShaderStage::Vertex:   return sm == 60 ? L"vs_6_0" : L"vs_6_0";
    case ShaderStage::Fragment: return sm == 60 ? L"ps_6_0" : L"ps_6_0";
    case ShaderStage::Compute:  return sm == 60 ? L"cs_6_0" : L"cs_6_0";
    default:                    return L"vs_6_0";
    }
}

} // namespace

DxcResult compile_source(const std::string& hlsl, ShaderStage stage,
                          const std::vector<fs::path>& includeDirs,
                          const std::vector<std::string>& defines,
                          int profile,
                          ID3D12ShaderReflection** outReflection,
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

    CComPtr<IDxcBlob> reflBlob;
    CComQIPtr<IDxcResult> dxcResult(opResult);
    if (dxcResult)
        dxcResult->GetOutput(DXC_OUT_REFLECTION, IID_PPV_ARGS(&reflBlob), nullptr);
    if (reflBlob)
        out.refl.assign((uint8_t*)reflBlob->GetBufferPointer(),
                         (uint8_t*)reflBlob->GetBufferPointer() + reflBlob->GetBufferSize());

    if (outReflection && reflBlob)
    {
        CComPtr<IDxcUtils> utils;
        DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils));

        DxcBuffer buf{ reflBlob->GetBufferPointer(), reflBlob->GetBufferSize(), 0 };
        utils->CreateReflection(&buf, IID_ID3D12ShaderReflection, (void**)outReflection);
    }

    return out;
}

} // namespace axslcc::dxc

#endif // _WIN32
