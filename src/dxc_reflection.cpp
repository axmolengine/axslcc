#ifdef _WIN32

#include "dxc_compiler.h"
#include "axslc-spec.h"
#include "yasio/obstream.hpp"

#include <combaseapi.h>
#include <dxcapi.h>
#include <d3d12shader.h>
#include <atlbase.h>

#include <cstdint>
#include <cstring>
#include <vector>
#include <stdexcept>
#include <algorithm>

namespace axslcc::dxc
{

namespace
{

void copy_name(char* dst, size_t size, const std::string& name)
{
    std::memset(dst, 0, size);
    std::strncpy(dst, name.c_str(), size - 1);
}

uint16_t resolve_sc_type(D3D_REGISTER_COMPONENT_TYPE compType, BYTE mask)
{
    int count = 0;
    if (mask & 1) ++count;
    if (mask & 2) ++count;
    if (mask & 4) ++count;
    if (mask & 8) ++count;

    switch (compType)
    {
    case D3D_REGISTER_COMPONENT_FLOAT32:
        switch (count) {
        case 1: return axslc::SC_TYPE_FLOAT;
        case 2: return axslc::SC_TYPE_FLOAT2;
        case 3: return axslc::SC_TYPE_FLOAT3;
        case 4: return axslc::SC_TYPE_FLOAT4;
        default: return axslc::SC_TYPE_FLOAT4;
        }
    case D3D_REGISTER_COMPONENT_SINT32:
        switch (count) {
        case 1: return axslc::SC_TYPE_INT;
        case 2: return axslc::SC_TYPE_INT2;
        case 3: return axslc::SC_TYPE_INT3;
        case 4: return axslc::SC_TYPE_INT4;
        default: return axslc::SC_TYPE_INT4;
        }
    case D3D_REGISTER_COMPONENT_UINT32:
        switch (count) {
        case 1: return axslc::SC_TYPE_INT;
        case 2: return axslc::SC_TYPE_INT2;
        case 4: return axslc::SC_TYPE_INT4;
        default: return axslc::SC_TYPE_INT4;
        }
    default:
        return axslc::SC_TYPE_FLOAT4;
    }
}

// Extract ID3D12ShaderReflection from a DXIL container blob
HRESULT extractShaderReflection(const std::vector<uint8_t>& dxil, CComPtr<ID3D12ShaderReflection>& outRefl)
{
    if (dxil.empty())
        return E_FAIL;

    CComPtr<IDxcLibrary> library;
    HRESULT hr = DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&library));
    if (FAILED(hr) || !library)
        return E_FAIL;

    CComPtr<IDxcBlobEncoding> containerBlob;
    hr = library->CreateBlobWithEncodingOnHeapCopy(
        dxil.data(), static_cast<UINT32>(dxil.size()), CP_UTF8, &containerBlob);
    if (FAILED(hr) || !containerBlob)
        return E_FAIL;

    CComPtr<IUnknown> containerRefl;
    hr = DxcCreateInstance(CLSID_DxcContainerReflection, IID_PPV_ARGS(&containerRefl));
    if (FAILED(hr) || !containerRefl)
        return E_FAIL;

    void** vtable = *(void***)(IUnknown*)containerRefl.p;

    // vtable[3] = Load(IUnknown*, IDxcBlob*) -> HRESULT
    auto loadFn = (HRESULT(STDMETHODCALLTYPE*)(IUnknown*, IDxcBlob*))vtable[3];
    hr = loadFn(containerRefl.p, containerBlob);
    if (FAILED(hr))
        return hr;

    // vtable[4] = FindFirstPartKind(IUnknown*, UINT32, UINT32*) -> HRESULT
    auto findFn = (HRESULT(STDMETHODCALLTYPE*)(IUnknown*, UINT32, UINT32*))vtable[4];
    const UINT32 DXIL_FOURCC = 'D' | ('X' << 8) | ('I' << 16) | ('L' << 24);
    UINT32 partIdx = 0;
    hr = findFn(containerRefl.p, DXIL_FOURCC, &partIdx);
    if (FAILED(hr))
    {
        // Try with reflection part kind 'RDAT'
        const UINT32 RDAT_FOURCC = 'R' | ('D' << 8) | ('A' << 16) | ('T' << 24);
        hr = findFn(containerRefl.p, RDAT_FOURCC, &partIdx);
        if (FAILED(hr))
        {
            // Try 'STAT'
            const UINT32 STAT_FOURCC = 'S' | ('T' << 8) | ('A' << 16) | ('T' << 24);
            hr = findFn(containerRefl.p, STAT_FOURCC, &partIdx);
        }
    }
    if (FAILED(hr))
        return hr;

    // vtable[5] = GetPartReflection(IUnknown*, UINT32, REFIID, void**) -> HRESULT
    auto getPartReflFn = (HRESULT(STDMETHODCALLTYPE*)(IUnknown*, UINT32, REFIID, void**))vtable[5];
    return getPartReflFn(containerRefl.p, partIdx, IID_ID3D12ShaderReflection, (void**)&outRefl);
}

} // namespace

