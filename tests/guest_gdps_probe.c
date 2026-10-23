// SPDX-License-Identifier: MIT
// Real ARM32 probe executed inside the Dynarmic guest to prove that the embedded
// private Geometry Dash 1.0 server is reachable exactly the way libgame.so talks to
// boomlings.com: gethostbyname() -> socket() -> connect() -> HTTP POST -> recv().
//
// Built in CI with arm-linux-gnueabi-gcc (-marm -march=armv7-a -mfloat-abi=soft -O2
// -shared -fPIC) and loaded by gd102_host_run_guest_probe().

#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PROBE_SECRET "Wmfd2893gb7"

#define FAIL_DNS         (1u << 0)
#define FAIL_CONNECT     (1u << 1)
#define FAIL_SEARCH      (1u << 2)
#define FAIL_DOWNLOAD    (1u << 3)
#define FAIL_UPLOAD      (1u << 4)
#define FAIL_RECENT      (1u << 5)
#define FAIL_DOWNLOADED  (1u << 6)
#define FAIL_LIKE        (1u << 7)
#define FAIL_UPDATE      (1u << 8)

static int http_post(const char *script, const char *body, char *out, int out_size) {
    struct hostent *he = gethostbyname("www.boomlings.com");
    if (!he || !he->h_addr_list || !he->h_addr_list[0] || he->h_addrtype != AF_INET || he->h_length != 4) {
        return -1000;
    }
    unsigned char ip[4];
    memcpy(ip, he->h_addr_list[0], 4);
    if (ip[0] != 127 || ip[1] != 0 || ip[2] != 0 || ip[3] != 77) {
        printf("[guest-probe] gethostbyname(www.boomlings.com) -> %u.%u.%u.%u (expected 127.0.0.77)\n",
               ip[0], ip[1], ip[2], ip[3]);
        return -1001;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1002;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(80);
    memcpy(&sa.sin_addr, he->h_addr_list[0], 4);

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1003;
    }

    char req[2048];
    int req_len = snprintf(req, sizeof(req),
                           "POST /database/%s HTTP/1.0\r\n"
                           "Host: www.boomlings.com\r\n"
                           "Content-Type: application/x-www-form-urlencoded\r\n"
                           "Content-Length: %u\r\n"
                           "Connection: close\r\n\r\n%s",
                           script, (unsigned)strlen(body), body);
    if (req_len <= 0 || req_len >= (int)sizeof(req)) {
        close(fd);
        return -1004;
    }

    int sent = 0;
    while (sent < req_len) {
        int n = (int)send(fd, req + sent, (size_t)(req_len - sent), 0);
        if (n <= 0) {
            close(fd);
            return -1005;
        }
        sent += n;
    }

    int total = 0;
    while (total < out_size - 1) {
        int n = (int)recv(fd, out + total, (size_t)(out_size - 1 - total), 0);
        if (n <= 0) break;
        total += n;
    }
    out[total] = '\0';
    close(fd);

    char *sep = strstr(out, "\r\n\r\n");
    if (!sep) return -1006;
    sep += 4;
    int body_len = total - (int)(sep - out);
    memmove(out, sep, (size_t)body_len);
    out[body_len] = '\0';
    return body_len;
}

