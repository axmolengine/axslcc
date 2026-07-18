/****************************************************************************
 Copyright (c) 2019-present Axmol Engine contributors (see AUTHORS.md).

 https://axmol.dev/

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 ****************************************************************************/
#include "utils.h"
#include "version.h"

#include <algorithm>
#include <cctype>
#include <fmt/format.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <charconv>

namespace axslcc::utils
{

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::vector<std::string> split(std::string_view value, char delim)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= value.size()) {
        size_t end = value.find(delim, start);
        if (end == std::string_view::npos)
            end = value.size();
        if (end > start)
            parts.emplace_back(value.substr(start, end - start));
        start = end + 1;
        if (end == value.size())
            break;
    }
    return parts;
}

bool starts_with(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

void print_help()
{
    std::cout
        << "axslcc - Axmol shader compiler\n\n"
        << "Usage:\n"
        << "  axslcc [options] <input>\n\n"
        << "Targets:\n"
        << "  d3d11, d3d12, vk, mtl, gl, gles\n\n"
        << "Options:\n"
        << "  -o <path>          Output file or basename (default: input stem)\n"
        << "  -t <target>        Output target (repeatable)\n"
        << "  -a                 Write Axmol .sc archive with reflection data\n"
        << "  -x <lang>          Input language: hlsl, glsl (default: hlsl)\n"
        << "  --hlsl-frontend <dxc|glslang>\n"
        << "                     HLSL frontend (default: dxc)\n"
        << "  --vulkan-samplers <separate|combined>\n"
        << "                     Vulkan descriptor model (default: separate)\n"
        << "  -S                 Keep HLSL source, don't compile to DXBC/DXIL (D3D targets only)\n"
        << "  -O<level>          Optimization level: 0 (debug, default), 1 (favor size), 2 (favor speed), 3 (aggressive)\n"
        << "  -I <dir>           Include directory (repeatable)\n"
        << "  -D <name>[=<val>]  Preprocessor define (repeatable)\n"
        << "  --msl-ios          Target iOS Metal (default: macOS)\n"
        << "  --cvar <name>      C variable name for embedded shader data\n"
        << "  --version          Print version and exit\n";
}

void print_version()
{
    std::cout << "axslcc v" << AXSLCC_VERSION_STRING << "\n\n"
              << "Axslcc suite maintiained and supported by axmol community (axmol.dev)\n";
}

static void resolve_target_defines(std::vector<Target>& targets, const Options& options)
{
    for (auto& t : targets) {
        t.defines.clear();
        t.defines.reserve(options.defines.size() + 3);
        for (const auto& d : options.defines)
            t.defines.push_back(d);
        switch (t.lang) {
        case axslc::SHADER_LANG_HLSL:
            t.defines.emplace_back("AXSLC_TARGET_HLSL=1");
            t.defines.emplace_back("AXSLC_UV_TOP=1");
            break;
        case axslc::SHADER_LANG_MSL:
            t.defines.emplace_back("AXSLC_TARGET_MSL=1");
            t.defines.emplace_back("AXSLC_UV_TOP=1");
            break;
        case axslc::SHADER_LANG_SPIRV:
            t.defines.emplace_back("AXSLC_TARGET_SPIRV=1");
            t.defines.emplace_back("AXSLC_UV_TOP=1");
            break;
        case axslc::SHADER_LANG_ESSL:
            t.defines.emplace_back("AXSLC_TARGET_GLSL=1");
            t.defines.emplace_back("AXSLC_UV_TOP=0");
            t.defines.emplace_back("AXSLC_TARGET_ESSL=1");
            break;
        case axslc::SHADER_LANG_GLSL:
            t.defines.emplace_back("AXSLC_TARGET_GLSL=1");
            t.defines.emplace_back("AXSLC_UV_TOP=0");
            break;
        default:
            break;
        }
    }
}

Target parse_target(std::string_view text)
{
    struct PlatformEntry {
        std::string_view name;
        axslc::ShaderLang lang;
        int defaultProfile;
    };
    static constexpr PlatformEntry kPlatforms[] = {
        {"d3d11", axslc::SHADER_LANG_HLSL,  50},
        {"d3d12", axslc::SHADER_LANG_HLSL,  51},
        {"vk",    axslc::SHADER_LANG_SPIRV, 100},
        {"mtl",   axslc::SHADER_LANG_MSL,   20000},
        {"gl",    axslc::SHADER_LANG_GLSL,  330},
        {"gles",  axslc::SHADER_LANG_ESSL,  300},
    };

    auto parse_profile = [text](std::string_view value) -> int {
        if (value.empty())
            throw std::runtime_error(fmt::format("unsupported target '{}'", text));

        int profile = 0;
        auto* begin = value.data();
        auto* end = value.data() + value.size();
        auto [ptr, ec] = std::from_chars(begin, end, profile);
        if (ec != std::errc{} || ptr != end)
            throw std::runtime_error(fmt::format("unsupported target '{}'", text));
        return profile;
    };

    auto profile_supported = [](std::string_view platform, int profile) {
        if (platform == "d3d11")
            return profile == 50;
        if (platform == "d3d12")
            return profile == 51 || profile == 60;
        if (platform == "vk")
            return profile == 100;
        if (platform == "mtl")
            return profile >= 10000;
        if (platform == "gl")
            return profile == 330 || profile == 450;
        if (platform == "gles")
            return profile == 300;
        return false;
    };

    // New-style: platform[-version]
    auto dash = text.find('-');
    std::string_view platform = (dash != std::string_view::npos) ? text.substr(0, dash) : text;

    for (const auto& entry : kPlatforms)
    {
        if (platform == entry.name)
        {
            Target target;
            target.lang = entry.lang;
            target.spec = std::string(text);

            if (dash != std::string_view::npos)
                target.profile = parse_profile(text.substr(dash + 1));
            else
                target.profile = entry.defaultProfile;

            if (!profile_supported(platform, target.profile))
                throw std::runtime_error(fmt::format("unsupported target '{}'", text));

            return target;
        }
    }

    // Legacy format: lang-profile (e.g. hlsl-50, spirv-100, msl-20000)
    if (dash == std::string_view::npos)
        throw std::runtime_error(fmt::format("unsupported target '{}'", text));

    std::string lang = lower(std::string(text.substr(0, dash)));
    int profile = parse_profile(text.substr(dash + 1));

    Target target;
    target.profile = profile;
    target.spec = fmt::format("{}-{}", lang, profile);

    if (lang == "hlsl" && (profile == 50 || profile == 51)) {
        target.lang = axslc::SHADER_LANG_HLSL;
    } else if (lang == "msl" && (profile >= 10000)) {
        target.lang = axslc::SHADER_LANG_MSL;
    } else if (lang == "essl" && profile == 300) {
        target.lang = axslc::SHADER_LANG_ESSL;
    } else if (lang == "glsl" && (profile == 330 || profile == 450)) {
        target.lang = axslc::SHADER_LANG_GLSL;
    } else if (lang == "spirv" && profile == 100) {
        target.lang = axslc::SHADER_LANG_SPIRV;
    } else {
        throw std::runtime_error(fmt::format("unsupported target '{}'", text));
    }

    return target;
}

Options parse_args(int argc, char** argv)
{
    Options options;

    auto require_value = [](int argc, char** argv, int& i, std::string_view option, std::string& out) {
        if (i + 1 >= argc)
            throw std::runtime_error(fmt::format("{} requires a value", option));
        out = argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::string value;

        if (arg.empty())
            continue;

        if (arg == "--help" || arg == "-h") {
            print_help();
            std::exit(0);
        } else if (arg == "--version") {
            print_version();
            std::exit(0);
        } else if (arg == "-o") {
            require_value(argc, argv, i, "-o", value);
            options.output = value;
        } else if (starts_with(arg, "-o") && arg.size() > 2) {
            options.output = arg.substr(2);
        } else if (arg == "-t") {
            require_value(argc, argv, i, "-t", value);
            options.targets.push_back(parse_target(value));
        } else if (arg == "-a") {
            options.archive = true;
        } else if (arg == "-x") {
            require_value(argc, argv, i, "-x", value);
            auto lang = lower(value);
            if (lang == "hlsl")
                options.input_lang = InputLang::HLSL;
            else if (lang == "glsl")
                options.input_lang = InputLang::GLSL;
            else
                throw std::runtime_error(fmt::format("unknown input language '{}' (expected hlsl or glsl)", value));
            options.xlang = true;
        } else if (arg == "-S") {
            options.keep_source_hint = true;
        } else if (arg == "--vulkan-samplers") {
            require_value(argc, argv, i, "--vulkan-samplers", value);
            auto mode = lower(value);
            if (mode == "separate")
                options.vulkan_sampler_mode = VulkanSamplerMode::Separate;
            else if (mode == "combined")
                options.vulkan_sampler_mode = VulkanSamplerMode::Combined;
            else
                throw std::runtime_error(fmt::format("unknown Vulkan sampler mode '{}' (expected separate or combined)", value));
        } else if (arg == "--hlsl-frontend") {
            require_value(argc, argv, i, "--hlsl-frontend", value);
            auto frontend = lower(value);
            if (frontend == "dxc")
                options.hlsl_frontend = HlslFrontend::DXC;
            else if (frontend == "glslang")
                options.hlsl_frontend = HlslFrontend::Glslang;
            else
                throw std::runtime_error(fmt::format("unknown HLSL frontend '{}' (expected dxc or glslang)", value));
        } else if (arg == "-O0") {
            options.opt_level = 0;
        } else if (arg == "-O1") {
            options.opt_level = 1;
        } else if (arg == "-O2") {
            options.opt_level = 2;
        } else if (arg == "-O3") {
            options.opt_level = 3;
        } else if (arg == "-I") {
            require_value(argc, argv, i, "-I", value);
            options.include_dirs.emplace_back(value);
        } else if (starts_with(arg, "-I") && arg.size() > 2) {
            options.include_dirs.emplace_back(arg.substr(2));
        } else if (arg == "-D") {
            require_value(argc, argv, i, "-D", value);
            options.defines.push_back(value);
        } else if (starts_with(arg, "-D") && arg.size() > 2) {
            options.defines.push_back(arg.substr(2));
        } else if (arg == "--msl-ios") {
            options.msl_ios = true;
        } else if (arg == "--cvar") {
            require_value(argc, argv, i, "--cvar", value);
            options.cvar = value;
        } else if (!arg.empty() && arg[0] != '-') {
            if (!options.input.empty())
                throw std::runtime_error(fmt::format("multiple input files specified: '{}'", arg));
            options.input = arg;
        } else {
            throw std::runtime_error(fmt::format("unknown option '{}'", arg));
        }
    }

    if (options.input.empty())
        throw std::runtime_error("no input file specified");
    if (options.targets.empty())
        throw std::runtime_error("at least one target (-t) is required");
    if (!fs::exists(options.input))
        throw std::runtime_error(fmt::format("input file does not exist: {}", options.input.string()));

    auto info = classify(options.input);
    options.stage = info.stage;
    if (!options.xlang)
        options.input_lang = info.lang;

    if (options.output.empty())
        options.output = options.input.parent_path() / options.input.stem();

    resolve_target_defines(options.targets, options);

    return options;
}

std::string read_text_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error(fmt::format("failed to open input: {}", path.string()));
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_file(const fs::path& path, std::string_view data)
{
    if (!path.parent_path().empty())
        fs::create_directories(path.parent_path());

    std::ofstream out(path, std::ios::binary);
    if (!out)
        throw std::runtime_error(fmt::format("failed to open output: {}", path.string()));
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    if (!out)
        throw std::runtime_error(fmt::format("failed to write output: {}", path.string()));
}

ShaderInfo classify(const fs::path& input)
{
    ShaderInfo info;

    std::string stem = lower(input.stem().string());
    std::string ext = lower(input.extension().string());

    // Detect stage
    if (ext == ".vert")
        info.stage = ShaderStage::Vertex;
    else if (ext == ".frag")
        info.stage = ShaderStage::Fragment;
    else if (ext == ".comp")
        info.stage = ShaderStage::Compute;
    else if (stem.ends_with("_vs"))
        info.stage = ShaderStage::Vertex;
    else if (stem.ends_with("_fs") || stem.ends_with("_ps"))
        info.stage = ShaderStage::Fragment;
    else if (stem.ends_with("_cs"))
        info.stage = ShaderStage::Compute;
    else
        throw std::runtime_error(fmt::format("cannot determine shader stage from filename '{}' (expected _vs/_fs/_ps/_cs suffix or .vert/.frag/.comp extension)", input.string()));

    // Detect language
    if (ext == ".hlsl" || ext == ".fx" || ext == ".hlsli")
        info.lang = InputLang::HLSL;
    else if (ext == ".vert" || ext == ".frag" || ext == ".comp" || ext == ".glsl" ||
             ext == ".geom" || ext == ".tesc" || ext == ".tese")
        info.lang = InputLang::GLSL;

    return info;
}

bool is_hlsl_source(const fs::path& input)
{
    std::string ext = lower(input.extension().string());
    return ext == ".hlsl" || ext == ".fx";
}

std::pair<std::string, uint16_t> parse_semantic(std::string_view semantic)
{
    if (semantic.empty())
        return {std::string{}, 0};

    size_t digit_pos = semantic.size();
    while (digit_pos > 0 && std::isdigit(static_cast<unsigned char>(semantic[digit_pos - 1])))
        --digit_pos;

    uint16_t index = 0;
    if (digit_pos < semantic.size())
    {
        uint32_t parsed_index = 0;
        auto digits = semantic.substr(digit_pos);
        auto first  = digits.data();
        auto last   = digits.data() + digits.size();
        auto result = std::from_chars(first, last, parsed_index);
        if (result.ec != std::errc{} || result.ptr != last || parsed_index > UINT16_MAX)
            throw std::runtime_error(fmt::format("invalid vertex semantic index '{}'", digits));
        index = static_cast<uint16_t>(parsed_index);
    }

    return {std::string(semantic.substr(0, digit_pos)), index};
}

fs::path output_path_for_target(const Options& options, const Target& target)
{
    fs::path out = options.output;
    if (options.archive)
        return out;

    std::string ext;
    switch (target.lang) {
    case axslc::SHADER_LANG_HLSL:
        ext = fmt::format(".hlsl{}", target.profile);
        break;
    case axslc::SHADER_LANG_ESSL:
        ext = fmt::format(".essl{}", target.profile);
        break;
    case axslc::SHADER_LANG_GLSL:
        ext = fmt::format(".glsl{}", target.profile);
        break;
    case axslc::SHADER_LANG_MSL:
        ext = ".msl";
        break;
    case axslc::SHADER_LANG_SPIRV:
        ext = ".spv";
        break;
    default:
        ext = ".out";
        break;
    }
    return out.replace_extension(ext);
}

} // namespace axslcc::utils
