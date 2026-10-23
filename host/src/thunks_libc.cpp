// SPDX-License-Identifier: MIT
#include "abi.h"
#include "paths.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include <wchar.h>
#include <wctype.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {
GuestThread &T() { return cur_thread(); }
int fail(int e = errno) {
    T().set_errno(e);
    return -1;
}
template <class R> R check(R r) {
    if (r == (R)-1) T().set_errno(errno);
    return r;
}

int write_guest_log(int prio, const char *tag, const char *msg) {
#ifdef __ANDROID__
    return __android_log_write(prio, tag ? tag : "gd102-guest", msg ? msg : "");
#else
    (void)prio;
    fprintf(stderr, "[guest/%s] %s\n", tag ? tag : "gd102", msg ? msg : "");
    return 0;
#endif
}

// ---------------------------------------------------------------- stdio
constexpr u32 GFILE_SIZE = 84;
constexpr u16 SEOF = 0x0020, SERR = 0x0040;
std::mutex g_files_lock;
std::unordered_map<u32, FILE *> g_files;
u32 g_sF = 0;

struct LogStream {
    std::mutex lock;
    std::string buf;
    int prio;
    void write(const char *p, size_t n) {
        std::lock_guard<std::mutex> lk(lock);
        for (size_t i = 0; i < n; i++) {
            if (p[i] == '\n') flush_locked();
            else buf += p[i];
        }
        if (buf.size() > 900) flush_locked();
    }
    void flush_locked() {
        if (!buf.empty()) write_guest_log(prio, "gd102-stdio", buf.c_str());
        buf.clear();
    }
};
LogStream g_out{.prio = ANDROID_LOG_INFO}, g_err{.prio = ANDROID_LOG_WARN};

int std_index(u32 gf) {
    if (g_sF && gf >= g_sF && gf < g_sF + 3 * GFILE_SIZE) return (int)((gf - g_sF) / GFILE_SIZE);
    return -1;
}
FILE *hfile(u32 gf) {
    int si = std_index(gf);
    if (si == 0) return stdin;
    std::lock_guard<std::mutex> lk(g_files_lock);
    auto it = g_files.find(gf);
    return it == g_files.end() ? nullptr : it->second;
}
void sync_flags(u32 gf, FILE *f) {
    u16 fl = rd<u16>(gf + 12) & ~(SEOF | SERR);
    if (feof(f)) fl |= SEOF;
    if (ferror(f)) fl |= SERR;
    wr<u16>(gf + 12, fl);
}

u32 guest_file(FILE *f) {
    if (!f) return fail(), 0;
    u32 gf = mem::halloc(GFILE_SIZE);
    wr<s16>(gf + 14, (s16)fileno(f));
    std::lock_guard<std::mutex> lk(g_files_lock);
    g_files[gf] = f;
    return gf;
}

u32 t_fopen(G<const char> path, G<const char> mode) {
    std::string p = paths::translate(path);
    FILE *f = fopen(p.c_str(), mode);
    if (!f) return fail(), 0;
    return guest_file(f);
}

int t_fclose(u32 gf) {
    if (std_index(gf) >= 0) return 0;
    FILE *f;
    {
        std::lock_guard<std::mutex> lk(g_files_lock);
        auto it = g_files.find(gf);
        if (it == g_files.end()) return fail(EBADF);
        f = it->second;
        g_files.erase(it);
    }
    mem::hfree(gf);
    return check(fclose(f));
}

u32 t_fread(G<void> buf, u32 size, u32 n, u32 gf) {
    FILE *f = hfile(gf);
    if (!f) return fail(EBADF), 0;
    size_t r = fread(buf, size, n, f);
    sync_flags(gf, f);
    return (u32)r;
}

u32 t_fwrite(G<const void> buf, u32 size, u32 n, u32 gf) {
    int si = std_index(gf);
    if (si == 1 || si == 2) {
        (si == 1 ? g_out : g_err).write((const char *)buf.get(), (size_t)size * n);
        return n;
    }
    FILE *f = hfile(gf);
    if (!f) return fail(EBADF), 0;
    size_t r = fwrite(buf, size, n, f);
    sync_flags(gf, f);
    return (u32)r;
}

int t_fputs(G<const char> s, u32 gf) {
    size_t n = strlen(s);
    return t_fwrite(G<const void>{s.a}, 1, (u32)n, gf) == n ? 1 : -1;
}

int t_fputc(int c, u32 gf) {
    char ch = (char)c;
    int si = std_index(gf);
    if (si == 1 || si == 2) {
        (si == 1 ? g_out : g_err).write(&ch, 1);
        return (u8)c;
    }
    FILE *f = hfile(gf);
    if (!f) return fail(EBADF);
    return fputc(c, f);
}

int t_fgetc(u32 gf) {
    FILE *f = hfile(gf);
    if (!f) return fail(EBADF);
    int c = fgetc(f);
    sync_flags(gf, f);
    return c;
}

int t_ungetc(int c, u32 gf) {
    FILE *f = hfile(gf);
    if (!f) return -1;
    int r = ungetc(c, f);
    sync_flags(gf, f);
    return r;
}

u32 t_fgets(G<char> buf, int n, u32 gf) {
    FILE *f = hfile(gf);
    if (!f) return 0;
    char *r = fgets(buf, n, f);
    sync_flags(gf, f);
    return r ? buf.a : 0;
}

int t_fflush(u32 gf) {
    if (!gf) return 0;
    if (std_index(gf) >= 0) return 0;
    FILE *f = hfile(gf);
    return f ? fflush(f) : 0;
}

int t_fseek(u32 gf, s32 off, int whence) {
    FILE *f = hfile(gf);
    if (!f) return fail(EBADF);
    int r = check(fseeko(f, off, whence));
    sync_flags(gf, f);
    return r;
}

s32 t_ftell(u32 gf) {
    FILE *f = hfile(gf);
    if (!f) return fail(EBADF);
    return (s32)check(ftello(f));
}

void t_rewind(u32 gf) {
    if (FILE *f = hfile(gf)) {
        rewind(f);
        sync_flags(gf, f);
    }
}

int t_feof(u32 gf) { FILE *f = hfile(gf); return f ? feof(f) : 0; }
int t_ferror(u32 gf) { FILE *f = hfile(gf); return f ? ferror(f) : 0; }
int t_fileno(u32 gf) { FILE *f = hfile(gf); return f ? fileno(f) : fail(EBADF); }
int t_setvbuf(u32, u32, int, u32) { return 0; }
void t_clearerr(u32 gf) {
    if (FILE *f = hfile(gf)) {
        clearerr(f);
        sync_flags(gf, f);
    }
}
void t_setbuf(u32, u32) {}
u32 t_fdopen(int fd, G<const char> mode) { return guest_file(fdopen(fd, mode)); }
u32 t_freopen(G<const char> path, G<const char> mode, u32 gf) {
    FILE *f = hfile(gf);
    if (!f) return fail(EBADF), 0;
    std::string p = path.a ? paths::translate(path) : std::string();
    if (!freopen(path.a ? p.c_str() : nullptr, mode, f)) return fail(), 0;
    wr<s16>(gf + 14, (s16)fileno(f));
    return gf;
}
u32 t_tmpfile() { return guest_file(tmpfile()); }
u32 t_tmpnam(u32 buf) {
    thread_local u32 own = mem::halloc(64);
    static std::atomic<int> n{0};
    u32 out = buf ? buf : own;
    snprintf((char *)g2h(out), 20, "/data/local/tmp/t%d", n++);
    return out;
}
u32 t_popen(G<const char> cmd, G<const char> mode) { return guest_file(popen(cmd, mode)); }
int t_pclose(u32 gf) {
    FILE *f;
    {
        std::lock_guard<std::mutex> lk(g_files_lock);
        auto it = g_files.find(gf);
        if (it == g_files.end()) return fail(EBADF);
        f = it->second;
        g_files.erase(it);
    }
    mem::hfree(gf);
    return pclose(f);
}

