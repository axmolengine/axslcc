#pragma once

#include "types.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace axslcc::utils
{

// ============= String Utilities =============

std::string lower(std::string value);
std::vector<std::string> split(std::string_view value, char delim);
bool starts_with(std::string_view value, std::string_view prefix);

// ============= HLSL Source Parsing =============

// Parse vertex input semantics from HLSL VS_IN struct.
// Returns map: member_name -> {semantic_name, semantic_index}
// E.g. {"a_texCoord" -> {"TEXCOORD", 0}, "a_color" -> {"COLOR", 0}}
std::map<std::string, std::pair<std::string, uint16_t>> parse_hlsl_semantics(const fs::path& input);

// Strip "input." prefix from glslang-generated names
std::string clean_input_name(const std::string& name);

// ============= Argument Parsing =============

Target parse_target(std::string_view text);
Options parse_args(int argc, char** argv);
void print_help();
void print_version();

// ============= File I/O =============

std::string read_text_file(const fs::path& path);
void write_file(const fs::path& path, const std::vector<uint8_t>& data);

// ============= Stage & Format Detection =============

std::optional<ShaderStage> stage_from_name(const fs::path& input);
bool is_hlsl_source(const fs::path& input);

// ============= Path Utilities =============

fs::path output_path_for_target(const Options& options, const Target& target);

} // namespace axslcc::utils
