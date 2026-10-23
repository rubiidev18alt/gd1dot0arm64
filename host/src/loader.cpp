// SPDX-License-Identifier: MIT
#include "loader.h"

#include <algorithm>
#include <elf.h>
#include <fstream>

#ifndef PT_ARM_EXIDX
#define PT_ARM_EXIDX 0x70000001
#endif
#ifndef R_ARM_NONE
#define R_ARM_NONE 0
#define R_ARM_ABS32 2
#define R_ARM_REL32 3
#define R_ARM_COPY 20
#define R_ARM_GLOB_DAT 21
#define R_ARM_JUMP_SLOT 22
#define R_ARM_RELATIVE 23
#endif

namespace loader {
namespace {
std::unordered_map<std::string, u32> g_imports;
std::vector<Module *> g_modules;
std::vector<std::string> g_unresolved;

u32 missing_stub(const std::string &sym) {
    char label[256];
    snprintf(label, sizeof(label), "UNRESOLVED:%s", sym.c_str());
    return cpu::add_thunk(label, [](GuestThread &t) {
        cpu::dump_state(t);
        fatal("called unresolved symbol (%s)", cpu::thunk_name(t.cur_svc));
    });
}

size_t elf_nsyms(const u8 *elf, size_t elf_len, const Elf32_Ehdr *eh, u32 symtab_va, u32 bias,
                 u32 hash_va, u32 gnu_hash_va) {
    if (hash_va) return rd<u32>(hash_va + 4);
    if (eh->e_shoff && (size_t)eh->e_shoff + (size_t)eh->e_shnum * sizeof(Elf32_Shdr) <= elf_len) {
        auto *sh = (const Elf32_Shdr *)(elf + eh->e_shoff);
        for (int i = 0; i < eh->e_shnum; i++)
            if (sh[i].sh_type == SHT_DYNSYM) return sh[i].sh_size / sizeof(Elf32_Sym);
    }
    if (gnu_hash_va) {
        u32 nbuckets = rd<u32>(gnu_hash_va);
        u32 symoffset = rd<u32>(gnu_hash_va + 4);
        u32 bloom_size = rd<u32>(gnu_hash_va + 8);
        u32 buckets = gnu_hash_va + 16 + bloom_size * 4;
        u32 chains = buckets + nbuckets * 4;
        u32 max_sym = symoffset;
        for (u32 i = 0; i < nbuckets; i++) {
            u32 b = rd<u32>(buckets + i * 4);
            if (b > max_sym) max_sym = b;
        }
        if (max_sym >= symoffset) {
            while ((rd<u32>(chains + (max_sym - symoffset) * 4) & 1u) == 0) max_sym++;
            return max_sym + 1;
        }
        return symoffset;
    }
    (void)symtab_va;
    (void)bias;
    return 0;
}
}  // namespace

void register_import(const char *name, ThunkFn fn) { g_imports[name] = cpu::add_thunk(name, fn); }
void register_data_import(const char *name, u32 addr) { g_imports[name] = addr; }
const std::vector<std::string> &unresolved_symbols() { return g_unresolved; }

u32 lookup(const std::string &name, Module *only) {
    if (only) {
        auto it = only->exports.find(name);
        return it == only->exports.end() ? 0 : it->second;
    }
    for (Module *m : g_modules) {
        auto it = m->exports.find(name);
        if (it != m->exports.end()) return it->second;
    }
    auto it = g_imports.find(name);
    return it == g_imports.end() ? 0 : it->second;
}

Module *find_by_addr(u32 a) {
    for (Module *m : g_modules)
        if (a >= m->base && a < m->base + m->size) return m;
    return nullptr;
}

Module *find_by_name(const std::string &soname) {
    for (Module *m : g_modules)
        if (m->name == soname) return m;
    return nullptr;
}

std::string symbolize(u32 a) {
    char buf[384];
    if (a >= GUEST_THUNK_BASE && a < GUEST_THUNK_BASE + GUEST_THUNK_MAX * 4) {
        snprintf(buf, sizeof(buf), "0x%08x [thunk %s]", a, cpu::thunk_name((a - GUEST_THUNK_BASE) / 4));
        return buf;
    }
    Module *m = find_by_addr(a);
    if (!m) {
        snprintf(buf, sizeof(buf), "0x%08x", a);
        return buf;
    }
    const char *best_name = nullptr;
    u32 best_addr = 0;
    u32 clean = a & ~1u;
    for (auto &[n, sa] : m->exports) {
        u32 sc = sa & ~1u;
        if (sc <= clean && sc >= best_addr) {
            best_addr = sc;
            best_name = n.c_str();
        }
    }
    if (best_name && clean - best_addr < 0x10000)
        snprintf(buf, sizeof(buf), "0x%08x (%s!%s+0x%x [off 0x%x])", a, m->name.c_str(), best_name,
                 clean - best_addr, a - m->bias);
    else
        snprintf(buf, sizeof(buf), "0x%08x (%s+0x%x)", a, m->name.c_str(), a - m->bias);
    return buf;
}

Module *load(const std::string &path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) fatal("cannot open ELF32 file %s", path.c_str());
    size_t sz = (size_t)f.tellg();
    std::vector<u8> elf(sz);
    f.seekg(0);
    f.read((char *)elf.data(), (std::streamsize)sz);