// ---------------------------------------------------------------- fd & filesystem I/O
int xlate_open_flags(int g) {
    constexpr int A32_DIRECTORY = 040000, A32_NOFOLLOW = 0100000, A32_DIRECT = 0200000, A32_LARGEFILE = 0400000;
    int h = g & ~(A32_DIRECTORY | A32_NOFOLLOW | A32_DIRECT | A32_LARGEFILE);
    if (g & A32_DIRECTORY) h |= O_DIRECTORY;
    if (g & A32_NOFOLLOW) h |= O_NOFOLLOW;
#ifdef O_DIRECT
    if (g & A32_DIRECT) h |= O_DIRECT;
#endif
    return h;
}
int xlate_open_flags_back(int h) {
    int g = h & ~(O_DIRECTORY | O_NOFOLLOW);
#ifdef O_DIRECT
    g &= ~O_DIRECT;
#endif
#ifdef O_LARGEFILE
    g &= ~O_LARGEFILE;
#endif
    if (h & O_DIRECTORY) g |= 040000;
    if (h & O_NOFOLLOW) g |= 0100000;
#ifdef O_DIRECT
    if (h & O_DIRECT) g |= 0200000;
#endif
    return g;
}

void t_open(GuestThread &t) {
    std::string p = paths::translate((const char *)g2h(t.r(0)));
    int flags = xlate_open_flags((int)t.r(1));
    int r = open(p.c_str(), flags, (mode_t)t.r(2));
    if (r < 0) r = fail();
    t.ret32((u32)r);
}

s32 t_read(int fd, G<void> buf, u32 n) { return (s32)check(read(fd, buf, n)); }
s32 t_write(int fd, G<const void> buf, u32 n) {
    if (fd == 1 || fd == 2) {
        (fd == 1 ? g_out : g_err).write((const char *)buf.get(), n);
        return (s32)n;
    }
    return (s32)check(write(fd, buf, n));
}
int t_close(int fd) { return check(close(fd)); }
s32 t_lseek(int fd, s32 off, int whence) {
    off_t r = lseek(fd, off, whence);
    return r < 0 ? fail() : (s32)r;
}
s64 t_lseek64(int fd, s64 off, int whence) {
    off_t r = lseek(fd, off, whence);
    return r < 0 ? (s64)fail() : (s64)r;
}
int t_fsync(int fd) { return check(fsync(fd)); }
int t_ftruncate(int fd, s32 len) { return check(ftruncate(fd, len)); }

void t_fcntl(GuestThread &t) {
    int fd = (int)t.r(0), cmd = (int)t.r(1);
    u32 arg = t.r(2);
    int r;
    switch (cmd) {
    case F_GETFL: r = fcntl(fd, F_GETFL); if (r >= 0) r = xlate_open_flags_back(r); break;
    case F_SETFL: r = fcntl(fd, F_SETFL, xlate_open_flags((int)arg)); break;
    case F_GETFD: case F_SETFD: case F_DUPFD: r = fcntl(fd, cmd, (int)arg); break;
    default:
        r = -1;
        errno = EINVAL;
    }
    t.ret32((u32)(r < 0 ? fail() : r));
}

void t_ioctl(GuestThread &t) {
    int fd = (int)t.r(0);
    u32 req = t.r(1), arg = t.r(2);
    int r;
    switch (req) {
    case FIONREAD:
    case FIONBIO:
        r = ioctl(fd, req, g2h(arg));
        break;
    default:
        r = -1;
        errno = ENOTTY;
    }
    t.ret32((u32)(r < 0 ? fail() : r));
}

// Bionic LP32 struct stat (104 bytes)
void put_stat(u32 g, const struct stat &s) {
    memset(g2h(g), 0, 104);
    wr<u64>(g + 0, s.st_dev);
    wr<u32>(g + 12, (u32)s.st_ino);
    wr<u32>(g + 16, s.st_mode);
    wr<u32>(g + 20, (u32)s.st_nlink);
    wr<u32>(g + 24, s.st_uid);
    wr<u32>(g + 28, s.st_gid);
    wr<u64>(g + 32, s.st_rdev);
    wr<s64>(g + 48, s.st_size);
    wr<u32>(g + 56, (u32)s.st_blksize);
    wr<u64>(g + 64, (u64)s.st_blocks);
    wr<u32>(g + 72, (u32)s.st_atim.tv_sec);
    wr<u32>(g + 76, (u32)s.st_atim.tv_nsec);
    wr<u32>(g + 80, (u32)s.st_mtim.tv_sec);
    wr<u32>(g + 84, (u32)s.st_mtim.tv_nsec);
    wr<u32>(g + 88, (u32)s.st_ctim.tv_sec);
    wr<u32>(g + 92, (u32)s.st_ctim.tv_nsec);
    wr<u64>(g + 96, s.st_ino);
}
int t_stat(G<const char> path, u32 out) {
    struct stat s;
    std::string p = paths::translate(path);
    if (stat(p.c_str(), &s) < 0) return fail();
    put_stat(out, s);
    return 0;
}
int t_lstat(G<const char> path, u32 out) {
    struct stat s;
    std::string p = paths::translate(path);
    if (lstat(p.c_str(), &s) < 0) return fail();
    put_stat(out, s);
    return 0;
}
int t_fstat(int fd, u32 out) {
    struct stat s;
    if (fstat(fd, &s) < 0) return fail();
    put_stat(out, s);
    return 0;
}

int t_access(G<const char> path, int mode) { return check(access(paths::translate(path).c_str(), mode)); }
int t_mkdir(G<const char> path, u32 mode) { return check(mkdir(paths::translate(path).c_str(), mode)); }
int t_rmdir(G<const char> path) { return check(rmdir(paths::translate(path).c_str())); }
int t_unlink(G<const char> path) { return check(unlink(paths::translate(path).c_str())); }
int t_remove(G<const char> path) { return check(remove(paths::translate(path).c_str())); }
int t_rename(G<const char> a, G<const char> b) {
    return check(rename(paths::translate(a).c_str(), paths::translate(b).c_str()));
}
int t_chmod(G<const char> path, u32 mode) { return check(chmod(paths::translate(path).c_str(), mode)); }
int t_chown(G<const char>, u32, u32) { return 0; }
int t_utime(G<const char> path, u32 times) {
    std::string p = paths::translate(path);
    if (!times) return check(utime(p.c_str(), nullptr));
    struct utimbuf u{(time_t)rd<s32>(times), (time_t)rd<s32>(times + 4)};
    return check(utime(p.c_str(), &u));
}
int t_utimes(G<const char> path, u32 tv) {
    std::string p = paths::translate(path);
    if (!tv) return check(utimes(p.c_str(), nullptr));
    struct timeval h[2];
    for (int i = 0; i < 2; i++) {
        h[i].tv_sec = rd<s32>(tv + i * 8);
        h[i].tv_usec = rd<s32>(tv + i * 8 + 4);
    }
    return check(utimes(p.c_str(), h));
}

