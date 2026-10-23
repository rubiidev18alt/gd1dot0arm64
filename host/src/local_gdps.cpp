// SPDX-License-Identifier: MIT
#include "local_gdps.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace local_gdps {
namespace {

struct LevelEntry {
    s32 level_id = 0;
    s32 user_id = 0;
    s32 base_downloads = 0;
    s32 base_likes = 0;
    s32 base_ratings = 0;
    s32 base_ratings_sum = 0;
    u16 level_version = 1;
    u8 audio_track = 0;
    u8 length = 0;
    u8 base_difficulty = 0;
    u8 featured = 0;
    u8 demon = 0;
    u8 stars = 0;
    std::string name;
    std::string name_lower;
    std::string desc;
    std::string user;
    std::string user_lower;
    u32 raw_level_len = 0;
    std::vector<u8> comp_level;
};

struct LocalDelta {
    s32 dl_inc = 0;
    s32 like_inc = 0;
    s32 rate_cnt = 0;
    s32 rate_sum = 0;
};

std::mutex g_init_lock;
std::vector<LevelEntry> g_levels;
std::unordered_map<s32, size_t> g_id_to_index;

std::mutex g_state_lock;
std::string g_state_file;
std::unordered_map<s32, LocalDelta> g_deltas;

std::atomic<bool> g_enabled{false};
std::atomic<u16> g_port{0};
std::atomic<u32> g_requests_handled{0};
std::atomic<u32> g_blocked_uploads{0};
int g_listen_fd = -1;

u16 rd_u16_le(const u8 *p) {
    return (u16)(p[0] | (u16(p[1]) << 8));
}
u32 rd_u32_le(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
s32 rd_s32_le(const u8 *p) {
    return (s32)rd_u32_le(p);
}

std::string to_lower_ascii(const std::string &s) {
    std::string out = s;
    for (char &c : out) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return out;
}

std::string sanitize_field(const std::string &s, size_t max_len = 64) {
    std::string out;
    out.reserve(std::min(s.size(), max_len));
    for (char ch : s) {
        if (out.size() >= max_len) break;
        if (ch == ':' || ch == '|' || ch == '#') out.push_back('-');
        else if (ch >= 0x20 && ch <= 0x7E) out.push_back(ch);
    }
    return out;
}

void load_state_locked() {
    g_deltas.clear();
    if (g_state_file.empty()) return;
    FILE *f = fopen(g_state_file.c_str(), "r");
    if (!f) return;
    s32 lid = 0, dl = 0, lk = 0, rc = 0, rs = 0;
    while (fscanf(f, "%d %d %d %d %d", &lid, &dl, &lk, &rc, &rs) == 5) {
        g_deltas[lid] = LocalDelta{dl, lk, rc, rs};
    }
    fclose(f);
}

void save_state_locked() {
    if (g_state_file.empty()) return;
    std::string tmp = g_state_file + ".tmp";
    FILE *f = fopen(tmp.c_str(), "w");
    if (!f) return;
    for (const auto &kv : g_deltas) {
        const auto &d = kv.second;
        if (d.dl_inc || d.like_inc || d.rate_cnt || d.rate_sum) {
            fprintf(f, "%d %d %d %d %d\n", kv.first, d.dl_inc, d.like_inc, d.rate_cnt, d.rate_sum);
        }
    }
    fclose(f);
    rename(tmp.c_str(), g_state_file.c_str());
}

struct EffectiveStats {
    s32 downloads;
    s32 likes;
    s32 ratings;
    s32 ratings_sum;
    u8 difficulty;
};

EffectiveStats get_effective_stats(const LevelEntry &lv) {
    LocalDelta d{};
    {
        std::lock_guard<std::mutex> lk(g_state_lock);
        auto it = g_deltas.find(lv.level_id);
        if (it != g_deltas.end()) d = it->second;
    }
    EffectiveStats s{};
    s.downloads = lv.base_downloads + d.dl_inc;
    s.likes = lv.base_likes + d.like_inc;
    s.ratings = lv.base_ratings + d.rate_cnt;
    s.ratings_sum = lv.base_ratings_sum + d.rate_sum;
    if (s.ratings >= 5 && s.ratings > 0) {
        int avg = s.ratings_sum / s.ratings;
        if (avg < 1) avg = 1;
        if (avg > 5) avg = 5;
        s.difficulty = (u8)avg;
    } else {
        s.difficulty = lv.base_difficulty;
    }
    return s;
}

bool parse_db_bytes(const u8 *data, size_t len) {
    if (!data || len < 16) return false;
    if (memcmp(data, "GD10DB01", 8) != 0) return false;
    u32 ver = rd_u32_le(data + 8);
    u32 cnt = rd_u32_le(data + 12);
    if (ver != 1 || cnt == 0 || cnt > 10000) return false;

    std::vector<LevelEntry> parsed;
    parsed.reserve(cnt);
    std::unordered_map<s32, size_t> id_map;
    id_map.reserve(cnt);

    size_t off = 16;
    for (u32 i = 0; i < cnt; i++) {
        if (off + 48 > len) return false;
        const u8 *p = data + off;
        LevelEntry e;
        e.level_id         = rd_s32_le(p + 0);
        e.user_id          = rd_s32_le(p + 4);
        e.base_downloads   = rd_s32_le(p + 8);
        e.base_likes       = rd_s32_le(p + 12);
        e.base_ratings     = rd_s32_le(p + 16);
        e.base_ratings_sum = rd_s32_le(p + 20);
        e.level_version    = rd_u16_le(p + 24);
        e.audio_track      = p[26];
        e.length           = p[27];
        e.base_difficulty  = p[28];
        e.featured         = p[29];
        e.demon            = p[30];
        e.stars            = p[31];
        u16 name_len       = rd_u16_le(p + 34);
        u16 desc_len       = rd_u16_le(p + 36);
        u16 user_len       = rd_u16_le(p + 38);
        e.raw_level_len    = rd_u32_le(p + 40);
        u32 comp_len       = rd_u32_le(p + 44);
        off += 48;

        size_t var_total = (size_t)name_len + (size_t)desc_len + (size_t)user_len + (size_t)comp_len;
        if (off + var_total > len || e.raw_level_len > 16 * 1024 * 1024) return false;

        e.name.assign((const char *)(data + off), name_len);
        off += name_len;
        e.desc.assign((const char *)(data + off), desc_len);
        off += desc_len;
        e.user.assign((const char *)(data + off), user_len);
        off += user_len;
        e.comp_level.assign(data + off, data + off + comp_len);
        off += comp_len;

        e.name_lower = to_lower_ascii(e.name);
        e.user_lower = to_lower_ascii(e.user);

        id_map[e.level_id] = parsed.size();
        parsed.push_back(std::move(e));
    }

    g_levels = std::move(parsed);
    g_id_to_index = std::move(id_map);
    return true;
}

bool inflate_level_string(const LevelEntry &lv, std::string &out) {
    if (lv.raw_level_len == 0) {
        out.clear();
        return true;
    }
    out.resize(lv.raw_level_len);
    uLongf dest_len = (uLongf)lv.raw_level_len;
    int rc = uncompress((Bytef *)out.data(), &dest_len, lv.comp_level.data(), (uLong)lv.comp_level.size());
    if (rc != Z_OK) return false;
    out.resize((size_t)dest_len);
    return true;
}

std::string url_decode(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        char c = in[i];
        if (c == '+') {
            out.push_back(' ');
        } else if (c == '%' && i + 2 < in.size()) {
            auto hex_val = [](char h) -> int {
                if (h >= '0' && h <= '9') return h - '0';
                if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                return -1;
            };
            int hi = hex_val(in[i + 1]);
            int lo = hex_val(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back((char)((hi << 4) | lo));
                i += 2;
            } else {
                out.push_back(c);
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

void parse_form_kv(const std::string &body, std::unordered_map<std::string, std::string> &out) {
    size_t pos = 0;
    while (pos < body.size()) {
        size_t amp = body.find('&', pos);
        if (amp == std::string::npos) amp = body.size();
        std::string pair = body.substr(pos, amp - pos);
        if (!pair.empty()) {
            size_t eq = pair.find('=');
            if (eq == std::string::npos) {
                out[url_decode(pair)] = "";
            } else {
                out[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
            }
        }
        pos = amp + 1;
    }
}

std::string trim_ws(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

bool is_all_digits(const std::string &s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

std::unordered_set<int> parse_csv_ints(const std::string &s) {
    std::unordered_set<int> res;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t comma = s.find(',', pos);
        if (comma == std::string::npos) comma = s.size();
        std::string tok = trim_ws(s.substr(pos, comma - pos));
        if (!tok.empty() && tok != "-") {
            char *endp = nullptr;
            long v = strtol(tok.c_str(), &endp, 10);
            if (endp && *endp == '\0') res.insert((int)v);
        }
        pos = comma + 1;
    }
    return res;
}

std::string format_level_summary(const LevelEntry &lv, const EffectiveStats &st) {
    std::ostringstream ss;
    ss << "1:" << lv.level_id
       << ":2:" << lv.name
       << ":3:" << lv.desc
       << ":5:" << lv.level_version
       << ":6:" << lv.user_id
       << ":8:" << st.ratings
       << ":9:" << st.ratings_sum
       << ":10:" << st.downloads
       << ":12:" << (int)lv.audio_track
       << ":13:1"
       << ":14:" << st.likes
       << ":15:" << (int)lv.length
       << ":16:0"
       << ":17:" << (int)lv.demon
       << ":18:" << (int)lv.stars
       << ":19:" << (lv.featured > 0 ? 1 : 0);
    return ss.str();
}

std::string format_full_level(const LevelEntry &lv, const EffectiveStats &st, const std::string &level_str) {
    std::ostringstream ss;
    ss << "1:" << lv.level_id
       << ":2:" << lv.name
       << ":3:" << lv.desc
       << ":4:" << level_str
       << ":5:" << lv.level_version
       << ":6:" << lv.user_id
       << ":8:" << st.ratings
       << ":9:" << st.ratings_sum
       << ":10:" << st.downloads
       << ":11:1"
       << ":12:" << (int)lv.audio_track
       << ":13:1"
       << ":14:" << st.likes
       << ":15:" << (int)lv.length
       << ":16:0"
       << ":17:" << (int)lv.demon
       << ":18:" << (int)lv.stars
       << ":19:" << (lv.featured > 0 ? 1 : 0);
    return ss.str();
}

std::string handle_get_levels(const std::unordered_map<std::string, std::string> &params) {
    int type = 0;
    if (auto it = params.find("type"); it != params.end() && !it->second.empty()) {
        type = atoi(it->second.c_str());
    }
    int page = 0;
    if (auto it = params.find("page"); it != params.end() && !it->second.empty()) {
        page = std::max(0, atoi(it->second.c_str()));
    }
    std::string str_q;
    if (auto it = params.find("str"); it != params.end()) {
        str_q = trim_ws(it->second);
    }
    std::string diff_s = "-";
    if (auto it = params.find("diff"); it != params.end() && !it->second.empty()) {
        diff_s = trim_ws(it->second);
    }
    std::string len_s = "-";
    if (auto it = params.find("len"); it != params.end() && !it->second.empty()) {
        len_s = trim_ws(it->second);
    }

    bool filter_diff = (!diff_s.empty() && diff_s != "-");
    std::unordered_set<int> diff_set = filter_diff ? parse_csv_ints(diff_s) : std::unordered_set<int>{};
    // Some 1.0 revisions send 10/20/..50 instead of 1..5; normalize both encodings.
    {
        std::unordered_set<int> normalized;
        for (int v : diff_set) normalized.insert((v >= 10 && v % 10 == 0) ? v / 10 : v);
        diff_set.swap(normalized);
    }

    bool filter_len = (!len_s.empty() && len_s != "-");
    std::unordered_set<int> len_set = filter_len ? parse_csv_ints(len_s) : std::unordered_set<int>{};

    auto passes_diff_and_len = [&](const LevelEntry &lv, const EffectiveStats &st) -> bool {
        if (filter_len && len_set.find((int)lv.length) == len_set.end()) return false;
        if (filter_diff) {
            if (diff_s == "-1") {
                if (st.difficulty != 0) return false;
            } else if (diff_s == "-2") {
                if (lv.demon == 0 && st.difficulty < 5) return false;
            } else {
                bool match = false;
                if (diff_set.count(-1) && st.difficulty == 0) match = true;
                if (st.difficulty > 0 && diff_set.count((int)st.difficulty)) match = true;
                if (!match) return false;
            }
        }
        return true;
    };

    struct Candidate {
        const LevelEntry *lv;
        EffectiveStats st;
    };
    std::vector<Candidate> matches;
    matches.reserve(g_levels.size());

    if (type == 0 && !str_q.empty() && is_all_digits(str_q)) {
        long target_id = strtol(str_q.c_str(), nullptr, 10);
        auto it = g_id_to_index.find((s32)target_id);
        if (it != g_id_to_index.end()) {
            const LevelEntry &lv = g_levels[it->second];
            EffectiveStats st = get_effective_stats(lv);
            if (passes_diff_and_len(lv, st)) {
                matches.push_back(Candidate{&lv, st});
            }
        } else if (target_id < 128 || target_id > 1941) {
            // Outside 1.0 ID range (e.g. searching "1" or "2" for "Level 1" / "Jumper 2"): match name substring.
            std::string q_low = to_lower_ascii(str_q);
            for (const auto &lv : g_levels) {
                if (lv.name_lower.find(q_low) == std::string::npos) continue;
                EffectiveStats st = get_effective_stats(lv);
                if (passes_diff_and_len(lv, st)) matches.push_back(Candidate{&lv, st});
            }
        }
    } else if (type == 5) {
        s32 uid = (s32)strtol(str_q.c_str(), nullptr, 10);
        if (uid > 0) {
            for (const auto &lv : g_levels) {
                if (lv.user_id != uid) continue;
                EffectiveStats st = get_effective_stats(lv);
                if (passes_diff_and_len(lv, st)) matches.push_back(Candidate{&lv, st});
            }
        }
    } else {
        std::string q_low = to_lower_ascii(str_q);
        for (const auto &lv : g_levels) {
            if (type == 0 && !q_low.empty()) {
                if (lv.name_lower.find(q_low) == std::string::npos &&
                    lv.user_lower.find(q_low) == std::string::npos) {
                    continue;
                }
            }
            EffectiveStats st = get_effective_stats(lv);
            if (type == 3 && st.likes <= 0) {
                continue;
            }
            if (passes_diff_and_len(lv, st)) {
                matches.push_back(Candidate{&lv, st});
            }
        }
    }

    switch (type) {
    case 1:  // Most Downloaded
        std::sort(matches.begin(), matches.end(), [](const Candidate &a, const Candidate &b) {
            if (a.st.downloads != b.st.downloads) return a.st.downloads > b.st.downloads;
            if (a.st.likes != b.st.likes) return a.st.likes > b.st.likes;
            return a.lv->level_id < b.lv->level_id;
        });
        break;
    case 3:  // Trending (1.0 archive momentum: newer 1.0 wave + strong like ratio)
        std::sort(matches.begin(), matches.end(), [](const Candidate &a, const Candidate &b) {
            s64 sa = (s64)a.st.likes * 8 + (s64)a.lv->level_id * 3;
            s64 sb = (s64)b.st.likes * 8 + (s64)b.lv->level_id * 3;
            if (sa != sb) return sa > sb;
            return a.lv->level_id > b.lv->level_id;
        });
        break;
    case 4:  // Most Recent (1941 -> 128)
    case 5:  // User's levels
        std::sort(matches.begin(), matches.end(), [](const Candidate &a, const Candidate &b) {
            return a.lv->level_id > b.lv->level_id;
        });
        break;
    case 6:  // Featured / Rated (ordered by feature score, stars, then likes; never empty)
        std::sort(matches.begin(), matches.end(), [](const Candidate &a, const Candidate &b) {
            if (a.lv->featured != b.lv->featured) return a.lv->featured > b.lv->featured;
            if (a.lv->stars != b.lv->stars) return a.lv->stars > b.lv->stars;
            if (a.st.likes != b.st.likes) return a.st.likes > b.st.likes;
            return a.lv->level_id < b.lv->level_id;
        });
        break;
    case 0:  // Search
    case 2:  // Most Liked
    default:
        std::sort(matches.begin(), matches.end(), [](const Candidate &a, const Candidate &b) {
            if (a.st.likes != b.st.likes) return a.st.likes > b.st.likes;
            if (a.st.downloads != b.st.downloads) return a.st.downloads > b.st.downloads;
            return a.lv->level_id < b.lv->level_id;
        });
        break;
    }

    const size_t total = matches.size();
    const size_t offset = (size_t)page * 10;
    if (total == 0 || offset >= total) {
        return "##" + std::to_string(total) + ":" + std::to_string(offset) + ":10";
    }

    const size_t end = std::min(total, offset + 10);
    std::string lvl_part;
    std::string usr_part;
    std::unordered_set<s32> seen_users;
    for (size_t i = offset; i < end; i++) {
        if (!lvl_part.empty()) lvl_part.push_back('|');
        lvl_part += format_level_summary(*matches[i].lv, matches[i].st);
        if (seen_users.insert(matches[i].lv->user_id).second) {
            if (!usr_part.empty()) usr_part.push_back('|');
            usr_part += std::to_string(matches[i].lv->user_id) + ":" + matches[i].lv->user + ":0";
        }
    }

    return lvl_part + "#" + usr_part + "#" + std::to_string(total) + ":" + std::to_string(offset) + ":10";
}

std::string handle_download_level(const std::unordered_map<std::string, std::string> &params) {
    auto it_id = params.find("levelID");
    if (it_id == params.end() || it_id->second.empty()) return "-1";
    s32 lid = (s32)strtol(it_id->second.c_str(), nullptr, 10);
    auto it = g_id_to_index.find(lid);
    if (it == g_id_to_index.end()) return "-1";

    const LevelEntry &lv = g_levels[it->second];
    std::string raw_lvl;
    if (!inflate_level_string(lv, raw_lvl) || raw_lvl.empty()) return "-1";

    if (auto it_inc = params.find("inc"); it_inc != params.end() && it_inc->second == "1") {
        std::lock_guard<std::mutex> lk(g_state_lock);
        auto &d = g_deltas[lid];
        if (d.dl_inc < 1) {
            d.dl_inc = 1;
            save_state_locked();
        }
    }

    EffectiveStats st = get_effective_stats(lv);
    return format_full_level(lv, st, raw_lvl);
}

std::string handle_update_level(const std::unordered_map<std::string, std::string> &params) {
    auto it_id = params.find("levelID");
    if (it_id == params.end() || it_id->second.empty()) return "-1";
    s32 lid = (s32)strtol(it_id->second.c_str(), nullptr, 10);
    auto it = g_id_to_index.find(lid);
    if (it == g_id_to_index.end()) return "-1";

    const LevelEntry &lv = g_levels[it->second];
    if (auto it_ver = params.find("levelVersion"); it_ver != params.end() && !it_ver->second.empty()) {
        int req_ver = atoi(it_ver->second.c_str());
        if (req_ver >= (int)lv.level_version) return "-1";
    }

    std::string raw_lvl;
    if (!inflate_level_string(lv, raw_lvl) || raw_lvl.empty()) return "-1";
    EffectiveStats st = get_effective_stats(lv);
    return format_full_level(lv, st, raw_lvl);
}

std::string handle_like_level(const std::unordered_map<std::string, std::string> &params) {
    auto it_id = params.find("levelID");
    if (it_id == params.end() || it_id->second.empty()) return "-1";
    s32 lid = (s32)strtol(it_id->second.c_str(), nullptr, 10);
    if (g_id_to_index.find(lid) == g_id_to_index.end()) return "-1";

    std::lock_guard<std::mutex> lk(g_state_lock);
    auto &d = g_deltas[lid];
    if (d.like_inc == 0) {
        d.like_inc = 1;
        save_state_locked();
    }
    return "1";
}

std::string handle_rate_level(const std::unordered_map<std::string, std::string> &params) {
    auto it_id = params.find("levelID");
    auto it_rt = params.find("rating");
    if (it_id == params.end() || it_rt == params.end()) return "-1";
    s32 lid = (s32)strtol(it_id->second.c_str(), nullptr, 10);
    int rating = atoi(it_rt->second.c_str());
    if (rating < 1 || rating > 5) return "-1";
    if (g_id_to_index.find(lid) == g_id_to_index.end()) return "-1";

    std::lock_guard<std::mutex> lk(g_state_lock);
    auto &d = g_deltas[lid];
    if (d.rate_cnt == 0) {
        d.rate_cnt = 1;
        d.rate_sum = rating;
        save_state_locked();
    }
    return "1";
}

std::string route_endpoint(const std::string &raw_target, const std::string &post_body) {
    g_requests_handled.fetch_add(1, std::memory_order_relaxed);

    std::string path = raw_target;
    std::string query;
    if (size_t qpos = path.find('?'); qpos != std::string::npos) {
        query = path.substr(qpos + 1);
        path = path.substr(0, qpos);
    }

    std::string script = path;
    if (size_t slash = script.rfind('/'); slash != std::string::npos) {
        script = script.substr(slash + 1);
    }
    std::string script_low = to_lower_ascii(script);

    // Block ALL level/comment upload and delete endpoints strictly!
    if (script_low.find("upload") != std::string::npos ||
        script_low.find("delete") != std::string::npos) {
        u32 blocked = g_blocked_uploads.fetch_add(1, std::memory_order_relaxed) + 1;
        LOGI("local_gdps: blocked upload/delete endpoint '%s' (total blocked=%u)",
             script.c_str(), blocked);
        return "-1";
    }

    std::unordered_map<std::string, std::string> params;
    if (!query.empty()) parse_form_kv(query, params);
    if (!post_body.empty()) parse_form_kv(post_body, params);

    if (script_low == "getgjlevels.php") {
        return handle_get_levels(params);
    }
    if (script_low == "downloadgjlevel.php") {
        return handle_download_level(params);
    }
    if (script_low == "updategjlevel.php") {
        return handle_update_level(params);
    }
    if (script_low == "likegjlevel.php" || script_low == "likegjitem.php") {
        return handle_like_level(params);
    }
    if (script_low == "rategjlevel.php") {
        return handle_rate_level(params);
    }
    if (script_low == "updategjusername.php") {
        auto it = params.find("userName");
        std::string uname = it != params.end() ? sanitize_field(it->second, 24) : "Player";
        return uname.empty() ? "Player" : uname;
    }
    if (script_low == "checkmoregames.php" || script_low == "checkmoregamesandroid.php") {
        return "0";
    }
    return "-1";
}

bool send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        ssize_t n = send(fd, buf + sent, len - sent, flags);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

void serve_client(int cfd) {
    struct timeval tv{};
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    std::string req;
    req.reserve(2048);
    char tmp[4096];
    size_t hdr_end = std::string::npos;

    while (req.size() < 65536) {
        ssize_t n = recv(cfd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        req.append(tmp, (size_t)n);
        hdr_end = req.find("\r\n\r\n");
        if (hdr_end != std::string::npos) break;
    }

    if (hdr_end == std::string::npos) {
        close(cfd);
        return;
    }

    const size_t body_offset = hdr_end + 4;
    std::string headers_str = req.substr(0, hdr_end);
    std::string headers_low = to_lower_ascii(headers_str);

    // Parse Request-Line: METHOD SP TARGET SP HTTP/1.x
    size_t line_end = headers_str.find("\r\n");
    std::string req_line = line_end == std::string::npos ? headers_str : headers_str.substr(0, line_end);
    size_t sp1 = req_line.find(' ');
    size_t sp2 = sp1 == std::string::npos ? std::string::npos : req_line.find(' ', sp1 + 1);
    std::string target = (sp1 != std::string::npos && sp2 != std::string::npos)
                             ? req_line.substr(sp1 + 1, sp2 - sp1 - 1)
                             : "/";

    // Parse Content-Length and Expect: 100-continue
    size_t content_len = 0;
    if (size_t cl_pos = headers_low.find("\r\ncontent-length:"); cl_pos != std::string::npos) {
        const char *p = headers_low.c_str() + cl_pos + 17;
        while (*p == ' ' || *p == '\t') p++;
        long v = strtol(p, nullptr, 10);
        if (v > 0 && v < 8 * 1024 * 1024) content_len = (size_t)v;
    }

    if (headers_low.find("expect: 100-continue") != std::string::npos) {
        static const char k100[] = "HTTP/1.1 100 Continue\r\n\r\n";
        send_all(cfd, k100, sizeof(k100) - 1);
    }

    std::string body = req.substr(body_offset);
    while (body.size() < content_len) {
        size_t need = std::min(sizeof(tmp), content_len - body.size());
        ssize_t n = recv(cfd, tmp, need, 0);
        if (n <= 0) break;
        body.append(tmp, (size_t)n);
    }
    if (body.size() > content_len && content_len > 0) {
        body.resize(content_len);
    }

    std::string resp_body = route_endpoint(target, body);
    std::ostringstream resp;
    resp << "HTTP/1.1 200 OK\r\n"
         << "Server: gd1dot0arm64-local-gdps/1.0\r\n"
         << "Content-Type: text/html; charset=utf-8\r\n"
         << "Content-Length: " << resp_body.size() << "\r\n"
         << "Connection: close\r\n\r\n"
         << resp_body;
    std::string wire = resp.str();
    send_all(cfd, wire.data(), wire.size());
    shutdown(cfd, SHUT_WR);
    close(cfd);
}

bool start_loopback_listener_locked() {
    if (g_enabled.load(std::memory_order_acquire)) return true;

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) return false;

    int one = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin.sin_port = htons(0);
    if (bind(sfd, (const struct sockaddr *)&sin, sizeof(sin)) != 0) {
        close(sfd);
        return false;
    }
    if (listen(sfd, 64) != 0) {
        close(sfd);
        return false;
    }

    struct sockaddr_in bound{};
    socklen_t blen = sizeof(bound);
    if (getsockname(sfd, (struct sockaddr *)&bound, &blen) != 0) {
        close(sfd);
        return false;
    }

    u16 chosen_port = ntohs(bound.sin_port);
    g_listen_fd = sfd;
    g_port.store(chosen_port, std::memory_order_release);
    g_enabled.store(true, std::memory_order_release);

    std::thread([sfd]() {
        while (true) {
            struct sockaddr_in cli{};
            socklen_t clen = sizeof(cli);
            int cfd = accept(sfd, (struct sockaddr *)&cli, &clen);
            if (cfd < 0) {
                if (errno == EINTR) continue;
                usleep(10000);
                continue;
            }
            std::thread(serve_client, cfd).detach();
        }
    }).detach();

    LOGI("local_gdps: started embedded GD 1.0 archive server on 127.0.0.1:%u (%zu levels, uploads BLOCKED)",
         (unsigned)chosen_port, g_levels.size());
    return true;
}

}  // namespace

bool init_from_bytes(const u8 *data, size_t len, const std::string &state_dir) {
    std::lock_guard<std::mutex> lk(g_init_lock);
    if (g_enabled.load(std::memory_order_acquire)) return true;
    if (!parse_db_bytes(data, len)) {
        LOGE("local_gdps: failed to parse GD10DB01 database (%zu bytes)", len);
        return false;
    }
    {
        std::lock_guard<std::mutex> slk(g_state_lock);
        g_state_file = state_dir.empty() ? "" : (state_dir + "/gdps_local_state.txt");
        load_state_locked();
    }
    return start_loopback_listener_locked();
}

bool init_from_file(const std::string &db_path, const std::string &state_dir) {
    FILE *f = fopen(db_path.c_str(), "rb");
    if (!f) {
        LOGE("local_gdps: cannot open %s", db_path.c_str());
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 16) {
        fclose(f);
        return false;
    }
    std::vector<u8> buf((size_t)sz);
    size_t nr = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    if (nr != buf.size()) return false;
    return init_from_bytes(buf.data(), buf.size(), state_dir);
}

bool is_enabled() {
    return g_enabled.load(std::memory_order_acquire);
}

u16 port() {
    return g_port.load(std::memory_order_acquire);
}

bool is_boomlings_host(const char *host) {
    if (!host || !*host) return false;
    std::string h = to_lower_ascii(host);
    while (!h.empty() && h.back() == '.') h.pop_back();
    return h == "www.boomlings.com" || h == "boomlings.com";
}

u32 level_count() {
    return (u32)g_levels.size();
}

u32 requests_handled() {
    return g_requests_handled.load(std::memory_order_acquire);
}

u32 blocked_upload_count() {
    return g_blocked_uploads.load(std::memory_order_acquire);
}

}  // namespace local_gdps
