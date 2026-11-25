// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>

namespace tick_command {

void loadLanguages(std::filesystem::path const& dir);

std::string translate(std::string_view locale, std::string_view key, std::initializer_list<std::string> args = {});

std::string fixed(double value);

}
