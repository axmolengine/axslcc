/****************************************************************************
 Copyright (c) 2026 Simdsoft Limited.

 https://axmol.dev/

 SPDX-License-Identifier: MIT
 ****************************************************************************/
#include "spirv_compiler.h"
#include "dxc_compiler.h"
#include "utils.h"

#include "axslc-spec.h"

#include "spirv-tools/optimizer.hpp"
#include "spirv-tools/libspirv.hpp"

#include <algorithm>
#include <cstring>
#include <fmt/format.h>
#include <limits>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace axslcc::spirv
{

namespace
{

std::string strip_comments(std::string_view source)
{
    std::string result;
    result.reserve(source.size());
    for (size_t i = 0; i < source.size();)
    {
        if (i + 1 < source.size() && source[i] == '/' && source[i + 1] == '/')
        {
            i += 2;
            while (i < source.size() && source[i] != '\n')
                ++i;
            if (i < source.size())
                result.push_back(source[i++]);
            continue;
        }
        if (i + 1 < source.size() && source[i] == '/' && source[i + 1] == '*')
        {
            i += 2;
            while (i + 1 < source.size() && !(source[i] == '*' && source[i + 1] == '/'))
            {
                if (source[i] == '\n')
                    result.push_back('\n');
                ++i;
            }
            if (i + 1 < source.size())
                i += 2;
            continue;
        }
        result.push_back(source[i++]);
    }
    return result;
}

// Like strip_comments() but length-preserving: comments collapse to spaces so match
// offsets taken on the mask address the original text directly.
std::string mask_comments(std::string_view source)
{
    std::string result;
    result.reserve(source.size());
    for (size_t i = 0; i < source.size();)
    {
        if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '/')
        {
            while (i < source.size() && source[i] != '\n')
                result.push_back(' '), ++i;
            continue;
        }
        if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '*')
        {
            result += "  ";
            i += 2;
            while (i < source.size())
            {
                if (source[i] == '*' && i + 1 < source.size() && source[i + 1] == '/')
                {
                    result += "  ";
                    i += 2;
                    break;
                }
                result.push_back(source[i] == '\n' ? '\n' : ' ');
                ++i;
            }
            continue;
        }
        if (source[i] == '"' || source[i] == '\'')
        {
            const char quote = source[i++];
            result.push_back(quote);
            while (i < source.size() && source[i] != quote)
            {
                if (source[i] == '\\' && i + 1 < source.size())
                    result.push_back(source[i++]);
                result.push_back(source[i++]);
            }
            if (i < source.size())
                result.push_back(source[i++]);
            continue;
        }
        result.push_back(source[i++]);
    }
    return result;
}

void validate_no_explicit_resource_registers(std::string_view source)
{
    static const std::regex kResourceRegisterPattern(
        R"(:\s*register\s*\(\s*[btsu]\d*)",
        std::regex_constants::icase | std::regex_constants::optimize);

    if (std::regex_search(strip_comments(source), kResourceRegisterPattern))
        throw std::runtime_error(
            "Explicit resource register assignments are not supported. "
            "Remove ': register(...)'; resource bindings are assigned automatically by axslcc.");
}

bool is_base_include_line(std::string_view line)
{
    static const std::regex kBaseIncludePattern(
        R"(^\s*#\s*include\s*[<"]base\.hlsli[>"]\s*(?://.*)?$)",
        std::regex_constants::icase | std::regex_constants::optimize);
    return std::regex_match(line.begin(), line.end(), kBaseIncludePattern);
}

fs::path find_base_hlsli(const Options& options)
{
    std::vector<fs::path> search_dirs;
    if (!options.input.empty() && !options.input.parent_path().empty())
        search_dirs.push_back(options.input.parent_path());
    search_dirs.insert(search_dirs.end(), options.include_dirs.begin(), options.include_dirs.end());

    for (const auto& dir : search_dirs)
    {
        auto candidate = dir / "base.hlsli";
        if (fs::exists(candidate))
            return candidate;
    }

    throw std::runtime_error("failed to locate base.hlsli for Axmol built-in sampler layout");
}

std::string inline_base_hlsli(std::string source, const Options& options)
{
    std::string result;
    result.reserve(source.size());

    bool inlined = false;
    size_t line_start = 0;
    while (line_start < source.size())
    {
        const size_t line_end = source.find('\n', line_start);
        const size_t line_size = (line_end == std::string::npos ? source.size() : line_end) - line_start;
        const std::string_view line(source.data() + line_start, line_size);

        if (is_base_include_line(line))
        {
            if (!inlined)
            {
                auto base = utils::read_text_file(find_base_hlsli(options));
                result += "\n// axslcc inlined base.hlsli for resource layout\n";
                result += base;
                if (!result.empty() && result.back() != '\n')
                    result.push_back('\n');
                inlined = true;
            }
        }
        else
        {
            result.append(line);
            if (line_end != std::string::npos)
                result.push_back('\n');
        }

        if (line_end == std::string::npos)
            break;
        line_start = line_end + 1;
    }

    return result;
}