// Bionic LP32 struct dirent (280 bytes): u64 d_ino (+0), s64 d_off (+8), u16 d_reclen (+16), u8 d_type (+18), char d_name[256] (+19).
constexpr u32 GDIRENT_SIZE = 280;
std::mutex g_dirs_lock;
std::unordered_map<u32, DIR *> g_dirs;
u32 t_opendir(G<const char> path) {
    std::string p = paths::translate(path);
    DIR *d = opendir(p.c_str());
    if (!d) return fail(), 0;
    u32 g = mem::halloc(GDIRENT_SIZE, 8);
    std::lock_guard<std::mutex> lk(g_dirs_lock);
    g_dirs[g] = d;
    return g;
}
u32 t_readdir(u32 g) {
    DIR *d;
    {
        std::lock_guard<std::mutex> lk(g_dirs_lock);
        auto it = g_dirs.find(g);
        if (it == g_dirs.end()) return 0;
        d = it->second;
    }
    struct dirent *e = readdir(d);
    if (!e) return 0;
    memset(g2h(g), 0, GDIRENT_SIZE);
    wr<u64>(g + 0, (u64)e->d_ino);
    wr<s64>(g + 8, (s64)e->d_off);
    wr<u16>(g + 16, GDIRENT_SIZE);
    wr<u8>(g + 18, e->d_type);
    strncpy((char *)g2h(g + 19), e->d_name, 255);
    return g;
}
int t_closedir(u32 g) {
    DIR *d;
    {
        std::lock_guard<std::mutex> lk(g_dirs_lock);
        auto it = g_dirs.find(g);
        if (it == g_dirs.end()) return fail(EBADF);
        d = it->second;
        g_dirs.erase(it);
    }
    mem::hfree(g);
    return closedir(d);
}

int t_select(int n, G<fd_set> r, G<fd_set> w, G<fd_set> e, u32 tv) {
    struct timeval h, *hp = nullptr;
    if (tv) {
        h.tv_sec = rd<s32>(tv);
        h.tv_usec = rd<s32>(tv + 4);
        hp = &h;
    }
    int res = select(n, r, w, e, hp);
    if (tv) {
        wr<s32>(tv, (s32)h.tv_sec);
        wr<s32>(tv + 4, (s32)h.tv_usec);
    }
    return res < 0 ? fail() : res;
}

int t_poll(G<struct pollfd> fds, u32 n, int timeout) { return check(poll(fds, n, timeout)); }

// ---------------------------------------------------------------- time
s32 t_time(u32 out) {
    s32 now = (s32)time(nullptr);
    if (out) wr<s32>(out, now);
    return now;
}
int t_gettimeofday(u32 tv, u32 tz) {
    struct timeval h;
    gettimeofday(&h, nullptr);
    if (tv) {
        wr<s32>(tv, (s32)h.tv_sec);
        wr<s32>(tv + 4, (s32)h.tv_usec);
    }
    if (tz) {
        wr<s32>(tz, 0);
        wr<s32>(tz + 4, 0);
    }
    return 0;
}
// Bionic LP32 struct timeb (12 bytes): s32 time (+0), u16 millitm (+4), s16 timezone (+6), s16 dstflag (+8).
int t_ftime(u32 tb) {
    if (!tb) return 0;
    struct timeval tv{};
    gettimeofday(&tv, nullptr);
    memset(g2h(tb), 0, 12);
    wr<s32>(tb + 0, (s32)tv.tv_sec);
    wr<u16>(tb + 4, (u16)(tv.tv_usec / 1000));
    wr<s16>(tb + 6, 0);
    wr<s16>(tb + 8, 0);
    return 0;
}
int t_clock_gettime(int clk, u32 ts) {
    struct timespec h;
    if (clock_gettime(clk, &h) < 0) return fail();
    wr<s32>(ts, (s32)h.tv_sec);
    wr<s32>(ts + 4, (s32)h.tv_nsec);
    return 0;
}
s32 t_clock() { return (s32)clock(); }
int t_nanosleep(u32 req, u32 rem) {
    struct timespec r{rd<s32>(req), rd<s32>(req + 4)}, left{};
    int res = nanosleep(&r, &left);
    if (rem) {
        wr<s32>(rem, (s32)left.tv_sec);
        wr<s32>(rem + 4, (s32)left.tv_nsec);
    }
    return res < 0 ? fail() : 0;
}
int t_usleep(u32 us) { return usleep(us); }
u32 t_sleep(u32 s) { return sleep(s); }
double t_difftime(s32 a, s32 b) { return (double)a - (double)b; }

// Bionic LP32 struct tm: 9 ints, long tm_gmtoff, const char* tm_zone (44 bytes).
void put_tm(u32 g, const struct tm &t) {
    s32 v[9] = {t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon, t.tm_year, t.tm_wday, t.tm_yday, t.tm_isdst};
    memcpy(g2h(g), v, sizeof(v));
    wr<s32>(g + 36, (s32)t.tm_gmtoff);
    static u32 zone_utc = mem::hstrdup("UTC");
    wr<u32>(g + 40, zone_utc);
}
struct tm get_tm(u32 g) {
    struct tm t{};
    s32 v[9];
    memcpy(v, g2h(g), sizeof(v));
    t.tm_sec = v[0]; t.tm_min = v[1]; t.tm_hour = v[2]; t.tm_mday = v[3]; t.tm_mon = v[4];
    t.tm_year = v[5]; t.tm_wday = v[6]; t.tm_yday = v[7]; t.tm_isdst = v[8];
    t.tm_gmtoff = rd<s32>(g + 36);
    return t;
}
u32 thread_tm() {
    thread_local u32 buf = mem::halloc(44);
    return buf;
}
u32 t_localtime(u32 tp) {
    time_t t = rd<s32>(tp);
    struct tm h{};
    localtime_r(&t, &h);
    u32 g = thread_tm();
    put_tm(g, h);
    return g;
}
u32 t_gmtime(u32 tp) {
    time_t t = rd<s32>(tp);
    struct tm h{};
    gmtime_r(&t, &h);
    u32 g = thread_tm();
    put_tm(g, h);
    return g;
}
u32 t_localtime_r(u32 tp, u32 out) {
    time_t t = rd<s32>(tp);
    struct tm h{};
    localtime_r(&t, &h);
    put_tm(out, h);
    return out;
}
u32 t_gmtime_r(u32 tp, u32 out) {
    time_t t = rd<s32>(tp);
    struct tm h{};
    gmtime_r(&t, &h);
    put_tm(out, h);
    return out;
}
s32 t_mktime(u32 g) {
    struct tm h = get_tm(g);
    time_t r = mktime(&h);
    put_tm(g, h);
    return (s32)r;
}
u32 t_strftime(G<char> buf, u32 max, G<const char> fmt, u32 tmp) {
    struct tm h = get_tm(tmp);
    return (u32)strftime(buf, max, fmt, &h);
}

// ---------------------------------------------------------------- math & Bionic FP classification
// Bionic ARM32 <math.h> FP_* bitmask constants:
//   FP_INFINITE = 0x01, FP_NAN = 0x02, FP_NORMAL = 0x04, FP_SUBNORMAL = 0x08, FP_ZERO = 0x10.
int t___fpclassifyd(double x) {
    u64 bits;
    memcpy(&bits, &x, sizeof(bits));
    u64 exp = (bits >> 52) & 0x7FFULL;
    u64 mant = bits & 0x000FFFFFFFFFFFFFULL;
    if (exp == 0x7FFULL) return mant ? 0x02 : 0x01;
    if (exp == 0) return mant ? 0x08 : 0x10;
    return 0x04;
}
int t___fpclassifyf(float x) {
    u32 bits;
    memcpy(&bits, &x, sizeof(bits));
    u32 exp = (bits >> 23) & 0xFFu;
    u32 mant = bits & 0x7FFFFFu;
    if (exp == 0xFFu) return mant ? 0x02 : 0x01;
    if (exp == 0) return mant ? 0x08 : 0x10;
    return 0x04;
}

