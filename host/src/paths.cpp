// SPDX-License-Identifier: MIT
#include "paths.h"
#include "common.h"

#include <mutex>
#include <utility>
#include <vector>

namespace paths {
namespace {
std::mutex g_lock;
std::vector<std::pair<std::string, std::string>> g_map;

std::string strip_trailing_slash(std::string p) {
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}
}  // namespace

void add_mapping(const std::string &from_raw, const std::string &to_raw) {
    std::string from = strip_trailing_slash(from_raw);
    std::string to = strip_trailing_slash(to_raw);
    if (from.empty() || to.empty() || from == to) return;
    std::lock_guard<std::mutex> lk(g_lock);
    for (auto &kv : g_map) {
        if (kv.first == from) {
            kv.second = to;
            return;
        }
    }
    g_map.emplace_back(from, to);
    LOGI("path mapping registered: %s -> %s", from.c_str(), to.c_str());
}

std::string translate(const char *guest_path) {
    if (!guest_path) return {};
    std::string s = guest_path;
    std::lock_guard<std::mutex> lk(g_lock);
    for (const auto &m : g_map) {
        const size_t n = m.first.size();
        if (s.compare(0, n, m.first) == 0 && (s.size() == n || s[n] == '/'))
            return m.second + s.substr(n);
    }
    return s;
}
}  // namespace paths
