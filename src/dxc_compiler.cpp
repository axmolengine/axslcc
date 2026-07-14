#include "dxc_compiler.h"

#ifdef _WIN32
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif
#include "dxc/dxcapi.h"

#include <algorithm>
#include <climits>
#include <fmt/format.h>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace axslcc::dxc
{
// minimal ComPtr matching WRL's subset interface, avoids pulling in WRL headers
template <class T>
class ComPtr
{
public:
    ComPtr() = default;
    ~ComPtr() noexcept { InternalRelease(); }
    ComPtr(const ComPtr&)            = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& rhs) noexcept : ptr_(std::exchange(rhs.ptr_, nullptr)) {}
    ComPtr& operator=(ComPtr&& rhs) noexcept
    {
        if (this != &rhs)
        {
            InternalRelease();
            ptr_ = std::exchange(rhs.ptr_, nullptr);
        }
        return *this;
    }
    T* Get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }
    T& operator*() const noexcept { return *ptr_; }
    T** GetAddressOf() noexcept { return &ptr_; }
    T** ReleaseAndGetAddressOf() noexcept
    {
        InternalRelease();
        return &ptr_;
    }
    void Reset() noexcept { InternalRelease(); }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

private:
    void InternalRelease() noexcept
    {
        T* temp = ptr_;
        if (temp != nullptr)
        {
            ptr_ = nullptr;
            temp->Release();
        }
    }
    T* ptr_{nullptr};
};

