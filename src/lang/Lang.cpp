// SPDX-License-Identifier: MIT
#include "lang/Lang.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include "fmt/format.h"
#include "nlohmann/json.hpp"

#include "ll/api/mod/NativeMod.h"

namespace tick_command {

namespace {

using Table = std::unordered_map<std::string, std::string>;

std::unordered_map<std::string, Table> languages;

Table readLanguage(std::filesystem::path const& file) {
    Table         out;
    std::ifstream in(file, std::ios::binary);
    if (!in) return out;
    std::stringstream ss;
    ss << in.rdbuf();
    auto j = nlohmann::json::parse(ss.str(), nullptr, false);
    if (!j.is_object()) return out;
    for (auto& [k, v] : j.items()) {
        if (v.is_string()) out[k] = v.get<std::string>();
    }
    return out;
}

std::string normalize(std::string_view locale) {
    std::string code(locale);
    std::transform(code.begin(), code.end(), code.begin(), [](unsigned char c) {
        return c == '-' ? '_' : static_cast<char>(std::tolower(c));
    });
    if (code == "zh_hk" || code == "zh_mo" || code == "zh_hant") return "zh_tw";
    if (code == "zh" || code == "zh_sg" || code == "zh_hans") return "zh_cn";
    if (code == "ja") return "ja_jp";
    return code;
}

}

void loadLanguages(std::filesystem::path const& dir) {
    for (auto code : {"en_us", "ja_jp", "zh_cn", "zh_tw"}) {
        languages[code] = readLanguage(dir / (std::string(code) + ".json"));
        if (languages[code].empty()) {
            ll::mod::NativeMod::current()->getLogger().warn("Cannot read the language file: {}.json", code);
        }
    }
}

std::string translate(std::string_view locale, std::string_view key, std::initializer_list<std::string> args) {
    std::string const keyText(key);
    std::string       text = keyText;
    auto              find = [&](std::string const& code) {
        auto table = languages.find(code);
        if (table == languages.end()) return false;
        auto entry = table->second.find(keyText);
        if (entry == table->second.end()) return false;
        text = entry->second;
        return true;
    };
    if (!find(normalize(locale))) find("en_us");

    std::string out;
    auto        arg = args.begin();
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 1 < text.size() && text[i + 1] == 's') {
            if (arg != args.end()) out += *arg++;
            ++i;
        } else {
            out += text[i];
        }
    }
    return out;
}

std::string fixed(double value) { return fmt::format("{:.1f}", value); }

}
