#ifdef _WIN32

#include "dxc_compiler.h"
#include "utils.h"

#include <windows.h>
#include <dxcapi.h>

#include <atlbase.h>
#include <stdexcept>

namespace axslcc::dxc
{

namespace
{

const wchar_t* profile_for_stage(ShaderStage stage)
{
    switch (stage)
    {
    case ShaderStage::Vertex:   return L"vs_6_0";
    case ShaderStage::Fragment: return L"ps_6_0";
    case ShaderStage::Compute:  return L"cs_6_0";
    default:                    return L"vs_6_0";
    }
}

} // namespace

DxcResult compile_hlsl(const Options& options)
{
    CComPtr<IDxcLibrary> library;
    CComPtr<IDxcCompiler> compiler;
    CComPtr<IDxcIncludeHandler> includeHandler;

    if (FAILED(DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&library))))
        throw std::runtime_error("Failed to create DXC library instance");

    if (FAILED(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))))
        throw std::runtime_error("Failed to create DXC compiler instance");

    library->CreateIncludeHandler(&includeHandler);

    std::string sourceText = utils::read_text_file(options.input);
    CComPtr<IDxcBlobEncoding> sourceBlob;
    if (FAILED(library->CreateBlobWithEncodingOnHeapCopy(
            sourceText.data(), static_cast<UINT32>(sourceText.size()), CP_UTF8, &sourceBlob)))
        throw std::runtime_error("Failed to create DXC source blob");

    auto stage = utils::stage_from_name(options.input);
    if (!stage)
        throw std::runtime_error("Cannot determine shader stage from filename");

    // Build DXC arguments (include dirs, defines)
    std::vector<std::wstring> argStorage;
    std::vector<LPCWSTR> args;

    for (const auto& inc : options.include_dirs)
    {
        argStorage.push_back(L"-I");
        argStorage.push_back(inc.wstring());
    }
    for (const auto& arg : argStorage)
        args.push_back(arg.c_str());

    CComPtr<IDxcOperationResult> opResult;
    HRESULT hr = compiler->Compile(
        sourceBlob, options.input.wstring().c_str(), L"main", profile_for_stage(*stage),
        args.data(), static_cast<UINT>(args.size()), nullptr, 0, includeHandler, &opResult);

    if (FAILED(hr))
        throw std::runtime_error("DXC Compile() call failed");

    HRESULT status;
    opResult->GetStatus(&status);
    if (FAILED(status))
    {
        CComPtr<IDxcBlobEncoding> errors;
        opResult->GetErrorBuffer(&errors);
        std::string msg = "DXC compile error: ";
        if (errors)
            msg.append((const char*)errors->GetBufferPointer(), errors->GetBufferSize());
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