// Returns 0 when every check passes, otherwise a bitmask of FAIL_* flags.
int gdps_probe_run(void) {
    static char resp[256 * 1024];
    unsigned fails = 0;

    struct hostent *he = gethostbyname("www.boomlings.com");
    if (!he || !he->h_addr_list || !he->h_addr_list[0] || he->h_length != 4) {
        printf("[guest-probe] FAIL dns: gethostbyname(www.boomlings.com) returned no IPv4\n");
        return FAIL_DNS;
    }

    printf("[guest-probe] gethostbyname(www.boomlings.com) -> %u.%u.%u.%u\n",
           (unsigned)he->h_addr_list[0][0], (unsigned)he->h_addr_list[0][1],
           (unsigned)he->h_addr_list[0][2], (unsigned)he->h_addr_list[0][3]);

    // 1) Level search (type=0, str=1st) must return real archived 1.0 levels.
    int n = http_post("getGJLevels.php",
                      "type=0&str=1st&diff=-&len=-&page=0&secret=" PROBE_SECRET, resp, sizeof(resp));
    if (n <= 0 || !strstr(resp, "1:") || !strstr(resp, ":2:") || !strstr(resp, "#")) {
        printf("[guest-probe] FAIL search: n=%d body='%.160s'\n", n, resp);
        fails |= FAIL_SEARCH;
    } else {
        printf("[guest-probe] search OK (%d bytes)\n", n);
    }

    // 2) Direct level download of the first user level (ID 128) must include level data.
    n = http_post("downloadGJLevel.php", "levelID=128&inc=1&secret=" PROBE_SECRET, resp, sizeof(resp));
    if (n <= 0 || !strstr(resp, ":1:128:") || !strstr(resp, ":4:kS1,") || !strstr(resp, ",kA1,") ||
        !strstr(resp, ":13:")) {
        printf("[guest-probe] FAIL download: n=%d body='%.200s'\n", n, resp);
        fails |= FAIL_DOWNLOAD;
    } else {
        printf("[guest-probe] download(128) OK (%d bytes, level string present)\n", n);
    }

    // 3) Upload endpoints must be hard-blocked.
    n = http_post("uploadGJLevel.php",
                  "udid=probe&userName=probe&levelName=x&levelDesc=x&levelString=kS1,1;&levelVersion=1&"
                  "levelLength=0&audioTrack=0&gameVersion=1&secret=" PROBE_SECRET,
                  resp, sizeof(resp));
    if (n < 0 || strcmp(resp, "-1") != 0) {
        printf("[guest-probe] FAIL upload-blocked: n=%d body='%.120s'\n", n, resp);
        fails |= FAIL_UPLOAD;
    } else {
        printf("[guest-probe] uploadGJLevel blocked OK\n");
    }

    // 4) "Most Recent" must be the newest archived 1.0 level (IDs descend from 1941).
    n = http_post("getGJLevels.php", "type=4&str=&diff=-&len=-&page=0&secret=" PROBE_SECRET, resp, sizeof(resp));
    if (n <= 0 || !strstr(resp, "1:19") || !strstr(resp, "#")) {
        printf("[guest-probe] FAIL recent: n=%d body='%.160s'\n", n, resp);
        fails |= FAIL_RECENT;
    } else {
        printf("[guest-probe] most-recent OK (starts at ID 1941)\n");
    }

    // 5) "Most Downloaded" must lead with one of the classic 1.0 IDs.
    n = http_post("getGJLevels.php", "type=1&str=&diff=-&len=-&page=0&secret=" PROBE_SECRET, resp, sizeof(resp));
    if (n <= 0 || !strstr(resp, "1:")) {
        printf("[guest-probe] FAIL most-downloaded: n=%d body='%.160s'\n", n, resp);
        fails |= FAIL_DOWNLOADED;
    } else {
        printf("[guest-probe] most-downloaded OK (%.120s)\n", resp);
    }

    // 6) Likes must be accepted exactly once per level.
    n = http_post("likeGJLevel.php", "levelID=128&secret=" PROBE_SECRET, resp, sizeof(resp));
    if (n <= 0 || strcmp(resp, "1") != 0) {
        printf("[guest-probe] FAIL like: n=%d body='%.80s'\n", n, resp);
        fails |= FAIL_LIKE;
    } else {
        printf("[guest-probe] likeGJLevel OK\n");
    }

    // 7) updateGJLevel must return the same level payload (update/refresh path used by LevelInfoLayer).
    n = http_post("updateGJLevel.php", "levelID=128&levelVersion=0&gameVersion=1&secret=" PROBE_SECRET, resp,
                  sizeof(resp));
    if (n <= 0 || !strstr(resp, ":1:128:") || !strstr(resp, ":4:kS1,")) {
        printf("[guest-probe] FAIL update: n=%d body='%.160s'\n", n, resp);
        fails |= FAIL_UPDATE;
    } else {
        printf("[guest-probe] updateGJLevel OK\n");
    }

    printf("[guest-probe] result=0x%x\n", fails);
    return (int)fails;
}