#define DEF_MATH1(fn) double t_##fn(double x) { return fn(x); }
#define DEF_MATH1F(fn) float t_##fn(float x) { return fn(x); }
#define DEF_MATH2(fn) double t_##fn(double x, double y) { return fn(x, y); }
#define DEF_MATH2F(fn) float t_##fn(float x, float y) { return fn(x, y); }
DEF_MATH1(acos) DEF_MATH1(asin) DEF_MATH1(atan) DEF_MATH1(ceil) DEF_MATH1(cos) DEF_MATH1(cosh)
DEF_MATH1(exp) DEF_MATH1(floor) DEF_MATH1(log) DEF_MATH1(log10) DEF_MATH1(rint) DEF_MATH1(sin)
DEF_MATH1(sinh) DEF_MATH1(sqrt) DEF_MATH1(tan) DEF_MATH1(tanh) DEF_MATH1(fabs) DEF_MATH1(round)
DEF_MATH1(trunc) DEF_MATH1(exp2) DEF_MATH1(log2)
DEF_MATH1F(acosf) DEF_MATH1F(asinf) DEF_MATH1F(atanf) DEF_MATH1F(ceilf) DEF_MATH1F(cosf) DEF_MATH1F(expf)
DEF_MATH1F(exp2f) DEF_MATH1F(floorf) DEF_MATH1F(log10f) DEF_MATH1F(logf) DEF_MATH1F(sinf) DEF_MATH1F(sqrtf)
DEF_MATH1F(tanf) DEF_MATH1F(fabsf) DEF_MATH1F(roundf) DEF_MATH1F(truncf) DEF_MATH1F(log2f)
DEF_MATH2(atan2) DEF_MATH2(fmod) DEF_MATH2(pow) DEF_MATH2(hypot)
DEF_MATH2F(atan2f) DEF_MATH2F(fmodf) DEF_MATH2F(powf) DEF_MATH2F(hypotf)

s32 t_lrintf(float x) { return (s32)lrintf(x); }
s32 t_lrint(double x) { return (s32)lrint(x); }
double t_frexp(double x, G<int> e) { return frexp(x, e); }
double t_ldexp(double x, int e) { return ldexp(x, e); }
double t_scalbn(double x, int e) { return scalbn(x, e); }
double t_modf(double x, G<double> ip) {
    double i;
    double r = modf(x, &i);
    memcpy(ip.get(), &i, 8);
    return r;
}
float t_copysignf(float a, float b) { return copysignf(a, b); }
double t_copysign(double a, double b) { return copysign(a, b); }

// ---------------------------------------------------------------- CSPRNG (arc4random)
u32 t_arc4random() {
    u32 val = 0;
    if (getrandom(&val, sizeof(val), 0) == (ssize_t)sizeof(val)) return val;
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    val = (u32)(ts.tv_nsec ^ (ts.tv_sec * 2654435761ULL));
    return val;
}
void t_arc4random_buf(G<void> buf, u32 n) {
    if (!buf.a || !n) return;
    u8 *p = (u8 *)buf.get();
    u32 rem = n;
    while (rem > 0) {
        ssize_t r = getrandom(p, rem, 0);
        if (r <= 0) {
            for (u32 i = 0; i < rem; i++) p[i] = (u8)t_arc4random();
            return;
        }
        p += r;
        rem -= (u32)r;
    }
}

// ---------------------------------------------------------------- process / misc / strings
u32 t___errno() { return T().errno_addr; }

[[noreturn]] void t_abort() {
    cpu::dump_state(T());
    fatal("guest called abort()");
}
void t_exit(int code) {
    LOGI("guest exit(%d)", code);
    _exit(code);
}
void t___assert2(G<const char> file, int line, G<const char> func, G<const char> msg) {
    cpu::dump_state(T());
    fatal("guest assertion failed: %s:%d %s: %s", file.get(), line, func.get(), msg.get());
}
void t___stack_chk_fail() {
    cpu::dump_state(T());
    fatal("guest stack corruption detected");
}
void t___cxa_pure_virtual() {
    cpu::dump_state(T());
    fatal("guest pure virtual call");
}
int t___cxa_atexit(u32, u32, u32) { return 0; }
void t___cxa_finalize(u32) {}
int t_atexit(u32) { return 0; }

int t_raise(int sig) {
    if (sig == SIGABRT || sig == SIGSEGV || sig == SIGKILL) t_abort();
    return 0;
}
int t_sigaction(int, u32, u32 old) {
    if (old) memset(g2h(old), 0, 16);
    return 0;
}
u32 t_bsd_signal(int, u32) { return 0; }
int t_kill(int pid, int sig) { return check(kill(pid, sig)); }

std::mutex g_env_lock;
std::unordered_map<std::string, u32> g_env_cache;
u32 t_getenv(G<const char> name) {
    const char *v = getenv(name);
    if (!v) return 0;
    std::lock_guard<std::mutex> lk(g_env_lock);
    std::string key = std::string(name.get()) + "=" + v;
    auto it = g_env_cache.find(key);
    if (it != g_env_cache.end()) return it->second;
    return g_env_cache[key] = mem::hstrdup(v);
}
int t_setenv(G<const char> n, G<const char> v, int o) { return check(setenv(n, v, o)); }
int t_unsetenv(G<const char> n) { return check(unsetenv(n)); }
int t_getpid() { return getpid(); }
int t_gettid() { return (int)gettid(); }
u32 t_getuid() { return getuid(); }
u32 t_getgid() { return getgid(); }
u32 t_getegid() { return getegid(); }
u32 t_alarm(u32 s) { return alarm(s); }
u32 t_getpwuid(u32 uid) {
    static u32 pw = [] {
        u32 p = mem::halloc(24);
        wr<u32>(p + 0, mem::hstrdup("android"));
        wr<u32>(p + 4, mem::hstrdup(""));
        wr<u32>(p + 16, mem::hstrdup("/data"));
        wr<u32>(p + 20, mem::hstrdup("/system/bin/sh"));
        return p;
    }();
    wr<u32>(pw + 8, uid);
    wr<u32>(pw + 12, uid);
    return pw;
}
int t_gethostname(G<char> buf, u32 n) { return check(gethostname(buf, n)); }
s32 t_sysconf(int name) {
    if (name == _SC_PAGESIZE) return 4096;  // Guest ABI page size is always 4096.
    return (s32)sysconf(name);
}
int t_system(G<const char>) { return -1; }
u32 t_setlocale(int, u32) {
    static u32 c = mem::hstrdup("C");
    return c;
}
u32 t_strerror(int e) {
    static std::mutex lk;
    static std::unordered_map<int, u32> cache;
    std::lock_guard<std::mutex> g(lk);
    auto it = cache.find(e);
    if (it != cache.end()) return it->second;
    return cache[e] = mem::hstrdup(strerror(e));
}

inline int finish_xsi_strerror_r(int r, char *, size_t) { return r == -1 ? errno : r; }
inline int finish_xsi_strerror_r(char *s, char *buf, size_t n) {
    if (!buf || n == 0) return ERANGE;
    if (s != buf) {
        size_t len = strlen(s);
        if (len >= n) {
            memcpy(buf, s, n - 1);
            buf[n - 1] = '\0';
            return ERANGE;
        }
        memcpy(buf, s, len + 1);
    }
    return 0;
}
// Bionic LP32 exports XSI-compliant `int strerror_r(int errnum, char *buf, size_t buflen)`.
int t_strerror_r(int errnum, G<char> buf, u32 buflen) {
    if (!buf.a || buflen == 0) return ERANGE;
    char *dst = buf.get();
    return finish_xsi_strerror_r(strerror_r(errnum, dst, buflen), dst, buflen);
}

