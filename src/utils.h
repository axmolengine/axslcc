#pragma once

#include "types.h"

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

// ============= Argument Parsing =============

Target parse_target(std::string_view text);
Options parse_args(int argc, char** argv);
void print_help();

// ============= File I/O =============

std::string read_text_file(const fs::path& path);
void write_file(const fs::path& path, const std::vector<uint8_t>& data);

// ============= Stage & Format Detection =============

std::optional<ShaderStage> stage_from_name(const fs::path& input);
bool is_hlsl_source(const fs::path& input);

// ============= Path Utilities =============

fs::path output_path_for_target(const Options& options, const Target& target);

} // namespace axslcc::utils