    auto slash = path.find_last_of('/');
    std::string soname = slash == std::string::npos ? path : path.substr(slash + 1);
    return load_bytes(soname, elf.data(), elf.size());
}

Module *load_bytes(const std::string &soname, const u8 *elf, size_t sz) {
    if (sz < sizeof(Elf32_Ehdr)) fatal("%s: truncated ELF header", soname.c_str());
    auto *eh = (const Elf32_Ehdr *)elf;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS32 || eh->e_machine != EM_ARM)
        fatal("%s: not a 32-bit ARM ELF", soname.c_str());

    if ((size_t)eh->e_phoff + (size_t)eh->e_phnum * sizeof(Elf32_Phdr) > sz)
        fatal("%s: program header table out of bounds", soname.c_str());
    auto *ph = (const Elf32_Phdr *)(elf + eh->e_phoff);

    u32 lo = ~0u, hi = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        lo = std::min(lo, ph[i].p_vaddr & ~0xFFFu);
        hi = std::max(hi, (ph[i].p_vaddr + ph[i].p_memsz + 0xFFF) & ~0xFFFu);
    }
    if (hi <= lo) fatal("%s: no PT_LOAD segments", soname.c_str());

    auto *m = new Module;
    m->name = soname;
    m->size = hi - lo;
    m->base = mem::map(m->size);
    m->bias = m->base - lo;

    u32 dyn_va = 0, dyn_sz = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) {
            if ((size_t)ph[i].p_offset + (size_t)ph[i].p_filesz > sz)
                fatal("%s: PT_LOAD segment out of file bounds", soname.c_str());
            memset(g2h(m->bias + ph[i].p_vaddr), 0, ph[i].p_memsz);
            memcpy(g2h(m->bias + ph[i].p_vaddr), elf + ph[i].p_offset, ph[i].p_filesz);
        } else if (ph[i].p_type == PT_DYNAMIC) {
            dyn_va = m->bias + ph[i].p_vaddr;
            dyn_sz = ph[i].p_memsz;
        } else if (ph[i].p_type == PT_ARM_EXIDX) {
            m->exidx = m->bias + ph[i].p_vaddr;
            m->exidx_count = ph[i].p_memsz / 8;
        }
    }

    u32 strtab = 0, symtab = 0, hash = 0, gnu_hash = 0;
    u32 rel = 0, relsz = 0, jmprel = 0, pltrelsz = 0;
    for (u32 off = 0; off + 8 <= dyn_sz; off += 8) {
        s32 tag = rd<s32>(dyn_va + off);
        u32 val = rd<u32>(dyn_va + off + 4);
        if (tag == DT_NULL) break;
        switch (tag) {
        case DT_STRTAB:       strtab = m->bias + val; break;
        case DT_SYMTAB:       symtab = m->bias + val; break;
        case DT_HASH:         hash = m->bias + val; break;
        case DT_GNU_HASH:     gnu_hash = m->bias + val; break;
        case DT_REL:          rel = m->bias + val; break;
        case DT_RELSZ:        relsz = val; break;
        case DT_JMPREL:       jmprel = m->bias + val; break;
        case DT_PLTRELSZ:     pltrelsz = val; break;
        case DT_INIT:         m->init_fn = m->bias + val; break;
        case DT_INIT_ARRAY:   m->init_array = m->bias + val; break;
        case DT_INIT_ARRAYSZ: m->init_arraysz = val; break;
        }
    }

    size_t nsyms = elf_nsyms(elf, sz, eh, symtab, m->bias, hash, gnu_hash);
    auto *syms = (const Elf32_Sym *)g2h(symtab);
    const char *strs = (const char *)g2h(strtab);

    for (size_t i = 1; i < nsyms; i++) {
        const Elf32_Sym &s = syms[i];
        if (s.st_shndx == SHN_UNDEF || !s.st_value) continue;
        u8 bind = ELF32_ST_BIND(s.st_info);
        if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
        const char *nm = strs + s.st_name;
        if (*nm) m->exports[nm] = m->bias + s.st_value;
    }

    g_modules.push_back(m);

    auto resolve = [&](u32 sym_idx) -> u32 {
        if (!sym_idx) return 0;
        const Elf32_Sym &s = syms[sym_idx];
        const char *nm = strs + s.st_name;
        u8 bind = ELF32_ST_BIND(s.st_info);
        if (s.st_shndx != SHN_UNDEF && bind == STB_LOCAL) return m->bias + s.st_value;
        u32 a = lookup(nm);
        if (a) return a;
        if (s.st_shndx != SHN_UNDEF) return m->bias + s.st_value;
        if (bind == STB_WEAK) return 0;
        LOGW("%s: unresolved import %s", m->name.c_str(), nm);
        g_unresolved.emplace_back(nm);
        u32 stub = missing_stub(nm);
        g_imports[nm] = stub;
        return stub;
    };

    auto apply_rel = [&](u32 table, u32 bytes) {
        for (u32 off = 0; off + 8 <= bytes; off += 8) {
            u32 r_offset = rd<u32>(table + off);
            u32 r_info = rd<u32>(table + off + 4);
            u32 where = m->bias + r_offset;
            u32 type = ELF32_R_TYPE(r_info);
            u32 sym = ELF32_R_SYM(r_info);
            switch (type) {
            case R_ARM_NONE:
                break;
            case R_ARM_RELATIVE:
                wr<u32>(where, m->bias + rd<u32>(where));
                break;
            case R_ARM_GLOB_DAT:
            case R_ARM_JUMP_SLOT:
                wr<u32>(where, resolve(sym));
                break;
            case R_ARM_ABS32: {
                u32 s = resolve(sym);
                wr<u32>(where, s ? s + rd<u32>(where) : 0);
                break;
            }
            case R_ARM_REL32: {
                u32 s = resolve(sym);
                wr<u32>(where, s + rd<u32>(where) - where);
                break;
            }
            case R_ARM_COPY: {
                const Elf32_Sym &ss = syms[sym];
                const char *nm = strs + ss.st_name;
                u32 src = 0;
                for (Module *other : g_modules) {
                    if (other == m) continue;
                    auto it = other->exports.find(nm);
                    if (it != other->exports.end()) {
                        src = it->second;
                        break;
                    }
                }
                if (!src) {
                    auto it = g_imports.find(nm);
                    if (it != g_imports.end()) src = it->second;
                }
                if (src) memcpy(g2h(where), g2h(src), ss.st_size);
                else LOGW("%s: R_ARM_COPY unresolved %s", m->name.c_str(), nm);
                break;
            }
            default:
                fatal("%s: unhandled relocation type %u at 0x%x", m->name.c_str(), type, r_offset);
            }
        }
    };

    if (rel) apply_rel(rel, relsz);
    if (jmprel) apply_rel(jmprel, pltrelsz);

    LOGI("loaded %s @ 0x%08x..0x%08x (bias 0x%08x, %zu exports)", m->name.c_str(), m->base, m->base + m->size,
         m->bias, m->exports.size());
    return m;
}

u32 run_initializers(Module *m) {
    if (m->initialized) return 0;
    m->initialized = true;
    u32 count = 0;
    if (m->init_fn) {
        cpu::call(m->init_fn, {});
        count++;
    }
    for (u32 off = 0; off + 4 <= m->init_arraysz; off += 4) {
        u32 fn = rd<u32>(m->init_array + off);
        if (fn && fn != ~0u) {
            cpu::call(fn, {});
            count++;
        }
    }
    LOGI("initialized %s (%u constructors executed)", m->name.c_str(), count);
    return count;
}
}  // namespace loader