int t_sched_yield() { return sched_yield(); }
int t_sched_get_priority_max(int) { return 0; }
int t___system_property_get(G<const char>, G<char> out) {
    if (out.a) *out.get() = 0;
    return 0;
}

u32 t_getcwd(u32 buf, u32 size) {
    char tmp[4096];
    if (!getcwd(tmp, sizeof(tmp))) return fail(), 0;
    size_t n = strlen(tmp) + 1;
    if (!buf) buf = mem::halloc(size > n ? size : (u32)n);
    else if (n > size) return fail(ERANGE), 0;
    memcpy(g2h(buf), tmp, n);
    return buf;
}
int t_chdir(G<const char> path) { return check(chdir(paths::translate(path).c_str())); }

u32 t_strtok_r(u32 str, G<const char> delim, u32 save) {
    u32 p = str ? str : rd<u32>(save);
    if (!p) return 0;
    const char *d = delim;
    while (rd<char>(p) && strchr(d, rd<char>(p))) p++;
    if (!rd<char>(p)) {
        wr<u32>(save, 0);
        return 0;
    }
    u32 tok = p;
    while (rd<char>(p) && !strchr(d, rd<char>(p))) p++;
    if (rd<char>(p)) {
        wr<char>(p, 0);
        wr<u32>(save, p + 1);
    } else {
        wr<u32>(save, 0);
    }
    return tok;
}
u32 t_strxfrm(G<char> dst, G<const char> src, u32 n) { return (u32)strxfrm(dst.a ? dst.get() : nullptr, src, n); }
u32 t_basename(G<const char> path) {
    thread_local u32 buf = mem::halloc(4096);
    std::string p = path.a ? path.get() : "";
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    size_t i = p.rfind('/');
    std::string b = p.empty() ? "." : (i == std::string::npos || p == "/") ? p : p.substr(i + 1);
    snprintf((char *)g2h(buf), 4096, "%s", b.c_str());
    return buf;
}

// ---------------------------------------------------------------- wide characters
struct GuestMbstate {
    u32 g;
    mbstate_t h{};
    explicit GuestMbstate(u32 g) : g(g) { if (g) memcpy(&h, g2h(g), 4); }
    ~GuestMbstate() { if (g) memcpy(g2h(g), &h, 4); }
};
u32 t_mbrtowc(G<wchar_t> pwc, G<const char> s, u32 n, u32 ps) {
    static thread_local u32 internal = mem::halloc(4);
    GuestMbstate st(ps ? ps : internal);
    return (u32)mbrtowc(pwc.a ? pwc.get() : nullptr, s.a ? s.get() : nullptr, n, &st.h);
}
u32 t_wcrtomb(G<char> s, s32 wc, u32 ps) {
    static thread_local u32 internal = mem::halloc(4);
    GuestMbstate st(ps ? ps : internal);
    return (u32)wcrtomb(s.a ? s.get() : nullptr, (wchar_t)wc, &st.h);
}
u32 t_btowc(int c) { return (u32)btowc(c); }
int t_wctob(u32 c) { return wctob((wint_t)c); }
u32 t_towlower(u32 c) { return (u32)towlower((wint_t)c); }
u32 t_towupper(u32 c) { return (u32)towupper((wint_t)c); }
u32 t_wctype(G<const char> name) { return (u32)wctype(name); }
int t_iswctype(u32 c, u32 type) { return iswctype((wint_t)c, (wctype_t)type); }
int t_wcscoll(G<const wchar_t> a, G<const wchar_t> b) { return wcscoll(a, b); }
u32 t_wcsxfrm(G<wchar_t> d, G<const wchar_t> s, u32 n) { return (u32)wcsxfrm(d.a ? d.get() : nullptr, s, n); }
u32 t_wcsftime(G<wchar_t> s, u32 max, G<const wchar_t> fmt, u32 tmp) {
    struct tm t = get_tm(tmp);
    return (u32)wcsftime(s, max, fmt, &t);
}
u32 t_wmemchr(u32 s, s32 c, u32 n) {
    const wchar_t *p = wmemchr((const wchar_t *)g2h(s), (wchar_t)c, n);
    return p ? h2g(p) : 0;
}
int t_wmemcmp(G<const wchar_t> a, G<const wchar_t> b, u32 n) { return wmemcmp(a, b, n); }
u32 t_wmemcpy(u32 d, G<const wchar_t> s, u32 n) { wmemcpy((wchar_t *)g2h(d), s, n); return d; }
u32 t_wmemmove(u32 d, G<const wchar_t> s, u32 n) { wmemmove((wchar_t *)g2h(d), s, n); return d; }
u32 t_wmemset(u32 d, s32 c, u32 n) { wmemset((wchar_t *)g2h(d), (wchar_t)c, n); return d; }
u32 t_getwc(u32 gf) {
    FILE *f = hfile(gf);
    if (!f) return (u32)WEOF;
    wint_t r = fgetwc(f);
    sync_flags(gf, f);
    return (u32)r;
}
u32 t_putwc(u32 c, u32 gf) {
    FILE *f = hfile(gf);
    return f ? (u32)fputwc((wchar_t)c, f) : (u32)WEOF;
}
u32 t_ungetwc(u32 c, u32 gf) {
    FILE *f = hfile(gf);
    return f ? (u32)ungetwc((wint_t)c, f) : (u32)WEOF;
}

// ---------------------------------------------------------------- memory mapping
constexpr u32 G_MAP_FAILED = 0xffffffffu;
void t_mmap(GuestThread &t) {
    u32 addr = t.r(0), len = t.r(1);
    int prot = (int)t.r(2), flags = (int)t.r(3);
    int fd = (int)t.arg(4);
    s32 off = (s32)t.arg(5);
    (void)prot;
    u32 a = mem::map(len, (flags & MAP_FIXED) ? addr : 0);
    if (!a) {
        t.set_errno(ENOMEM);
        t.ret32(G_MAP_FAILED);
        return;
    }
    if (!(flags & MAP_ANONYMOUS) && fd >= 0) {
        ssize_t n = pread(fd, g2h(a), len, off);
        if (n < 0) LOGW("mmap pread failed: %s", strerror(errno));
    }
    t.ret32(a);
}
int t_munmap(u32 addr, u32 len) {
    mem::unmap(addr, len);
    return 0;
}
void t_mremap(GuestThread &t) {
    u32 old = t.r(0), old_len = t.r(1), new_len = t.r(2);
    u32 a = mem::map(new_len);
    if (!a) {
        t.set_errno(ENOMEM);
        t.ret32(G_MAP_FAILED);
        return;
    }
    memcpy(g2h(a), g2h(old), old_len < new_len ? old_len : new_len);
    mem::unmap(old, old_len);
    t.ret32(a);
}
int t_mprotect(u32, u32, int) { return 0; }
int t_madvise(u32, u32, int) { return 0; }

// ---------------------------------------------------------------- dynamic linking & unwinding
constexpr u32 G_RTLD_DEFAULT = 0xffffffffu;
constexpr u32 HOST_LIB_HANDLE = 0xfffffff0u;
thread_local u32 t_dlerror_msg = 0;

void set_dlerror(const std::string &msg) {
    if (t_dlerror_msg) mem::hfree(t_dlerror_msg);
    t_dlerror_msg = mem::hstrdup(msg.c_str());
}

