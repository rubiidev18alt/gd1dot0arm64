// SPDX-License-Identifier: MIT
#pragma once
#include <string>

namespace paths {
void add_mapping(const std::string &from, const std::string &to);
std::string translate(const char *guest_path);
}  // namespace paths