uint32_t resource_array_count(std::string_view arraySuffix, std::string_view name)
{
    if (arraySuffix.empty())
        return 1;

    std::string_view value = arraySuffix;
    if (value.size() >= 2 && value.front() == '[' && value.back() == ']')
        value = value.substr(1, value.size() - 2);

    const auto first = value.find_first_not_of(" \t\r\n");
    const auto last  = value.find_last_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        throw std::runtime_error("Resource array '" + std::string(name) + "' must use a numeric literal size.");

    value = value.substr(first, last - first + 1);
    if (!std::all_of(value.begin(), value.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
        throw std::runtime_error(
            "Resource array '" + std::string(name) +
            "' must use a numeric literal size because axslcc assigns resource bindings before compilation.");

    uint64_t count = 0;
    for (char ch : value)
        count = count * 10 + static_cast<uint64_t>(ch - '0');

    if (count == 0 || count > (std::numeric_limits<uint32_t>::max)())
        throw std::runtime_error("Resource array '" + std::string(name) + "' has an invalid size.");

    return static_cast<uint32_t>(count);
}

int32_t find_builtin_sampler_preset(std::string_view name)
{
    static constexpr std::string_view kPresetNames[] = {
        "LinearClamp", "LinearWrap", "LinearMirror", "LinearBorder",
        "PointClamp", "PointWrap", "PointMirror", "PointBorder",
        "LinearMipClamp", "LinearMipWrap", "LinearMipMirror", "LinearMipBorder",
        "AnisoClamp", "AnisoWrap", "AnisoMirror", "AnisoBorder",
        "ShadowCmpClamp", "ShadowCmpWrap", "ShadowCmpMirror", "ShadowCmpBorder",
        "LinearNoMipClamp", "PointNoMipClamp",
    };

    for (size_t i = 0; i < std::size(kPresetNames); ++i)
    {
        if (kPresetNames[i] == name)
            return static_cast<int32_t>(i);
    }

    return -1;
}

std::string inject_hlsl_resource_layout(std::string source, const Options& options)
{
    struct Allocation
    {
        uint32_t cbv{0};
        uint32_t resource{0};  // unified logical slot: sampled textures + storage buffers share one sequence
    } alloc;

    if (options.stage == ShaderStage::Fragment)
        alloc.cbv = 1;
    uint32_t customSamplerIndex = 0;

    auto apply_regex = [](std::string text, const std::regex& pattern, auto&& replacer) {
        std::string result;
        size_t last = 0;
        for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it)
        {
            const auto& match = *it;
            result.append(text, last, static_cast<size_t>(match.position()) - last);
            result += replacer(match);
            last = static_cast<size_t>(match.position() + match.length());
        }
        result.append(text, last, std::string::npos);
        return result;
    };

    // Scan over a comment mask: a trailing `// ...` between the block name and its brace must
    // not hide the declaration, and a `cbuffer` that exists only inside a comment must not
    // consume a slot. Mask offsets address `source` directly.
    static const std::regex kCBufferPattern(R"(\b(cbuffer)\s+([A-Za-z_]\w*))", std::regex_constants::optimize);

    const std::string masked = mask_comments(source);
    if (masked.size() != source.size())
        throw std::runtime_error("internal error: comment mask length differs from the source");
    std::string annotated;
    annotated.reserve(source.size() + 64);
    size_t last = 0;
    for (std::sregex_iterator it(masked.begin(), masked.end(), kCBufferPattern), end; it != end; ++it)
    {
        const std::smatch& match  = *it;
        const size_t nameEnd      = static_cast<size_t>(match.position(2)) + static_cast<size_t>(match.length(2));
        const size_t opener       = masked.find_first_not_of(" \t\r\n", nameEnd);
        if (opener == std::string::npos || masked[opener] != '{')
            throw std::runtime_error("cbuffer '" + match.str(2) +
                                     "' is not followed by '{', so axslcc cannot assign it a register. "
                                     "Declare it as 'cbuffer Name {' without manual register/packoffset annotations.");

        annotated.append(source, last, nameEnd - last);
        annotated += " : register(b" + std::to_string(alloc.cbv++) + ", space0)";
        last = nameEnd;
    }
    annotated.append(source, last, std::string::npos);
    source = std::move(annotated);

    static const std::regex kUnsupportedResourcePattern(
        R"(\b(ConstantBuffer\s*<[^;{}>]+>|Texture2DMS(?:Array)?(?:\s*<[^;{}>]+>)?|(?:Append|Consume)StructuredBuffer\s*<[^;{}>]+>|RasterizerOrdered(?:Texture(?:1D|2D|3D)(?:Array)?|Buffer|StructuredBuffer|ByteAddressBuffer)(?:\s*<[^;{}>]+>)?)\s+([A-Za-z_]\w*)\s*(\[[^\]]+\])?\s*;)",
        std::regex_constants::optimize);
    for (std::sregex_iterator it(source.begin(), source.end(), kUnsupportedResourcePattern), end; it != end; ++it)
    {
        throw std::runtime_error(
            "HLSL resource type '" + (*it).str(1) + "' used by '" + (*it).str(2) +
            "' is not supported by the Axmol shader ABI yet.");
    }

    static const std::regex kResourcePattern(
        R"(\b((?:RW)?Texture(?:1D|2D|3D|Cube)(?:Array)?(?:\s*<[^;{}>]+>)?|(?:RW)?(?:StructuredBuffer|ByteAddressBuffer)(?:\s*<[^;{}>]+>)?|(?:RW)?Buffer(?:\s*<[^;{}>]+>)?|Sampler(?:Comparison)?State)\s+([A-Za-z_]\w*)\s*(\[[^\]]+\])?\s*;)",
        std::regex_constants::optimize);

    source = apply_regex(std::move(source), kResourcePattern, [&](const std::smatch& match) {
        const std::string type = match.str(1);
        const std::string name = match.str(2);
        const std::string arraySuffix = match.str(3);
        const uint32_t count = resource_array_count(arraySuffix, name);

        char regClass = 't';
        uint32_t binding = 0;
        uint32_t space = 1;

        if (type.find("Sampler") != std::string::npos)
        {
            regClass = 's';

            auto presetIdx = find_builtin_sampler_preset(name);
            if (presetIdx >= 0)
            {
                space   = static_cast<uint32_t>(axslc::kPresetSamplerDescriptorSet);
                binding = static_cast<uint32_t>(presetIdx);
            }
            else
            {
                if (count > 1)
                    throw std::runtime_error(
                        "Custom sampler array '" + name +
                        "' is not supported. Custom samplers must be non-array (count = 1).");
                space   = static_cast<uint32_t>(axslc::kCustomSamplerDescriptorSet);
                binding = customSamplerIndex;
                customSamplerIndex += count;
            }
        }
        else if (type.rfind("RW", 0) == 0)
        {
            regClass = 'u';
            binding = alloc.resource;
            alloc.resource += count;
        }
        else
        {
            regClass = 't';
            binding = alloc.resource;
            alloc.resource += count;
        }

        return type + " " + name + arraySuffix + " : register(" + regClass + std::to_string(binding) + ", space" +
               std::to_string(space) + ");";
    });

    return source;
}

} // namespace