u32 t_dlopen(G<const char> name, int) {
    if (!name.a) return G_RTLD_DEFAULT;
    std::string n = name.get();
    auto slash = n.rfind('/');
    if (slash != std::string::npos) n = n.substr(slash + 1);
    if (Module *m = loader::find_by_name(n)) return m->base | 1u;
    static const char *host_libs[] = {
        "libc.so", "libm.so", "libdl.so", "liblog.so", "libz.so",
        "libGLESv2.so", "libGLESv1_CM.so", "libEGL.so", "libandroid.so", "libstdc++.so"
    };
    for (const char *h : host_libs)
        if (n == h) return HOST_LIB_HANDLE;
    set_dlerror("library \"" + n + "\" not found");
    return 0;
}

u32 t_dlsym(u32 handle, G<const char> sym) {
    u32 a = 0;
    if (handle != G_RTLD_DEFAULT && handle != HOST_LIB_HANDLE && (handle & 1u)) {
        if (Module *m = loader::find_by_addr(handle & ~1u)) a = loader::lookup(sym.get(), m);
    } else {
        a = loader::lookup(sym.get());
    }
    if (!a) set_dlerror(std::string("undefined symbol: ") + sym.get());
    return a;
}
int t_dlclose(u32) { return 0; }
u32 t_dlerror() {
    u32 m = t_dlerror_msg;
    t_dlerror_msg = 0;
    return m;
}

u32 t___gnu_Unwind_Find_exidx(u32 pc, G<s32> count) {
    Module *m = loader::find_by_addr(pc);
    if (!m || !m->exidx) {
        *count.get() = 0;
        return 0;
    }
    *count.get() = (s32)m->exidx_count;
    return m->exidx;
}

int t___android_log_write(int prio, G<const char> tag, G<const char> msg) {
    return write_guest_log(prio, tag.a ? tag.get() : "gd102-guest", msg.a ? msg.get() : "");
}

// ---------------------------------------------------------------- POSIX networking & DNS (IPv4 + IPv6)
int t_socket(int domain, int type, int protocol) {
    return check(socket(domain, type, protocol));
}

int t_connect(int fd, G<const struct sockaddr> addr, u32 addrlen) {
    return check(connect(fd, addr.a ? addr.get() : nullptr, (socklen_t)addrlen));
}

int t_bind(int fd, G<const struct sockaddr> addr, u32 addrlen) {
    return check(bind(fd, addr.a ? addr.get() : nullptr, (socklen_t)addrlen));
}

int t_listen(int fd, int backlog) { return check(listen(fd, backlog)); }

int t_accept(int fd, G<struct sockaddr> addr, G<socklen_t> addrlen) {
    return check(accept(fd, addr.a ? addr.get() : nullptr, addrlen.a ? addrlen.get() : nullptr));
}

int t_getsockname(int fd, G<struct sockaddr> addr, G<socklen_t> addrlen) {
    return check(getsockname(fd, addr.a ? addr.get() : nullptr, addrlen.a ? addrlen.get() : nullptr));
}

int t_getpeername(int fd, G<struct sockaddr> addr, G<socklen_t> addrlen) {
    return check(getpeername(fd, addr.a ? addr.get() : nullptr, addrlen.a ? addrlen.get() : nullptr));
}

s32 t_send(int fd, G<const void> buf, u32 len, int flags) {
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    return (s32)check(send(fd, buf.a ? buf.get() : nullptr, len, flags));
}

s32 t_recv(int fd, G<void> buf, u32 len, int flags) {
    return (s32)check(recv(fd, buf.a ? buf.get() : nullptr, len, flags));
}

void t_sendto(GuestThread &t) {
    int fd = (int)t.r(0);
    const void *buf = g2h(t.r(1));
    size_t len = t.r(2);
    int flags = (int)t.r(3);
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    const auto *dest = (const struct sockaddr *)g2h(t.arg(4));
    socklen_t addrlen = (socklen_t)t.arg(5);
    ssize_t r = sendto(fd, buf, len, flags, dest, addrlen);
    t.ret32((u32)(r < 0 ? fail() : (s32)r));
}

void t_recvfrom(GuestThread &t) {
    int fd = (int)t.r(0);
    void *buf = g2h(t.r(1));
    size_t len = t.r(2);
    int flags = (int)t.r(3);
    auto *src = (struct sockaddr *)g2h(t.arg(4));
    auto *addrlen = (socklen_t *)g2h(t.arg(5));
    ssize_t r = recvfrom(fd, buf, len, flags, src, addrlen);
    t.ret32((u32)(r < 0 ? fail() : (s32)r));
}

int t_shutdown(int fd, int how) { return check(shutdown(fd, how)); }

int t_setsockopt(int fd, int level, int optname, u32 optval, u32 optlen) {
    if (level == SOL_SOCKET && (optname == SO_RCVTIMEO || optname == SO_SNDTIMEO) && optval && optlen >= 8) {
        struct timeval tv{};
        tv.tv_sec = rd<s32>(optval);
        tv.tv_usec = rd<s32>(optval + 4);
        return check(setsockopt(fd, level, optname, &tv, sizeof(tv)));
    }
    return check(setsockopt(fd, level, optname, g2h(optval), (socklen_t)optlen));
}

int t_getsockopt(int fd, int level, int optname, u32 optval, G<socklen_t> optlen) {
    if (level == SOL_SOCKET && (optname == SO_RCVTIMEO || optname == SO_SNDTIMEO) && optval && optlen.a && *optlen.get() >= 8) {
        struct timeval tv{};
        socklen_t hlen = sizeof(tv);
        int r = getsockopt(fd, level, optname, &tv, &hlen);
        if (r < 0) return fail();
        wr<s32>(optval, (s32)tv.tv_sec);
        wr<s32>(optval + 4, (s32)tv.tv_usec);
        *optlen.get() = 8;
        return 0;
    }
    return check(getsockopt(fd, level, optname, g2h(optval), optlen.a ? optlen.get() : nullptr));
}

// Bionic LP32 struct addrinfo (32 bytes):
//   +0  s32 ai_flags
//   +4  s32 ai_family
//   +8  s32 ai_socktype
//   +12 s32 ai_protocol
//   +16 u32 ai_addrlen
//   +20 u32 ai_canonname   (BSD/Bionic LP32 order: canonname BEFORE ai_addr!)
//   +24 u32 ai_addr
//   +28 u32 ai_next
std::mutex g_ai_lock;
std::unordered_map<u32, std::vector<u32>> g_ai_allocs;

int xlate_bionic_ai_flags(int gf) {
    int hf = 0;
    if (gf & 0x0001) hf |= AI_PASSIVE;
    if (gf & 0x0002) hf |= AI_CANONNAME;
    if (gf & 0x0004) hf |= AI_NUMERICHOST;
    if (gf & 0x0008) hf |= AI_NUMERICSERV;
    if (gf & 0x0100) hf |= AI_ALL;
    if (gf & 0x0400) hf |= AI_ADDRCONFIG;
    if (gf & 0x0800) hf |= AI_V4MAPPED;
    return hf;
}

int xlate_eai_to_bionic(int r) {
    if (r == 0) return 0;
#ifdef __ANDROID__
    return r;
#else
    switch (r) {
    case EAI_AGAIN:    return 2;
    case EAI_BADFLAGS: return 3;
    case EAI_FAIL:     return 4;
    case EAI_FAMILY:   return 5;
    case EAI_MEMORY:   return 6;
#ifdef EAI_NODATA
    case EAI_NODATA:   return 7;
#endif
    case EAI_NONAME:   return 8;
    case EAI_SERVICE:  return 9;
    case EAI_SOCKTYPE: return 10;
    case EAI_SYSTEM:   T().set_errno(errno); return 11;
    default:           return 4;
    }
#endif
}

