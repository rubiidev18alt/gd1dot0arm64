// SPDX-License-Identifier: MIT
#pragma once
#include "cpu.h"

#include <string>
#include <unordered_map>
#include <vector>

struct Module {
    std::string name;
    u32 base = 0;
    u32 bias = 0;
    u32 size = 0;

    u32 init_fn = 0;
    u32 init_array = 0;
    u32 init_arraysz = 0;
    bool initialized = false;

    u32 exidx = 0;
    u32 exidx_count = 0;

    std::unordered_map<std::string, u32> exports;
};

namespace loader {
void register_import(const char *name, ThunkFn fn);
void register_data_import(const char *name, u32 guest_addr);

Module *load(const std::string &path);
Module *load_bytes(const std::string &name, const u8 *data, size_t len);
u32 run_initializers(Module *m);

u32 lookup(const std::string &name, Module *only = nullptr);
Module *find_by_addr(u32 guest_addr);
Module *find_by_name(const std::string &soname);
std::string symbolize(u32 guest_addr);
const std::vector<std::string> &unresolved_symbols();
}  // namespace loader