std::vector<uint8_t> build_reflection(const DxcResult& result, ShaderStage stage,
                                       const fs::path& input)
{
    CComPtr<ID3D12ShaderReflection> shaderRefl;
    if (FAILED(extractShaderReflection(result.refl.empty() ? result.dxil : result.refl, shaderRefl)) || !shaderRefl)
        throw std::runtime_error("Failed to extract shader reflection from DXIL");

    D3D12_SHADER_DESC shaderDesc{};
    shaderRefl->GetDesc(&shaderDesc);

    yasio::fast_obstream out;

    axslc::sc_chunk_refl header{};
    copy_name(header.name, sizeof(header.name), input.filename().string());
    header.debug_info = 1;
    out.write_bytes(&header, sizeof(header));

    // --- Vertex inputs ---
    if (stage == ShaderStage::Vertex)
    {
        for (UINT i = 0; i < shaderDesc.InputParameters; ++i)
        {
            D3D12_SIGNATURE_PARAMETER_DESC sigDesc{};
            shaderRefl->GetInputParameterDesc(i, &sigDesc);

            if (sigDesc.SystemValueType != D3D_NAME_UNDEFINED)
                continue;

            axslc::sc_refl_input inputEntry{};
            copy_name(inputEntry.name, sizeof(inputEntry.name), sigDesc.SemanticName);
            copy_name(inputEntry.semantic, sizeof(inputEntry.semantic), sigDesc.SemanticName);

            inputEntry.location = static_cast<int32_t>(sigDesc.Register);
            inputEntry.semantic_index = sigDesc.SemanticIndex;
            inputEntry.var_type = resolve_sc_type(sigDesc.ComponentType, sigDesc.Mask);

            out.write_bytes(&inputEntry, sizeof(inputEntry));
            ++header.num_inputs;
        }
    }

    // --- Constant buffers (UBOs) ---
    for (UINT i = 0; i < shaderDesc.ConstantBuffers; ++i)
    {
        auto* cb = shaderRefl->GetConstantBufferByIndex(i);
        D3D12_SHADER_BUFFER_DESC cbDesc{};
        cb->GetDesc(&cbDesc);

        if (cbDesc.Type != D3D_CT_CBUFFER || cbDesc.Variables == 0)
            continue;

        axslc::sc_refl_uniformbuffer ubo{};
        copy_name(ubo.name, sizeof(ubo.name), cbDesc.Name);
        ubo.binding = 0;
        ubo.size_bytes = cbDesc.Size;
        ubo.num_members = static_cast<uint16_t>(std::min<UINT>(cbDesc.Variables, 0xffff));
        ubo.array_size = 0;
        out.write_bytes(&ubo, sizeof(ubo));
        ++header.num_uniform_buffers;

        for (UINT j = 0; j < cbDesc.Variables; ++j)
        {
            auto* var = cb->GetVariableByIndex(j);
            D3D12_SHADER_VARIABLE_DESC varDesc{};
            var->GetDesc(&varDesc);

            auto* varType = var->GetType();
            D3D12_SHADER_TYPE_DESC varTypeDesc{};
            varType->GetDesc(&varTypeDesc);

            axslc::sc_refl_uniformbuffer_member member{};
            copy_name(member.name, sizeof(member.name), varDesc.Name);
            member.offset = static_cast<int32_t>(varDesc.StartOffset);
            member.size_bytes = varDesc.Size;
            member.array_size = static_cast<uint16_t>(std::max<UINT>(varTypeDesc.Elements, 1u));
            member.var_type = axslc::SC_TYPE_FLOAT4;
            out.write_bytes(&member, sizeof(member));
        }
    }

    // --- Textures (SRV bindings) ---
    for (UINT i = 0; i < shaderDesc.BoundResources; ++i)
    {
        D3D12_SHADER_INPUT_BIND_DESC bindDesc{};
        shaderRefl->GetResourceBindingDesc(i, &bindDesc);

        if (bindDesc.Type != D3D_SIT_TEXTURE)
            continue;

        axslc::sc_refl_texture texture{};
        copy_name(texture.name, sizeof(texture.name), bindDesc.Name);
        texture.binding = static_cast<int32_t>(bindDesc.BindPoint);
        texture.image_dim = static_cast<uint8_t>(bindDesc.Dimension);
        texture.count = static_cast<uint8_t>((std::max)(bindDesc.BindCount, 1u));
        texture.sampler_slot = 0;
        texture.multisample = 0;
        texture.arrayed = 0;

        out.write_bytes(&texture, sizeof(texture));
        ++header.num_textures;
    }

    std::memcpy(out.data(), &header, sizeof(header));
    return std::vector<uint8_t>(out.data(), out.data() + out.length());
}

} // namespace axslcc::dxc

#endif // _WIN32