int t_getaddrinfo(G<const char> node, G<const char> service, u32 hints_g, G<u32> res_g) {
    if (res_g.a) *res_g.get() = 0;
    struct addrinfo hints{};
    if (hints_g) {
        hints.ai_flags = xlate_bionic_ai_flags(rd<s32>(hints_g + 0));
        hints.ai_family = rd<s32>(hints_g + 4);
        hints.ai_socktype = rd<s32>(hints_g + 8);
        hints.ai_protocol = rd<s32>(hints_g + 12);
    }
    struct addrinfo *hres = nullptr;
    int rc = getaddrinfo(node.a ? node.get() : nullptr, service.a ? service.get() : nullptr,
                         hints_g ? &hints : nullptr, &hres);
    if (rc != 0) return xlate_eai_to_bionic(rc);

    std::vector<u32> blocks;
    u32 head = 0, prev = 0;
    for (struct addrinfo *cur = hres; cur; cur = cur->ai_next) {
        u32 node_addr = mem::halloc(32, 8);
        blocks.push_back(node_addr);

        u32 sa_addr = 0;
        if (cur->ai_addr && cur->ai_addrlen > 0) {
            sa_addr = mem::halloc((u32)cur->ai_addrlen, 8);
            memcpy(g2h(sa_addr), cur->ai_addr, cur->ai_addrlen);
            blocks.push_back(sa_addr);
        }
        u32 canon_addr = 0;
        if (cur->ai_canonname) {
            canon_addr = mem::hstrdup(cur->ai_canonname);
            blocks.push_back(canon_addr);
        }

        wr<s32>(node_addr + 0, cur->ai_flags);
        wr<s32>(node_addr + 4, cur->ai_family);
        wr<s32>(node_addr + 8, cur->ai_socktype);
        wr<s32>(node_addr + 12, cur->ai_protocol);
        wr<u32>(node_addr + 16, (u32)cur->ai_addrlen);
        wr<u32>(node_addr + 20, canon_addr);
        wr<u32>(node_addr + 24, sa_addr);
        wr<u32>(node_addr + 28, 0);

        if (!head) head = node_addr;
        if (prev) wr<u32>(prev + 28, node_addr);
        prev = node_addr;
    }
    freeaddrinfo(hres);

    if (head) {
        std::lock_guard<std::mutex> lk(g_ai_lock);
        g_ai_allocs[head] = std::move(blocks);
    }
    if (res_g.a) *res_g.get() = head;
    return 0;
}

void t_freeaddrinfo(u32 ai) {
    if (!ai) return;
    std::vector<u32> blocks;
    {
        std::lock_guard<std::mutex> lk(g_ai_lock);
        auto it = g_ai_allocs.find(ai);
        if (it == g_ai_allocs.end()) return;
        blocks = std::move(it->second);
        g_ai_allocs.erase(it);
    }
    for (u32 b : blocks) mem::hfree(b);
}

u32 t_gethostbyname(G<const char> name) {
    if (!name.a) return 0;
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    struct addrinfo *res = nullptr;
    if (getaddrinfo(name.get(), nullptr, &hints, &res) != 0 || !res) return 0;

    // Bionic LP32 struct hostent (20 bytes): h_name (+0), h_aliases (+4), h_addrtype (+8), h_length (+12), h_addr_list (+16).
    thread_local u32 hent = 0;
    if (!hent) hent = mem::halloc(64, 8);
    auto *sin = (const struct sockaddr_in *)res->ai_addr;
    u32 ipv4 = sin->sin_addr.s_addr;
    freeaddrinfo(res);

    u32 name_p = hent + 36;
    snprintf((char *)g2h(name_p), 28, "%s", name.get());
    u32 aliases_p = hent + 20;
    wr<u32>(aliases_p, 0);
    u32 addr_storage = hent + 32;
    wr<u32>(addr_storage, ipv4);
    u32 addr_list_p = hent + 24;
    wr<u32>(addr_list_p + 0, addr_storage);
    wr<u32>(addr_list_p + 4, 0);

    wr<u32>(hent + 0, name_p);
    wr<u32>(hent + 4, aliases_p);
    wr<s32>(hent + 8, AF_INET);
    wr<s32>(hent + 12, 4);
    wr<u32>(hent + 16, addr_list_p);
    return hent;
}

u32 t_inet_addr(G<const char> s) {
    if (!s.a) return 0xffffffffu;
    struct in_addr in{};
    return inet_aton(s.get(), &in) ? in.s_addr : 0xffffffffu;
}
u32 t_inet_ntoa(u32 in) {
    thread_local u32 buf = mem::halloc(16);
    snprintf((char *)g2h(buf), 16, "%u.%u.%u.%u", in & 255, (in >> 8) & 255, (in >> 16) & 255, in >> 24);
    return buf;
}
u32 t_inet_ntop(int af, G<const void> src, u32 dst, u32 size) {
    int haf = (af == 2) ? AF_INET : (af == 10) ? AF_INET6 : -1;
    if (haf < 0) return fail(EAFNOSUPPORT), 0;
    const char *r = inet_ntop(haf, src.get(), (char *)g2h(dst), (socklen_t)size);
    if (!r) return fail(), 0;
    return dst;
}
int t_inet_pton(int af, G<const char> src, G<void> dst) {
    int haf = (af == 2) ? AF_INET : (af == 10) ? AF_INET6 : -1;
    if (haf < 0) return fail(EAFNOSUPPORT);
    int r = inet_pton(haf, src.get(), dst.get());
    if (r < 0) return fail();
    return r;
}

}  // namespace