std::vector<uint32_t> make_vulkan_runtime_spirv(const std::vector<uint32_t>& spirv)
{
    spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_1);
    optimizer.RegisterPass(spvtools::CreateStripNonSemanticInfoPass());

    std::string diagnostics;
    auto consume_message = [&diagnostics](spv_message_level_t, const char*, const spv_position_t& position,
                                          const char* message) {
        diagnostics += fmt::format("{}: {}\n", position.index, message ? message : "unknown SPIR-V error");
    };
    optimizer.SetMessageConsumer(consume_message);

    std::vector<uint32_t> runtime_spirv;
    if (!optimizer.Run(spirv.data(), spirv.size(), &runtime_spirv))
        throw std::runtime_error(fmt::format("failed to strip non-runtime SPIR-V metadata\n{}", diagnostics));

    spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
    validator.SetMessageConsumer(consume_message);
    if (!validator.Validate(runtime_spirv))
        throw std::runtime_error(fmt::format("Vulkan runtime SPIR-V validation failed\n{}", diagnostics));

    return runtime_spirv;
}

CompileUnit compile_input(const Options& options, const Target& target)
{
    std::string source_text = utils::read_text_file(options.input);
    source_text = inline_base_hlsli(std::move(source_text), options);
    validate_no_explicit_resource_registers(source_text);
    source_text = inject_hlsl_resource_layout(std::move(source_text), options);

    auto result = dxc::compile_spirv(source_text, options, target);
    if (result.object.size() % sizeof(uint32_t) != 0)
        throw std::runtime_error("DXC produced a malformed SPIR-V object");

    CompileUnit unit;
    unit.stage = options.stage;
    unit.spirv.resize(result.object.size() / sizeof(uint32_t));
    std::memcpy(unit.spirv.data(), result.object.data(), result.object.size());

    // DXC preserves all globals from base.hlsli as entry-point interface
    // variables. Remove unused sampler presets before reflection/layout
    // generation; otherwise a shader using one sampler would expose all preset samplers.
    spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_1);
    optimizer.RegisterPassFromFlag("--eliminate-dead-variables", false);
    std::vector<uint32_t> optimized;
    if (!optimizer.Run(unit.spirv.data(), unit.spirv.size(), &optimized))
        throw std::runtime_error("failed to eliminate unused DXC SPIR-V resources");
    unit.spirv = std::move(optimized);
    return unit;
}

std::string spirv_to_bytes(const std::vector<uint32_t>& spirv)
{
    std::string bytes;
    bytes.reserve(spirv.size() * sizeof(uint32_t));
    bytes.append(reinterpret_cast<const char*>(spirv.data()), spirv.size() * sizeof(uint32_t));
    return bytes;
}

} // namespace axslcc::spirv
