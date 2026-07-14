#include "reflection.h"

#include "spirv_cross.hpp"
#include "spirv_glsl.hpp"
#include "spirv_hlsl.hpp"
#include "spirv_msl.hpp"
#include "utils.h"
#include "yasio/obstream.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>

namespace axslcc::reflection
{

namespace
{

constexpr std::string_view kPresetSamplers[] = {
    "LinearClamp", "LinearWrap", "LinearMirror", "LinearBorder",
    "PointClamp", "PointWrap", "PointMirror", "PointBorder",
    "LinearMipClamp", "LinearMipWrap", "LinearMipMirror", "LinearMipBorder",
    "AnisoClamp", "AnisoWrap", "AnisoMirror", "AnisoBorder",
    "ShadowCmpClamp", "ShadowCmpWrap", "ShadowCmpMirror", "ShadowCmpBorder",
    "LinearNoMipClamp", "PointNoMipClamp",
};

int16_t preset_sampler_index(std::string_view name)
{
    for (size_t i = 0; i < std::size(kPresetSamplers); ++i)
        if (name == kPresetSamplers[i])
            return static_cast<int16_t>(i);
    return -1;
}

constexpr VariableTypeMap kVariableTypeMap[] = {
    {spirv_cross::SPIRType::Float, 4, 1, axslc::SC_TYPE_FLOAT4},
    {spirv_cross::SPIRType::Float, 3, 1, axslc::SC_TYPE_FLOAT3},
    {spirv_cross::SPIRType::Float, 2, 1, axslc::SC_TYPE_FLOAT2},
    {spirv_cross::SPIRType::Float, 1, 1, axslc::SC_TYPE_FLOAT},
    {spirv_cross::SPIRType::Int, 4, 1, axslc::SC_TYPE_INT4},
    {spirv_cross::SPIRType::Int, 3, 1, axslc::SC_TYPE_INT3},
    {spirv_cross::SPIRType::Int, 2, 1, axslc::SC_TYPE_INT2},
    {spirv_cross::SPIRType::Int, 1, 1, axslc::SC_TYPE_INT},
    {spirv_cross::SPIRType::UShort, 4, 1, axslc::SC_TYPE_USHORT4},
    {spirv_cross::SPIRType::UShort, 2, 1, axslc::SC_TYPE_USHORT2},
    {spirv_cross::SPIRType::UByte, 4, 1, axslc::SC_TYPE_UBYTE4},
    {spirv_cross::SPIRType::Float, 4, 4, axslc::SC_TYPE_MAT4},
    {spirv_cross::SPIRType::Float, 3, 3, axslc::SC_TYPE_MAT3},
    {spirv_cross::SPIRType::Half, 4, 1, axslc::SC_TYPE_HALF4},
    {spirv_cross::SPIRType::Half, 3, 1, axslc::SC_TYPE_HALF3},
    {spirv_cross::SPIRType::Half, 2, 1, axslc::SC_TYPE_HALF2},
    {spirv_cross::SPIRType::Half, 1, 1, axslc::SC_TYPE_HALF},
};

uint16_t resolve_sc_type(const spirv_cross::SPIRType& type)
{
    for (const auto& item : kVariableTypeMap) {
        if (item.base_type == static_cast<uint32_t>(type.basetype) && item.vec_size == type.vecsize && item.columns == type.columns)
            return item.sc_type;
    }
    return axslc::SC_TYPE_FLOAT4;
}

uint16_t array_size(const spirv_cross::SPIRType& type)
{
    if (type.array.empty())
        return 1;

    uint32_t size = 1;
    for (uint32_t dim : type.array)
        size *= (std::max)(dim, 1u);
    return static_cast<uint16_t>((std::min)(size, 0xffffu));
}

template <typename T>
void write_struct(yasio::fast_obstream& out, const T& value)
{
    out.write_bytes(&value, static_cast<int>(sizeof(T)));
}

void copy_name(char* dst, size_t size, std::string_view name)
{
    std::memset(dst, 0, size);
    const auto copy_size = std::min(name.size(), size - 1);
    std::memcpy(dst, name.data(), copy_size);
}

std::unique_ptr<spirv_cross::CompilerGLSL> make_compiler(const Target& target, const std::vector<uint32_t>& spirv)
{
    if (target.lang == axslc::SHADER_LANG_HLSL)
        return std::make_unique<spirv_cross::CompilerHLSL>(spirv);
    if (target.lang == axslc::SHADER_LANG_MSL)
        return std::make_unique<spirv_cross::CompilerMSL>(spirv);
    return std::make_unique<spirv_cross::CompilerGLSL>(spirv);
}

} // namespace

tlx::byte_buffer build_reflection(const Target& target, const std::vector<uint32_t>& spirv,
    ShaderStage stage, const fs::path& input,
    const std::vector<UniformBlockNameOverride>& uniform_block_names)
{
    auto compiler = target.lang == axslc::SHADER_LANG_SPIRV
        ? make_compiler(Target{axslc::SHADER_LANG_GLSL, 450, "glsl-450"}, spirv)
        : make_compiler(target, spirv);

    auto resources = compiler->get_shader_resources();
    
    yasio::fast_obstream out;

    axslc::sc_chunk_refl header{};
    copy_name(header.name, sizeof(header.name), input.filename().string());
    header.debug_info = 1;
    write_struct(out, header);

    const bool is_hlsl = utils::is_hlsl_source(input);

    auto write_input = [&](const spirv_cross::Resource& resource) {
        auto& type = compiler->get_type(resource.type_id);

        auto write_one = [&](const std::string& name, const spirv_cross::SPIRType& input_type,
                            uint32_t location, std::string_view semantic_name = {}) {
            axslc::sc_refl_input input_refl{};
            copy_name(input_refl.name, sizeof(input_refl.name), name);
            input_refl.location = static_cast<int32_t>(location);
            input_refl.semantic_index = 0;
            input_refl.var_type = resolve_sc_type(input_type);

            if (!semantic_name.empty())
            {
                auto [semantic, sem_index] = utils::parse_semantic(semantic_name);
                copy_name(input_refl.semantic, sizeof(input_refl.semantic), semantic);
                input_refl.semantic_index = sem_index;
            }

            write_struct(out, input_refl);
            ++header.num_inputs;
        };

        const uint32_t member_count = static_cast<uint32_t>(type.member_types.size());
        if (member_count > 0)
        {
            for (uint32_t mi = 0; mi < member_count; ++mi)
            {
                const std::string name = compiler->get_member_name(type.self, mi);
                const std::string semantic = is_hlsl
                    ? compiler->get_member_decoration_string(type.self, mi, spv::DecorationHlslSemanticGOOGLE)
                    : std::string{};

                const auto& member_type = compiler->get_type(type.member_types[mi]);
                const uint32_t location = compiler->get_member_decoration(type.self, mi, spv::DecorationLocation);

                write_one(name, member_type, location, semantic);
            }
        }
        else
        {
            const std::string name = resource.name.empty()
                ? compiler->get_fallback_name(resource.id)
                : resource.name;

            const std::string semantic = is_hlsl
                ? compiler->get_decoration_string(resource.id, spv::DecorationHlslSemanticGOOGLE)
                : std::string{};

            const uint32_t location = compiler->get_decoration(resource.id, spv::DecorationLocation);

            write_one(name, type, location, semantic);
        }
    };

    if (stage == ShaderStage::Vertex) {
        for (const auto& resource : resources.stage_inputs)
            write_input(resource);
    }

    auto write_ubo = [&](const spirv_cross::Resource& resource) {
        auto& type = compiler->get_type(resource.base_type_id);
        axslc::sc_refl_uniformbuffer ubo{};
        ubo.binding = static_cast<int32_t>(compiler->get_decoration(resource.id, spv::DecorationBinding));
        const auto descriptor_set = static_cast<uint16_t>(compiler->has_decoration(resource.id, spv::DecorationDescriptorSet)
            ? compiler->get_decoration(resource.id, spv::DecorationDescriptorSet) : 0);
        std::string block_name = resource.name.empty() ? compiler->get_fallback_name(resource.base_type_id) : resource.name;
        for (const auto& item : uniform_block_names)
        {
            if (item.binding == ubo.binding && item.descriptor_set == descriptor_set)
            {
                block_name = item.name;
                break;
            }
        }
        copy_name(ubo.name, sizeof(ubo.name), block_name);
        ubo.size_bytes = static_cast<uint32_t>(compiler->get_declared_struct_size(type));
        ubo.array_size = array_size(compiler->get_type(resource.type_id));
        ubo.num_members = static_cast<uint16_t>(std::min<size_t>(type.member_types.size(), 0xffff));
        write_struct(out, ubo);
        ++header.num_uniform_buffers;

        for (uint32_t i = 0; i < ubo.num_members; ++i) {
            auto& member_type = compiler->get_type(type.member_types[i]);
            axslc::sc_refl_uniformbuffer_member member{};
            copy_name(member.name, sizeof(member.name), compiler->get_member_name(type.self, i));
            member.offset = static_cast<int32_t>(compiler->type_struct_member_offset(type, i));
            member.size_bytes = static_cast<uint32_t>(compiler->get_declared_struct_member_size(type, i));
            member.array_size = array_size(member_type);
            member.var_type = resolve_sc_type(member_type);
            write_struct(out, member);
        }
    };

    for (const auto& resource : resources.uniform_buffers)
        write_ubo(resource);

    auto write_texture = [&](const spirv_cross::Resource& resource, bool storage) {
        auto& type = compiler->get_type(resource.type_id);
        axslc::sc_refl_texture texture{};
        copy_name(texture.name, sizeof(texture.name), resource.name.empty() ? compiler->get_fallback_name(resource.id) : resource.name);
        texture.binding = static_cast<int32_t>(compiler->get_decoration(resource.id, spv::DecorationBinding));
        texture.descriptor_set = static_cast<uint16_t>(compiler->has_decoration(resource.id, spv::DecorationDescriptorSet)
            ? compiler->get_decoration(resource.id, spv::DecorationDescriptorSet) : 0);
        texture.image_dim = static_cast<uint8_t>(type.image.dim);
        texture.multisample = type.image.ms ? 1 : 0;
        texture.arrayed = type.image.arrayed ? 1 : 0;
        texture.count = array_size(type);
        texture.sampler_source = resources.separate_images.empty()
            ? axslc::SC_SAMPLER_SOURCE_TEXTURE_OWNED
            : axslc::SC_SAMPLER_SOURCE_SHADER_PRESET;
        write_struct(out, texture);
        if (storage)
            ++header.num_storage_images;
        else
            ++header.num_textures;
    };

    for (const auto& resource : resources.sampled_images)
        write_texture(resource, false);

    for (const auto& resource : resources.separate_images)
        write_texture(resource, false);

    for (const auto& resource : resources.separate_samplers) {
        auto& type = compiler->get_type(resource.type_id);
        axslc::sc_refl_sampler sampler{};
        const auto name = resource.name.empty() ? compiler->get_fallback_name(resource.id) : resource.name;
        copy_name(sampler.name, sizeof(sampler.name), name);
        sampler.binding = static_cast<int32_t>(compiler->get_decoration(resource.id, spv::DecorationBinding));
        sampler.descriptor_set = static_cast<uint16_t>(compiler->has_decoration(resource.id, spv::DecorationDescriptorSet)
            ? compiler->get_decoration(resource.id, spv::DecorationDescriptorSet) : 0);
        sampler.count = array_size(type);
        sampler.preset_index = preset_sampler_index(name);
        sampler.comparison = type.basetype == spirv_cross::SPIRType::Sampler && type.image.depth ? 1 : 0;
        write_struct(out, sampler);
        ++header.num_samplers;
    }

    if (!resources.separate_images.empty() && !resources.separate_samplers.empty()) {
        compiler->build_combined_image_samplers();
        for (const auto& combined : compiler->get_combined_image_samplers()) {
            axslc::sc_refl_sampling_pair pair{};
            pair.texture_binding = static_cast<int32_t>(compiler->get_decoration(combined.image_id, spv::DecorationBinding));
            pair.sampler_binding = static_cast<int32_t>(compiler->get_decoration(combined.sampler_id, spv::DecorationBinding));
            pair.texture_set = static_cast<uint16_t>(compiler->has_decoration(combined.image_id, spv::DecorationDescriptorSet)
                ? compiler->get_decoration(combined.image_id, spv::DecorationDescriptorSet) : 0);
            pair.sampler_set = static_cast<uint16_t>(compiler->has_decoration(combined.sampler_id, spv::DecorationDescriptorSet)
                ? compiler->get_decoration(combined.sampler_id, spv::DecorationDescriptorSet) : 0);
            pair.preset_index = preset_sampler_index(compiler->get_name(combined.sampler_id));
            pair.sampler_source = pair.preset_index >= 0
                ? axslc::SC_SAMPLER_SOURCE_SHADER_PRESET
                : axslc::SC_SAMPLER_SOURCE_CUSTOM;
            write_struct(out, pair);
            ++header.num_sampling_pairs;
        }
    }

    if (stage == ShaderStage::Compute) {
        for (const auto& resource : resources.storage_images)
            write_texture(resource, true);

        for (const auto& resource : resources.storage_buffers) {
            auto& type = compiler->get_type(resource.base_type_id);
            axslc::sc_refl_buffer buffer{};
            copy_name(buffer.name, sizeof(buffer.name), resource.name.empty() ? compiler->get_fallback_name(resource.base_type_id) : resource.name);
            buffer.binding = static_cast<int32_t>(compiler->get_decoration(resource.id, spv::DecorationBinding));
            buffer.size_bytes = static_cast<uint32_t>(compiler->get_declared_struct_size(type));
            buffer.array_stride = static_cast<uint32_t>(compiler->get_declared_struct_size_runtime_array(type, 1)
                - compiler->get_declared_struct_size_runtime_array(type, 0));
            write_struct(out, buffer);
            ++header.num_storage_buffers;
        }
    }

    std::memcpy(out.data(), &header, sizeof(header));
    return std::move(out.buffer());
}

} // namespace axslcc::reflection
