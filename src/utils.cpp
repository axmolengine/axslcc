#include "utils.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>

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
    std::cerr
        << "axslcc - Axmol shader compiler\n\n"
        << "Usage:\n"
        << "  axslcc --input <file> [--output <path>] --target=<lang-profile>[;<lang-profile>] [--sc] [--reflect]\n"
        << "  axslcc --input <glsl> --output <path> --target=hlsl-51 --migrate\n\n"
        << "Targets:\n"
        << "  hlsl-50, hlsl-51, gles-300, glsl-330, glsl-450, spirv-100\n\n"
        << "Options:\n"
        << "  --input <file>      Input HLSL 5.1 or GLSL file\n"
        << "  --output <path>     Output file or basename. Defaults to input stem\n"
        << "  --target <targets>  Semicolon-separated output targets\n"
        << "  --sc                Write one Axmol .sc file containing all targets\n"
        << "  --reflect           Include reflection data in .sc output\n"
        << "  --migrate           GLSL to HLSL migration mode\n"
        << "  -DNAME[=VALUE]      Preprocessor define\n"
        << "  -I<dir>             Include directory\n";
}

Target parse_target(std::string_view text)
{
    auto dash = text.find('-');
    if (dash == std::string_view::npos)
        throw std::runtime_error("invalid target '" + std::string(text) + "'");

    std::string lang = lower(std::string(text.substr(0, dash)));
    int profile = std::stoi(std::string(text.substr(dash + 1)));

    Target target;
    target.profile = profile;
    target.spec = lang + "-" + std::to_string(profile);

    if (lang == "hlsl" && (profile == 50 || profile == 51)) {
        target.lang = axslc::SHADER_LANG_HLSL;
    } else if (lang == "gles" && profile == 300) {
        target.lang = axslc::SHADER_LANG_ESSL;
    } else if (lang == "glsl" && (profile == 330 || profile == 450)) {
        target.lang = axslc::SHADER_LANG_GLSL;
    } else if (lang == "spirv" && profile == 100) {
        target.lang = axslc::SHADER_LANG_SPIRV;
    } else {
        throw std::runtime_error("unsupported target '" + std::string(text) + "'");
    }

    return target;
}

Options parse_args(int argc, char** argv)
{
    Options options;

    auto require_value = [](int argc, char** argv, int& i, std::string_view option, std::string& out) {
        if (i + 1 >= argc)
            throw std::runtime_error(std::string(option) + " requires a value");
        out = argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::string value;

        if (arg == "--help" || arg == "-h") {
            print_help();
            std::exit(0);
        } else if (arg == "--input") {
            require_value(argc, argv, i, "--input", value);
            options.input = value;
        } else if (starts_with(arg, "--input=")) {
            options.input = arg.substr(8);
        } else if (arg == "--output") {
            require_value(argc, argv, i, "--output", value);
            options.output = value;
        } else if (starts_with(arg, "--output=")) {
            options.output = arg.substr(9);
        } else if (arg == "--target") {
            require_value(argc, argv, i, "--target", value);
            for (const auto& item : split(value, ';'))
                options.targets.push_back(parse_target(item));
        } else if (starts_with(arg, "--target=")) {
            for (const auto& item : split(arg.substr(9), ';'))
                options.targets.push_back(parse_target(item));
        } else if (arg == "--sc") {
            options.sc = true;
        } else if (arg == "--reflect") {
            options.reflect = true;
        } else if (arg == "--migrate") {
            options.migrate = true;
        } else if (starts_with(arg, "-D")) {
            if (arg.size() == 2) {
                require_value(argc, argv, i, "-D", value);
                options.defines.push_back(value);
            } else {
                options.defines.push_back(arg.substr(2));
            }
        } else if (starts_with(arg, "-I")) {
            if (arg.size() == 2) {
                require_value(argc, argv, i, "-I", value);
                options.include_dirs.emplace_back(value);
            } else {
                options.include_dirs.emplace_back(arg.substr(2));
            }
        } else {
            throw std::runtime_error("unknown option '" + arg + "'");
        }
    }

    if (options.input.empty())
        throw std::runtime_error("--input is required");
    if (options.targets.empty())
        throw std::runtime_error("--target is required");
    if (options.reflect && !options.sc)
        throw std::runtime_error("--reflect is only valid with --sc");
    if (!fs::exists(options.input))
        throw std::runtime_error("input file does not exist: " + options.input.string());

    if (options.output.empty())
        options.output = options.input.parent_path() / options.input.stem();

    if (options.migrate) {
        auto ext = lower(options.input.extension().string());
        if (ext == ".hlsl" || ext == ".fx")
            throw std::runtime_error("--migrate expects GLSL input");
        for (const auto& target : options.targets) {
            if (target.lang != axslc::SHADER_LANG_HLSL)
                throw std::runtime_error("--migrate only supports HLSL output targets");
        }
    }

    return options;
}

std::string read_text_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("failed to open input: " + path.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_file(const fs::path& path, const std::vector<uint8_t>& data)
{
    if (!path.parent_path().empty())
        fs::create_directories(path.parent_path());

    std::ofstream out(path, std::ios::binary);
    if (!out)
        throw std::runtime_error("failed to open output: " + path.string());
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

std::optional<ShaderStage> stage_from_name(const fs::path& input)
{
    std::string ext = lower(input.extension().string());
    std::string stem = lower(input.stem().string());

    if (ext == ".vert" || ext == ".vs" || ext == ".vsh" || stem.ends_with("_vs") || stem.ends_with(".vs"))
        return ShaderStage::Vertex;
    if (ext == ".frag" || ext == ".fs" || ext == ".fsh" || ext == ".ps" || stem.ends_with("_fs") || stem.ends_with("_ps"))
        return ShaderStage::Fragment;
    if (ext == ".comp" || ext == ".cs" || stem.ends_with("_cs"))
        return ShaderStage::Compute;

    return std::nullopt;
}

bool is_hlsl_source(const fs::path& input)
{
    std::string ext = lower(input.extension().string());
    return ext == ".hlsl" || ext == ".fx";
}

fs::path output_path_for_target(const Options& options, const Target& target)
{
    fs::path out = options.output;
    if (options.sc)
        return out.extension() == ".sc" ? out : out.replace_extension(".sc");

    std::string ext;
    switch (target.lang) {
    case axslc::SHADER_LANG_HLSL:
        ext = ".hlsl" + std::to_string(target.profile);
        break;
    case axslc::SHADER_LANG_ESSL:
        ext = ".gles" + std::to_string(target.profile);
        break;
    case axslc::SHADER_LANG_GLSL:
        ext = ".glsl" + std::to_string(target.profile);
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