namespace
{
template <class T>
void create_instance(REFCLSID clsid, ComPtr<T>& out, const char* what)
{
    if (FAILED(DxcCreateInstance(clsid, __uuidof(T), reinterpret_cast<void**>(out.GetAddressOf()))))
        throw std::runtime_error(fmt::format("failed to create DXC {}", what));
}

std::wstring widen_utf8(std::string_view value)
{
    // DXC command-line switches and repository paths are UTF-8. DXC's Unix API
    // uses wchar_t as well, so perform a small platform-independent conversion.
    std::wstring result;
    result.reserve(value.size());
    size_t i = 0;
    while (i < value.size())
    {
        uint32_t cp = static_cast<unsigned char>(value[i++]);
        if ((cp & 0x80u) == 0)
        {
        }
        else if ((cp & 0xe0u) == 0xc0u && i < value.size())
        {
            cp = ((cp & 0x1fu) << 6) | (static_cast<unsigned char>(value[i++]) & 0x3fu);
        }
        else if ((cp & 0xf0u) == 0xe0u && i + 1 < value.size())
        {
            cp = ((cp & 0x0fu) << 12) | ((static_cast<unsigned char>(value[i++]) & 0x3fu) << 6);
            cp |= static_cast<unsigned char>(value[i++]) & 0x3fu;
        }
        else if ((cp & 0xf8u) == 0xf0u && i + 2 < value.size())
        {
            cp = ((cp & 0x07u) << 18) | ((static_cast<unsigned char>(value[i++]) & 0x3fu) << 12);
            cp |= (static_cast<unsigned char>(value[i++]) & 0x3fu) << 6;
            cp |= static_cast<unsigned char>(value[i++]) & 0x3fu;
        }
        else
        {
            cp = 0xfffdu;
        }
#if WCHAR_MAX <= 0xffff
        if (cp > 0xffffu)
        {
            cp -= 0x10000u;
            result.push_back(static_cast<wchar_t>(0xd800u + (cp >> 10)));
            result.push_back(static_cast<wchar_t>(0xdc00u + (cp & 0x3ffu)));
        }
        else
#endif
            result.push_back(static_cast<wchar_t>(cp));
    }
    return result;
}

std::string path_utf8(const fs::path& path)
{
    auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

const wchar_t* profile_for_stage(ShaderStage stage, bool spirv, int sm)
{
    // DXC promotes pre-SM6 profiles. Use SM6 for SPIR-V while accepting the
    // project's HLSL 5.1 source language; DXBC remains an FXC responsibility.
    if (spirv || sm >= 60)
    {
        switch (stage)
        {
        case ShaderStage::Vertex:
            return L"vs_6_0";
        case ShaderStage::Fragment:
            return L"ps_6_0";
        case ShaderStage::Compute:
            return L"cs_6_0";
        }
    }
    switch (stage)
    {
    case ShaderStage::Vertex:
        return sm <= 50 ? L"vs_5_0" : L"vs_5_1";
    case ShaderStage::Fragment:
        return sm <= 50 ? L"ps_5_0" : L"ps_5_1";
    case ShaderStage::Compute:
        return sm <= 50 ? L"cs_5_0" : L"cs_5_1";
    }
    return L"vs_6_0";
}

DxcResult compile_impl(std::string_view hlsl,
                       const Options& options,
                       const Target& target,
                       bool spirv,
                       bool separateSamplerBindings)
{
    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    ComPtr<IDxcIncludeHandler> includeHandler;
    create_instance(CLSID_DxcUtils, utils, "utilities");
    create_instance(CLSID_DxcCompiler, compiler, "compiler");
    if (FAILED(utils->CreateDefaultIncludeHandler(includeHandler.GetAddressOf())))
        throw std::runtime_error("failed to create DXC include handler");

    DxcBuffer source{};
    source.Ptr      = hlsl.data();
    source.Size     = hlsl.size();
    source.Encoding = DXC_CP_UTF8;

    std::vector<std::wstring> storage;
    storage.reserve(16 + options.include_dirs.size() * 2 + target.defines.size() * 2);
    auto push = [&](std::wstring value) { storage.emplace_back(std::move(value)); };

    push(L"-E");
    push(L"main");
    push(L"-T");
    push(profile_for_stage(options.stage, spirv, spirv ? 60 : target.profile));
    push(L"-HV");
    push(L"2021");
    push(L"-Ges");
    if (spirv)
    {
        push(L"-spirv");
        push(L"-fspv-target-env=vulkan1.1");
        // Keep HLSL semantics in the in-memory intermediate. Runtime Vulkan
        // modules strip this metadata after reflection/cross-compilation.
        push(L"-fspv-reflect");
        if (separateSamplerBindings)
        {
            // HLSL register classes share Vulkan's binding namespace. Keep s#
            // disjoint from t# without changing the source-level registers.
            push(L"-fvk-s-shift");
            push(L"1024");
            push(L"all");
        }
    }
    switch (options.opt_level)
    {
    case 0:
        push(L"-Od");
        break;
    case 1:
        push(L"-O1");
        break;
    case 2:
        push(L"-O2");
        break;
    default:
        push(L"-O3");
        break;
    }

    std::vector<fs::path> searchDirs;
    if (!options.input.empty() && !options.input.parent_path().empty())
        searchDirs.push_back(options.input.parent_path());
    searchDirs.insert(searchDirs.end(), options.include_dirs.begin(), options.include_dirs.end());
    for (const auto& dir : searchDirs)
    {
        push(L"-I");
        push(widen_utf8(path_utf8(dir)));
    }
    for (const auto& define : target.defines)
    {
        push(L"-D");
        push(widen_utf8(define));
    }
    if (!options.input.empty())
        push(widen_utf8(path_utf8(options.input)));

    std::vector<LPCWSTR> args;
    args.reserve(storage.size());
    for (const auto& item : storage)
        args.push_back(item.c_str());

    ComPtr<IDxcResult> result;
    HRESULT hr = compiler->Compile(&source, args.data(), static_cast<uint32_t>(args.size()), includeHandler.Get(),
                                   __uuidof(IDxcResult), reinterpret_cast<void**>(result.GetAddressOf()));
    if (FAILED(hr) || !result)
        throw std::runtime_error("DXC Compile() call failed");

    ComPtr<IDxcBlobUtf8> errors;
    result->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8), reinterpret_cast<void**>(errors.GetAddressOf()), nullptr);

    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    if (FAILED(status))
    {
        std::string message = "DXC compilation failed";
        if (errors && errors->GetStringLength())
            message.assign(errors->GetStringPointer(), errors->GetStringLength());
        throw std::runtime_error(message);
    }

    ComPtr<IDxcBlob> object;
    if (FAILED(
            result->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob), reinterpret_cast<void**>(object.GetAddressOf()), nullptr)) ||
        !object)
        throw std::runtime_error("DXC did not produce an output object");

    auto* begin = static_cast<const char*>(object->GetBufferPointer());
    DxcResult output;
    output.object.assign(begin, object->GetBufferSize());
    return output;
}

}  // namespace

DxcResult compile_source(std::string_view hlsl,
                         const Options& options,
                         const Target& target)
{
    return compile_impl(hlsl, options, target, false, false);
}

DxcResult compile_spirv(std::string_view hlsl,
                        const Options& options,
                        const Target& target)
{
    bool separateSamplerBindings = target.lang == axslc::SHADER_LANG_SPIRV &&
        options.vulkan_sampler_mode == VulkanSamplerMode::Separate;
    return compile_impl(hlsl, options, target, true, separateSamplerBindings);
}

}  // namespace axslcc::dxc
