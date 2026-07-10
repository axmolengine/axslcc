#ifdef _WIN32

#include "dxc_compiler.h"
#include "axslc-spec.h"
#include "yasio/obstream.hpp"

#include <d3d12shader.h>

#include <cstdio>
#include <cstring>
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

// Map D3D12 input signature component type + mask to SC_TYPE_*
uint16_t resolve_input_sc_type(D3D_REGISTER_COMPONENT_TYPE compType, BYTE mask)
{
    int count = 0;
    if (mask & D3D_COMPONENT_MASK_X) ++count;
    if (mask & D3D_COMPONENT_MASK_Y) ++count;
    if (mask & D3D_COMPONENT_MASK_Z) ++count;
    if (mask & D3D_COMPONENT_MASK_W) ++count;

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

// Map D3D12_SHADER_TYPE_DESC (UBO member) to SC_TYPE_*
uint16_t resolve_ubo_sc_type(const D3D12_SHADER_TYPE_DESC& td)
{
    auto elems = static_cast<uint16_t>(td.Columns);
    auto rows  = static_cast<uint16_t>(td.Rows);

    if (td.Class == D3D_SVC_MATRIX_COLUMNS || td.Class == D3D_SVC_MATRIX_ROWS)
    {
        if (td.Type == D3D_SVT_FLOAT)
        {
            if (elems == 4 && rows == 4) return axslc::SC_TYPE_MAT4;
            if (elems == 3 && rows == 3) return axslc::SC_TYPE_MAT3;
        }
        return axslc::SC_TYPE_FLOAT4;
    }

    // VECTOR or SCALAR
    int count = (td.Class == D3D_SVC_SCALAR) ? 1 : std::max<int>(elems, 1);

    switch (td.Type)
    {
    case D3D_SVT_FLOAT:
    case D3D_SVT_FLOAT16:
        switch (count) {
        case 1: return axslc::SC_TYPE_FLOAT;
        case 2: return axslc::SC_TYPE_FLOAT2;
        case 3: return axslc::SC_TYPE_FLOAT3;
        case 4: return axslc::SC_TYPE_FLOAT4;
        default: return axslc::SC_TYPE_FLOAT4;
        }
    case D3D_SVT_INT:
        switch (count) {
        case 1: return axslc::SC_TYPE_INT;
        case 2: return axslc::SC_TYPE_INT2;
        case 3: return axslc::SC_TYPE_INT3;
        case 4: return axslc::SC_TYPE_INT4;
        default: return axslc::SC_TYPE_INT4;
        }
    case D3D_SVT_UINT:
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

bool is_multisample(D3D_SRV_DIMENSION dim)
{
    return dim == D3D_SRV_DIMENSION_TEXTURE2DMS ||
           dim == D3D_SRV_DIMENSION_TEXTURE2DMSARRAY;
}

bool is_arrayed(D3D_SRV_DIMENSION dim)
{
    return dim == D3D_SRV_DIMENSION_TEXTURE1DARRAY ||
           dim == D3D_SRV_DIMENSION_TEXTURE2DARRAY ||
           dim == D3D_SRV_DIMENSION_TEXTURE2DMSARRAY ||
           dim == D3D_SRV_DIMENSION_TEXTURECUBEARRAY;
}

} // namespace

tlx::byte_buffer build_reflection(ID3D12ShaderReflection* shaderRefl, ShaderStage stage,
                                       const fs::path& input)
{
    if (!shaderRefl)
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
            inputEntry.var_type = resolve_input_sc_type(sigDesc.ComponentType, sigDesc.Mask);

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

        D3D12_SHADER_INPUT_BIND_DESC bindDesc{};
        shaderRefl->GetResourceBindingDescByName(cbDesc.Name, &bindDesc);
        ubo.binding = static_cast<int32_t>(bindDesc.BindPoint);
        ubo.size_bytes = cbDesc.Size;
        ubo.num_members = static_cast<uint16_t>(std::min<UINT>(cbDesc.Variables, 0xffff));
        ubo.array_size = static_cast<uint16_t>(std::max<UINT>(bindDesc.BindCount, 1u));
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
            member.var_type = resolve_ubo_sc_type(varTypeDesc);
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
        texture.multisample = is_multisample(bindDesc.Dimension) ? 1 : 0;
        texture.arrayed = is_arrayed(bindDesc.Dimension) ? 1 : 0;
        texture.sampler_slot = 0;

        out.write_bytes(&texture, sizeof(texture));
        ++header.num_textures;
    }

    std::memcpy(out.data(), &header, sizeof(header));
    return std::move(out.buffer());
}

} // namespace axslcc::dxc

#endif // _WIN32
