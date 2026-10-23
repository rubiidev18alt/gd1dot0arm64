// SPDX-License-Identifier: MIT
#pragma once
#include "common.h"
#include <string>

namespace local_gdps {

// Virtual loopback IPv4 (127.0.0.77) returned by guest DNS lookups for boomlings.com
// when the embedded private 1.0 GDPS server is active; t_connect rewrites 127.0.0.77:*
// to 127.0.0.1:<port()>.
constexpr u32 VIRTUAL_BOOMLINGS_IPV4_HOST_ORDER = 0x7F00004Du;  // 127.0.0.77

bool init_from_bytes(const u8 *data, size_t len, const std::string &state_dir);
bool init_from_file(const std::string &db_path, const std::string &state_dir);

bool is_enabled();
u16 port();
bool is_boomlings_host(const char *host);

u32 level_count();
u32 requests_handled();
u32 blocked_upload_count();

}  // namespace local_gdps