void register_libc_thunks() {
    // stdio
    THUNK("fopen", t_fopen);
    THUNK("fclose", t_fclose);
    THUNK("fread", t_fread);
    THUNK("fwrite", t_fwrite);
    THUNK("fputs", t_fputs);
    THUNK("fputc", t_fputc);
    THUNK("putc", t_fputc);
    THUNK("fgetc", t_fgetc);
    THUNK("getc", t_fgetc);
    THUNK("__srget", t_fgetc);
    THUNK("ungetc", t_ungetc);
    THUNK("fgets", t_fgets);
    THUNK("fflush", t_fflush);
    THUNK("fseek", t_fseek);
    THUNK("fseeko", t_fseek);
    THUNK("ftell", t_ftell);
    THUNK("ftello", t_ftell);
    THUNK("rewind", t_rewind);
    THUNK("feof", t_feof);
    THUNK("ferror", t_ferror);
    THUNK("fileno", t_fileno);
    THUNK("setvbuf", t_setvbuf);
    THUNK("clearerr", t_clearerr);
    THUNK("setbuf", t_setbuf);
    THUNK("fdopen", t_fdopen);
    THUNK("freopen", t_freopen);
    THUNK("tmpfile", t_tmpfile);
    THUNK("tmpnam", t_tmpnam);
    THUNK("popen", t_popen);
    THUNK("pclose", t_pclose);

    // fd & filesystem
    THUNK_RAW("open", t_open);
    THUNK("read", t_read);
    THUNK("write", t_write);
    THUNK("close", t_close);
    THUNK("lseek", t_lseek);
    THUNK("lseek64", t_lseek64);
    THUNK("fsync", t_fsync);
    THUNK("ftruncate", t_ftruncate);
    THUNK_RAW("fcntl", t_fcntl);
    THUNK_RAW("ioctl", t_ioctl);
    THUNK("stat", t_stat);
    THUNK("lstat", t_lstat);
    THUNK("fstat", t_fstat);
    THUNK("access", t_access);
    THUNK("mkdir", t_mkdir);
    THUNK("rmdir", t_rmdir);
    THUNK("unlink", t_unlink);
    THUNK("remove", t_remove);
    THUNK("rename", t_rename);
    THUNK("chmod", t_chmod);
    THUNK("chown", t_chown);
    THUNK("utime", t_utime);
    THUNK("utimes", t_utimes);
    THUNK("opendir", t_opendir);
    THUNK("readdir", t_readdir);
    THUNK("closedir", t_closedir);
    THUNK("select", t_select);
    THUNK("poll", t_poll);

    // time
    THUNK("time", t_time);
    THUNK("gettimeofday", t_gettimeofday);
    THUNK("ftime", t_ftime);
    THUNK("clock_gettime", t_clock_gettime);
    THUNK("clock", t_clock);
    THUNK("nanosleep", t_nanosleep);
    THUNK("usleep", t_usleep);
    THUNK("sleep", t_sleep);
    THUNK("difftime", t_difftime);
    THUNK("localtime", t_localtime);
    THUNK("gmtime", t_gmtime);
    THUNK("localtime_r", t_localtime_r);
    THUNK("gmtime_r", t_gmtime_r);
    THUNK("mktime", t_mktime);
    THUNK("strftime", t_strftime);

    // math
    THUNK("__fpclassifyd", t___fpclassifyd);
    THUNK("__fpclassifyf", t___fpclassifyf);
#define REG_M(fn) THUNK(#fn, t_##fn)
    REG_M(acos); REG_M(asin); REG_M(atan); REG_M(ceil); REG_M(cos); REG_M(cosh);
    REG_M(exp); REG_M(floor); REG_M(log); REG_M(log10); REG_M(rint); REG_M(sin);
    REG_M(sinh); REG_M(sqrt); REG_M(tan); REG_M(tanh); REG_M(fabs); REG_M(round);
    REG_M(trunc); REG_M(exp2); REG_M(log2);
    REG_M(acosf); REG_M(asinf); REG_M(atanf); REG_M(ceilf); REG_M(cosf); REG_M(expf);
    REG_M(exp2f); REG_M(floorf); REG_M(log10f); REG_M(logf); REG_M(sinf); REG_M(sqrtf);
    REG_M(tanf); REG_M(fabsf); REG_M(roundf); REG_M(truncf); REG_M(log2f);
    REG_M(atan2); REG_M(fmod); REG_M(pow); REG_M(hypot);
    REG_M(atan2f); REG_M(fmodf); REG_M(powf); REG_M(hypotf);
    REG_M(lrintf); REG_M(lrint); REG_M(frexp); REG_M(ldexp); REG_M(scalbn); REG_M(modf);
    REG_M(copysignf); REG_M(copysign);
#undef REG_M

    // CSPRNG & process
    THUNK("arc4random", t_arc4random);
    THUNK("arc4random_buf", t_arc4random_buf);
    THUNK("__errno", t___errno);
    THUNK("abort", t_abort);
    THUNK("exit", t_exit);
    THUNK("_exit", t_exit);
    THUNK("__assert2", t___assert2);
    THUNK("__stack_chk_fail", t___stack_chk_fail);
    THUNK("__cxa_pure_virtual", t___cxa_pure_virtual);
    THUNK("__cxa_atexit", t___cxa_atexit);
    THUNK("__aeabi_atexit", t___cxa_atexit);
    THUNK("__cxa_finalize", t___cxa_finalize);
    THUNK("atexit", t_atexit);
    THUNK("raise", t_raise);
    THUNK("sigaction", t_sigaction);
    THUNK("bsd_signal", t_bsd_signal);
    THUNK("signal", t_bsd_signal);
    THUNK("kill", t_kill);
    THUNK("getenv", t_getenv);
    THUNK("setenv", t_setenv);
    THUNK("unsetenv", t_unsetenv);
    THUNK("getpid", t_getpid);
    THUNK("gettid", t_gettid);
    THUNK("getuid", t_getuid);
    THUNK("geteuid", t_getuid);
    THUNK("getgid", t_getgid);
    THUNK("getegid", t_getegid);
    THUNK("alarm", t_alarm);
    THUNK("getpwuid", t_getpwuid);
    THUNK("gethostname", t_gethostname);
    THUNK("sysconf", t_sysconf);
    THUNK("system", t_system);
    THUNK("setlocale", t_setlocale);
    THUNK("strerror", t_strerror);
    THUNK("strerror_r", t_strerror_r);
    THUNK("sched_yield", t_sched_yield);
    THUNK("sched_get_priority_max", t_sched_get_priority_max);
    THUNK("sched_get_priority_min", t_sched_get_priority_max);
    THUNK("__system_property_get", t___system_property_get);
    THUNK("getcwd", t_getcwd);
    THUNK("chdir", t_chdir);
    THUNK("strtok_r", t_strtok_r);
    THUNK("strxfrm", t_strxfrm);
    THUNK("basename", t_basename);

    // wide characters
    THUNK("mbrtowc", t_mbrtowc);
    THUNK("wcrtomb", t_wcrtomb);
    THUNK("btowc", t_btowc);
    THUNK("wctob", t_wctob);
    THUNK("towlower", t_towlower);
    THUNK("towupper", t_towupper);
    THUNK("wctype", t_wctype);
    THUNK("iswctype", t_iswctype);
    THUNK("wcscoll", t_wcscoll);
    THUNK("wcsxfrm", t_wcsxfrm);
    THUNK("wcsftime", t_wcsftime);
    THUNK("wmemchr", t_wmemchr);
    THUNK("wmemcmp", t_wmemcmp);
    THUNK("wmemcpy", t_wmemcpy);
    THUNK("wmemmove", t_wmemmove);
    THUNK("wmemset", t_wmemset);
    THUNK("getwc", t_getwc);
    THUNK("putwc", t_putwc);
    THUNK("ungetwc", t_ungetwc);

    // mmap & dlopen
    THUNK_RAW("mmap", t_mmap);
    THUNK("munmap", t_munmap);
    THUNK_RAW("mremap", t_mremap);
    THUNK("mprotect", t_mprotect);
    THUNK("madvise", t_madvise);
    THUNK("dlopen", t_dlopen);
    THUNK("dlsym", t_dlsym);
    THUNK("dlclose", t_dlclose);
    THUNK("dlerror", t_dlerror);
    THUNK("__gnu_Unwind_Find_exidx", t___gnu_Unwind_Find_exidx);
    THUNK("__android_log_write", t___android_log_write);

    // networking & DNS
    THUNK("socket", t_socket);
    THUNK("connect", t_connect);
    THUNK("bind", t_bind);
    THUNK("listen", t_listen);
    THUNK("accept", t_accept);
    THUNK("getsockname", t_getsockname);
    THUNK("getpeername", t_getpeername);
    THUNK("send", t_send);
    THUNK("recv", t_recv);
    THUNK_RAW("sendto", t_sendto);
    THUNK_RAW("recvfrom", t_recvfrom);
    THUNK("shutdown", t_shutdown);
    THUNK("setsockopt", t_setsockopt);
    THUNK("getsockopt", t_getsockopt);
    THUNK("getaddrinfo", t_getaddrinfo);
    THUNK("freeaddrinfo", t_freeaddrinfo);
    THUNK("gethostbyname", t_gethostbyname);
    THUNK("inet_addr", t_inet_addr);
    THUNK("inet_ntoa", t_inet_ntoa);
    THUNK("inet_ntop", t_inet_ntop);
    THUNK("inet_pton", t_inet_pton);
}

void bind_guest_stdio() {
    g_sF = loader::lookup("__sF");
    if (!g_sF) fatal("guest runtime does not define __sF");
    for (int i = 0; i < 3; i++) wr<s16>(g_sF + (u32)i * GFILE_SIZE + 14, (s16)i);
}
