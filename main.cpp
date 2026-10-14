// ps4chrono — NTP time sync payload for PS4 (GoldHEN)
// Created by SergioPoverony and Mr.Claude
// License: GPLv3

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <ctime>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "icon_data.h"

// ── Version ──────────────────────────────────────────────
#define APP_VERSION "0.2.1"
#define APP_NAME    "ps4chrono"

// Build timestamp as fallback minimum time (compiled into binary)
#define BUILD_YEAR  2026
#define BUILD_MONTH 10
#define BUILD_DAY   14

// ── Paths ────────────────────────────────────────────────
#define DATA_DIR     "/data/ps4chrono"
#define TIME_FILE    DATA_DIR "/lastgood.dat"
#define CONFIG_FILE  DATA_DIR "/config.dat"
#define LOG_FILE     DATA_DIR "/ps4chrono.log"
#define QUIT_FLAG    DATA_DIR "/quit.flag"
#define LOCK_FILE    DATA_DIR "/lock"

// ── Defaults ─────────────────────────────────────────────
#define DEFAULT_CHECK_INTERVAL_SEC (6 * 3600)  // 6 hours
#define NTP_PORT           123
#define NTP_PACKET_SIZE    48
#define NTP_TIMEOUT_SEC    2
#define WEB_PORT           8585
#define MAX_NTP_SERVERS    8

// ── NTP epoch offset (1900 → 1970) ──────────────────────
#define NTP_EPOCH_DELTA 2208988800ULL

// ── Log ──────────────────────────────────────────────────
static FILE* g_logfp = nullptr;

static void log_msg(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    // timestamp
    time_t now = time(nullptr);
    struct tm tm;
    gmtime_r(&now, &tm);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

    if (g_logfp) {
        fprintf(g_logfp, "[%s] %s\n", ts, buf);
        fflush(g_logfp);
    }
}

// ── Notifications ────────────────────────────────────────
struct notify_request_t {
    int type, reqId, priority, msgId, targetId, userId;
    int unk1, unk2, appId, errorNum, unk3;
    unsigned char useIconImageUri;
    char message[1024];
    char iconImageUri[1024];
    char unk[1024];
};
static_assert(sizeof(notify_request_t) == 3120, "notification request size");

extern "C" int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);

// Notify icon URL — set at startup when web server is ready
static char g_notify_icon_url[256] = "";

static void notify(const char* fmt, ...) {
    notify_request_t req;
    memset(&req, 0, sizeof(req));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(req.message, sizeof(req.message), fmt, ap);
    va_end(ap);
    if (g_notify_icon_url[0]) {
        req.useIconImageUri = 1;
        strncpy(req.iconImageUri, g_notify_icon_url, sizeof(req.iconImageUri) - 1);
    }
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

// ── File helpers ─────────────────────────────────────────
static void ensure_dir(const char* path) {
    mkdir(path, 0777);
}

static bool file_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void remove_file(const char* path) {
    unlink(path);
}

// ── Last known good time ─────────────────────────────────
static time_t load_last_good_time() {
    FILE* f = fopen(TIME_FILE, "r");
    if (!f) return 0;
    time_t t = 0;
    if (fscanf(f, "%ld", &t) != 1) t = 0;
    fclose(f);
    return t;
}

static void save_last_good_time(time_t t) {
    FILE* f = fopen(TIME_FILE, "w");
    if (!f) {
        log_msg("ERROR: cannot write %s: %s", TIME_FILE, strerror(errno));
        return;
    }
    fprintf(f, "%ld\n", (long)t);
    fclose(f);
    log_msg("Saved last good time: %ld", (long)t);
}

// ── Build date as minimum valid time ─────────────────────
static time_t get_build_time() {
    struct tm bt;
    memset(&bt, 0, sizeof(bt));
    bt.tm_year = BUILD_YEAR - 1900;
    bt.tm_mon  = BUILD_MONTH - 1;
    bt.tm_mday = BUILD_DAY;
    return timegm(&bt);
}

// ── NTP ──────────────────────────────────────────────────
struct ntp_packet_t {
    uint8_t  li_vn_mode;
    uint8_t  stratum;
    uint8_t  poll;
    int8_t   precision;
    uint32_t root_delay;
    uint32_t root_dispersion;
    uint32_t ref_id;
    uint32_t ref_ts_sec;
    uint32_t ref_ts_frac;
    uint32_t orig_ts_sec;
    uint32_t orig_ts_frac;
    uint32_t recv_ts_sec;
    uint32_t recv_ts_frac;
    uint32_t tx_ts_sec;
    uint32_t tx_ts_frac;
};
static_assert(sizeof(ntp_packet_t) == NTP_PACKET_SIZE, "NTP packet size");

// Default NTP servers
static const char* g_default_servers[] = {
    "pool.ntp.org",
    "time.google.com",
    "time.cloudflare.com",
    "time.windows.com"
};
#define NUM_DEFAULT_SERVERS 4

// Active server list
struct server_list_t {
    char servers[MAX_NTP_SERVERS][128];
    int  count;
};

static server_list_t g_servers;
static int g_check_interval_sec = DEFAULT_CHECK_INTERVAL_SEC;

// ── Config persistence ───────────────────────────────────
static void load_config() {
    g_servers.count = 0;
    g_check_interval_sec = DEFAULT_CHECK_INTERVAL_SEC;

    FILE* f = fopen(CONFIG_FILE, "r");
    if (!f) {
        // Load defaults
        for (int i = 0; i < NUM_DEFAULT_SERVERS && i < MAX_NTP_SERVERS; i++) {
            strncpy(g_servers.servers[i], g_default_servers[i],
                    sizeof(g_servers.servers[i]) - 1);
            g_servers.count++;
        }
        log_msg("No config file, using %d default servers", g_servers.count);
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        // Remove newline
        char* nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char* cr = strchr(line, '\r');
        if (cr) *cr = '\0';

        if (strncmp(line, "server=", 7) == 0 && g_servers.count < MAX_NTP_SERVERS) {
            strncpy(g_servers.servers[g_servers.count], line + 7,
                    sizeof(g_servers.servers[0]) - 1);
            g_servers.count++;
        } else if (strncmp(line, "interval=", 9) == 0) {
            int val = atoi(line + 9);
            if (val >= 60) g_check_interval_sec = val;
        }
    }
    fclose(f);

    if (g_servers.count == 0) {
        for (int i = 0; i < NUM_DEFAULT_SERVERS && i < MAX_NTP_SERVERS; i++) {
            strncpy(g_servers.servers[i], g_default_servers[i],
                    sizeof(g_servers.servers[i]) - 1);
            g_servers.count++;
        }
    }
    log_msg("Config loaded: %d servers, interval %d sec",
            g_servers.count, g_check_interval_sec);
}

static void save_config() {
    FILE* f = fopen(CONFIG_FILE, "w");
    if (!f) {
        log_msg("ERROR: cannot write config: %s", strerror(errno));
        return;
    }
    fprintf(f, "interval=%d\n", g_check_interval_sec);
    for (int i = 0; i < g_servers.count; i++) {
        fprintf(f, "server=%s\n", g_servers.servers[i]);
    }
    fclose(f);
    log_msg("Config saved");
}

// ── Hardcoded fallback IPs for default NTP servers ───────
struct ip_fallback_t {
    const char* hostname;
    const char* ip;
};

static const ip_fallback_t g_fallback_ips[] = {
    {"pool.ntp.org",        "162.159.200.1"},    // Cloudflare NTP
    {"time.google.com",     "216.239.35.0"},
    {"time.cloudflare.com", "162.159.200.123"},
    {"time.windows.com",    "168.61.215.74"},
};
#define NUM_FALLBACK_IPS 4

// DNS servers to try (Google, Cloudflare)
static const char* g_dns_servers[] = {"8.8.8.8", "1.1.1.1"};
#define NUM_DNS_SERVERS 2

// ── Manual DNS resolver via UDP ──────────────────────────
// Sends a standard DNS A-record query and parses the response.
// This bypasses getaddrinfo which doesn't work from PS4 payload.

static bool dns_resolve_udp(const char* hostname, const char* dns_server,
                            struct in_addr* result) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return false;

    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(53);
    inet_pton(AF_INET, dns_server, &sa.sin_addr);

    // Build DNS query packet
    uint8_t query[512];
    memset(query, 0, sizeof(query));

    // Header
    query[0] = 0xAB; query[1] = 0xCD;  // Transaction ID
    query[2] = 0x01; query[3] = 0x00;  // Standard query, recursion desired
    query[4] = 0x00; query[5] = 0x01;  // 1 question
    // Answers, authority, additional = 0

    // Question: encode hostname as DNS labels
    int pos = 12;
    const char* p = hostname;
    while (*p) {
        const char* dot = strchr(p, '.');
        int label_len;
        if (dot) {
            label_len = (int)(dot - p);
        } else {
            label_len = (int)strlen(p);
        }
        if (label_len <= 0 || label_len > 63 || pos + label_len + 1 > 450) {
            close(sock);
            return false;
        }
        query[pos++] = (uint8_t)label_len;
        memcpy(query + pos, p, label_len);
        pos += label_len;
        p += label_len;
        if (*p == '.') p++;
    }
    query[pos++] = 0;  // Root label

    // Type A (1), Class IN (1)
    query[pos++] = 0x00; query[pos++] = 0x01;
    query[pos++] = 0x00; query[pos++] = 0x01;

    ssize_t sent = sendto(sock, query, pos, 0,
                          (struct sockaddr*)&sa, sizeof(sa));
    if (sent != pos) {
        close(sock);
        return false;
    }

    uint8_t resp[512];
    ssize_t rcvd = recvfrom(sock, resp, sizeof(resp), 0, nullptr, nullptr);
    close(sock);

    if (rcvd < 12) return false;

    // Check response: same ID, is response, no error
    if (resp[0] != 0xAB || resp[1] != 0xCD) return false;
    if (!(resp[2] & 0x80)) return false;  // Not a response
    int rcode = resp[3] & 0x0F;
    if (rcode != 0) return false;  // Error

    int answers = (resp[6] << 8) | resp[7];
    if (answers == 0) return false;

    // Skip question section
    int rpos = 12;
    // Skip QNAME
    while (rpos < rcvd && resp[rpos] != 0) {
        if ((resp[rpos] & 0xC0) == 0xC0) { rpos += 2; goto skip_done; }
        rpos += resp[rpos] + 1;
    }
    rpos++;  // skip null terminator
skip_done:
    rpos += 4;  // skip QTYPE + QCLASS

    // Parse answer records, find first A record
    for (int i = 0; i < answers && rpos < rcvd - 10; i++) {
        // Skip name (may be compressed)
        if ((resp[rpos] & 0xC0) == 0xC0) {
            rpos += 2;
        } else {
            while (rpos < rcvd && resp[rpos] != 0) {
                if ((resp[rpos] & 0xC0) == 0xC0) { rpos += 2; goto name_done; }
                rpos += resp[rpos] + 1;
            }
            rpos++;
        name_done:;
        }

        if (rpos + 10 > rcvd) return false;

        uint16_t rtype = (resp[rpos] << 8) | resp[rpos+1];
        // uint16_t rclass = (resp[rpos+2] << 8) | resp[rpos+3];
        // uint32_t ttl = ...
        uint16_t rdlength = (resp[rpos+8] << 8) | resp[rpos+9];
        rpos += 10;

        if (rtype == 1 && rdlength == 4 && rpos + 4 <= rcvd) {
            // A record — 4 bytes IPv4
            memcpy(&result->s_addr, resp + rpos, 4);
            return true;
        }
        rpos += rdlength;
    }

    return false;
}

// ── Resolve hostname to IP ───────────────────────────────
// Strategy: 1) if input is already an IP, use it directly
//           2) try UDP DNS via Google/Cloudflare DNS
//           3) fallback to hardcoded IPs for known servers
static bool resolve_host(const char* hostname, struct in_addr* addr) {
    // Check if it's already an IP address
    if (inet_pton(AF_INET, hostname, addr) == 1) {
        return true;
    }

    // Try UDP DNS resolvers
    for (int d = 0; d < NUM_DNS_SERVERS; d++) {
        if (dns_resolve_udp(hostname, g_dns_servers[d], addr)) {
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, addr, ip_str, sizeof(ip_str));
            log_msg("DNS resolved %s -> %s (via %s)",
                    hostname, ip_str, g_dns_servers[d]);
            return true;
        }
    }

    // Fallback: check hardcoded IPs for known servers
    for (int i = 0; i < NUM_FALLBACK_IPS; i++) {
        if (strcmp(hostname, g_fallback_ips[i].hostname) == 0) {
            inet_pton(AF_INET, g_fallback_ips[i].ip, addr);
            log_msg("DNS fallback for %s -> %s", hostname, g_fallback_ips[i].ip);
            return true;
        }
    }

    log_msg("DNS failed for %s: no resolver worked, no fallback", hostname);
    return false;
}

// ── NTP query to a single server ─────────────────────────
// Returns UTC time_t on success, 0 on failure
static time_t ntp_query(const char* server) {
    struct in_addr addr;
    if (!resolve_host(server, &addr)) return 0;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        log_msg("socket() failed: %s", strerror(errno));
        return 0;
    }

    // Set timeout
    struct timeval tv;
    tv.tv_sec = NTP_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(NTP_PORT);
    sa.sin_addr = addr;

    // Build NTP request: LI=0, VN=4, Mode=3 (client)
    ntp_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.li_vn_mode = (0 << 6) | (4 << 3) | 3;

    ssize_t sent = sendto(sock, &pkt, sizeof(pkt), 0,
                          (struct sockaddr*)&sa, sizeof(sa));
    if (sent != sizeof(pkt)) {
        log_msg("sendto %s failed: %s", server, strerror(errno));
        close(sock);
        return 0;
    }

    // Receive response
    ntp_packet_t resp;
    memset(&resp, 0, sizeof(resp));
    ssize_t rcvd = recvfrom(sock, &resp, sizeof(resp), 0, nullptr, nullptr);
    close(sock);

    if (rcvd < (ssize_t)sizeof(resp)) {
        log_msg("NTP response from %s too short or timeout: %zd", server, rcvd);
        return 0;
    }

    // Extract transmit timestamp (seconds since 1900)
    uint32_t ntp_sec = ntohl(resp.tx_ts_sec);
    if (ntp_sec == 0) {
        log_msg("NTP %s returned zero timestamp", server);
        return 0;
    }

    time_t unix_time = (time_t)(ntp_sec - NTP_EPOCH_DELTA);
    log_msg("NTP %s: %ld (UTC)", server, (long)unix_time);
    return unix_time;
}

// ── Try all servers, return first success ────────────────
static time_t ntp_sync() {
    for (int i = 0; i < g_servers.count; i++) {
        log_msg("Trying NTP server: %s", g_servers.servers[i]);
        time_t t = ntp_query(g_servers.servers[i]);
        if (t > 0) return t;
    }
    return 0;
}

// ── Set system time ──────────────────────────────────────
static bool set_system_time(time_t new_time) {
    struct timeval tv;
    tv.tv_sec = new_time;
    tv.tv_usec = 0;
    int rc = settimeofday(&tv, nullptr);
    if (rc != 0) {
        log_msg("settimeofday failed: %s (errno %d)", strerror(errno), errno);
        return false;
    }
    log_msg("System time set to %ld", (long)new_time);
    return true;
}

// ── Check if time looks bad ──────────────────────────────
static bool time_looks_bad(time_t now) {
    time_t build = get_build_time();
    if (now < build) {
        log_msg("System time %ld is before build date %ld — bad",
                (long)now, (long)build);
        return true;
    }
    return false;
}

// ── Main sync logic ──────────────────────────────────────
static void do_sync() {
    time_t now = time(nullptr);
    time_t last_good = load_last_good_time();

    log_msg("=== Sync check: system=%ld last_good=%ld ===", (long)now, (long)last_good);

    bool bad = time_looks_bad(now);
    if (!bad && last_good > 0 && now < last_good) {
        log_msg("System time is behind last known good time — bad");
        bad = true;
    }

    if (!bad) {
        // Time looks OK — just try NTP to keep it accurate
        time_t ntp_time = ntp_sync();
        if (ntp_time > 0) {
            long drift = (long)(now - ntp_time);
            log_msg("Drift from NTP: %ld sec", drift);
            // Only set if drift is more than 2 seconds
            if (drift > 2 || drift < -2) {
                if (set_system_time(ntp_time)) {
                    notify("ps4chrono: time synced");
                    save_last_good_time(ntp_time);
                }
            } else {
                log_msg("Drift is small, no adjustment needed");
                save_last_good_time(now);
            }
        } else {
            log_msg("NTP unreachable, time looks OK, no action");
            save_last_good_time(now);
        }
        return;
    }

    // Time is bad — need to fix it
    log_msg("*** System date is BAD, attempting fix ***");
    notify("ps4chrono: date error");

    time_t ntp_time = ntp_sync();
    if (ntp_time > 0) {
        if (set_system_time(ntp_time)) {
            notify("ps4chrono: time synced");
            save_last_good_time(ntp_time);
            log_msg("Time fixed via NTP");
        }
        return;
    }

    // NTP failed — fallback to last good time + 60 sec
    log_msg("NTP failed, trying fallback...");
    if (last_good > 0 && last_good > get_build_time()) {
        time_t fallback = last_good + 60;
        if (set_system_time(fallback)) {
            notify("ps4chrono: offline time set");
            save_last_good_time(fallback);
            log_msg("Time set from file fallback: %ld", (long)fallback);
        }
    } else {
        log_msg("No valid fallback available, cannot fix time");
        notify("ps4chrono: sync failed");
    }
}

// ── Single instance ──────────────────────────────────────
static int g_lock_fd = -1;

static bool acquire_lock() {
    g_lock_fd = open(LOCK_FILE, O_CREAT | O_RDWR, 0666);
    if (g_lock_fd < 0) return false;

    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;

    if (fcntl(g_lock_fd, F_SETLK, &fl) < 0) {
        close(g_lock_fd);
        g_lock_fd = -1;
        return false;
    }
    return true;
}

static void request_quit_old_instance() {
    // Create quit flag
    FILE* f = fopen(QUIT_FLAG, "w");
    if (f) {
        fprintf(f, "quit\n");
        fclose(f);
    }
}

static bool should_quit() {
    return file_exists(QUIT_FLAG);
}

// ── Web interface (minimal, Phase 1) ─────────────────────
// Simple HTTP server on port 8585 — non-blocking check
static int g_web_sock = -1;

static bool init_web_server() {
    g_web_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_web_sock < 0) {
        log_msg("Web socket failed: %s", strerror(errno));
        return false;
    }

    int opt = 1;
    setsockopt(g_web_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(g_web_sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    // Non-blocking
    int flags = fcntl(g_web_sock, F_GETFL, 0);
    fcntl(g_web_sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = htons(WEB_PORT);

    if (bind(g_web_sock, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        log_msg("Web bind failed: %s", strerror(errno));
        close(g_web_sock);
        g_web_sock = -1;
        return false;
    }

    if (listen(g_web_sock, 4) < 0) {
        log_msg("Web listen failed: %s", strerror(errno));
        close(g_web_sock);
        g_web_sock = -1;
        return false;
    }

    log_msg("Web server started on port %d", WEB_PORT);
    return true;
}

// Format time for display
static void format_time_str(time_t t, char* buf, size_t len) {
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S UTC", &tm);
}

// Build status page HTML
static int build_status_page(char* buf, size_t len) {
    time_t now = time(nullptr);
    time_t last_good = load_last_good_time();
    char now_str[64], lg_str[64];
    format_time_str(now, now_str, sizeof(now_str));
    format_time_str(last_good, lg_str, sizeof(lg_str));

    char servers_html[2048] = "";
    int pos = 0;
    for (int i = 0; i < g_servers.count; i++) {
        pos += snprintf(servers_html + pos, sizeof(servers_html) - pos,
            "<tr><td>%d</td><td>%s</td>"
            "<td><a href='/delete?idx=%d' style='color:#e74c3c'>remove</a></td></tr>",
            i + 1, g_servers.servers[i], i);
    }

    // Interval options
    const int intervals[] = {3600, 10800, 21600, 43200, 86400};
    const char* labels[] = {"1 hour", "3 hours", "6 hours", "12 hours", "24 hours"};
    char interval_opts[512] = "";
    int ipos = 0;
    for (int i = 0; i < 5; i++) {
        ipos += snprintf(interval_opts + ipos, sizeof(interval_opts) - ipos,
            "<option value='%d'%s>%s</option>",
            intervals[i],
            (intervals[i] == g_check_interval_sec) ? " selected" : "",
            labels[i]);
    }

    return snprintf(buf, len,
        "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n\r\n"
        "<!DOCTYPE html><html><head><title>ps4chrono</title>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<style>"
        "body{font-family:sans-serif;background:#1a1a2e;color:#e0e0e0;margin:0;padding:20px}"
        "h1{color:#0ff;font-size:1.5em}"
        ".box{background:#16213e;border-radius:8px;padding:15px;margin:10px 0}"
        "table{width:100%%;border-collapse:collapse}"
        "td{padding:6px;border-bottom:1px solid #333}"
        "input,select,button{background:#0d1b2a;color:#e0e0e0;border:1px solid #444;"
        "padding:8px;border-radius:4px;margin:4px}"
        "button{background:#0ff;color:#000;cursor:pointer;font-weight:bold}"
        "a{color:#0ff}"
        ".footer{margin-top:20px;font-size:0.8em;color:#666;text-align:center}"
        "</style></head><body>"
        "<h1>&#128336; ps4chrono v" APP_VERSION "</h1>"
        "<div class='box'>"
        "<b>System time:</b> %s<br>"
        "<b>Last good time:</b> %s<br>"
        "<b>Check interval:</b> %d sec"
        "</div>"
        "<div class='box'><h3>NTP Servers</h3>"
        "<table>%s</table>"
        "<form method='GET' action='/add'>"
        "<input type='text' name='s' placeholder='server address' size='30'>"
        "<button type='submit'>Add</button></form></div>"
        "<div class='box'><h3>Check interval</h3>"
        "<form method='GET' action='/interval'>"
        "<select name='v'>%s</select>"
        "<button type='submit'>Save</button></form></div>"
        "<div class='box'><h3>Set time manually</h3>"
        "<form method='GET' action='/settime'>"
        "<input type='text' name='t' placeholder='YYYY-MM-DD HH:MM:SS' size='20'>"
        " (UTC)<button type='submit'>Set</button></form></div>"
        "<div class='box'>"
        "<a href='/sync'>&#9889; Sync now</a> &nbsp; "
        "<a href='/'>&#128260; Refresh</a> &nbsp; "
        "<a href='/quit' style='color:#f44'>&#9724; Stop</a></div>"
        "<div class='footer'>Created by SergioPoverony and Mr.Claude</div>"
        "</body></html>",
        now_str, lg_str, g_check_interval_sec,
        servers_html, interval_opts);
}

// URL decode helper (minimal: just '+' and %%XX)
static void url_decode(const char* src, char* dst, size_t len) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < len - 1; i++) {
        if (src[i] == '+') {
            dst[j++] = ' ';
        } else if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = {src[i+1], src[i+2], 0};
            dst[j++] = (char)strtol(hex, nullptr, 16);
            i += 2;
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

// Extract query param value
static bool get_param(const char* url, const char* key, char* val, size_t vlen) {
    char search[64];
    snprintf(search, sizeof(search), "%s=", key);
    const char* p = strstr(url, search);
    if (!p) return false;
    p += strlen(search);
    size_t i = 0;
    while (p[i] && p[i] != '&' && p[i] != ' ' && p[i] != '\r' && i < vlen - 1) {
        i++;
    }
    char encoded[256];
    snprintf(encoded, sizeof(encoded), "%.*s", (int)i, p);
    url_decode(encoded, val, vlen);
    return true;
}

// Handle one HTTP request, return true if sync was requested
static bool handle_web_request(int client) {
    char req[2048];
    ssize_t n = recv(client, req, sizeof(req) - 1, 0);
    if (n <= 0) { close(client); return false; }
    req[n] = '\0';

    bool need_sync = false;

    // Parse first line: GET /path HTTP/1.x
    if (strncmp(req, "GET /add", 8) == 0) {
        char server[128];
        if (get_param(req, "s", server, sizeof(server)) && server[0] &&
            g_servers.count < MAX_NTP_SERVERS) {
            // check for duplicates
            bool dup = false;
            for (int i = 0; i < g_servers.count; i++) {
                if (strcasecmp(g_servers.servers[i], server) == 0) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                // shift existing servers down, insert new at top (highest priority)
                for (int i = g_servers.count; i > 0; i--)
                    memcpy(g_servers.servers[i], g_servers.servers[i - 1],
                           sizeof(g_servers.servers[0]));
                strncpy(g_servers.servers[0], server,
                        sizeof(g_servers.servers[0]) - 1);
                g_servers.servers[0][sizeof(g_servers.servers[0]) - 1] = '\0';
                g_servers.count++;
                save_config();
                log_msg("Added server: %s (priority 1)", server);
            } else {
                log_msg("Server %s already in list, skipping", server);
            }
        }
    } else if (strncmp(req, "GET /delete", 11) == 0) {
        char idx_str[16];
        if (get_param(req, "idx", idx_str, sizeof(idx_str))) {
            int idx = atoi(idx_str);
            if (idx >= 0 && idx < g_servers.count) {
                log_msg("Removed server: %s", g_servers.servers[idx]);
                for (int i = idx; i < g_servers.count - 1; i++) {
                    memcpy(g_servers.servers[i], g_servers.servers[i+1],
                           sizeof(g_servers.servers[0]));
                }
                g_servers.count--;
                save_config();
            }
        }
    } else if (strncmp(req, "GET /interval", 13) == 0) {
        char val[16];
        if (get_param(req, "v", val, sizeof(val))) {
            int v = atoi(val);
            if (v >= 60) {
                g_check_interval_sec = v;
                save_config();
                log_msg("Interval set to %d sec", v);
            }
        }
    } else if (strncmp(req, "GET /settime", 12) == 0) {
        char tstr[32];
        if (get_param(req, "t", tstr, sizeof(tstr))) {
            struct tm tm;
            memset(&tm, 0, sizeof(tm));
            if (sscanf(tstr, "%d-%d-%d %d:%d:%d",
                       &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                       &tm.tm_hour, &tm.tm_min, &tm.tm_sec) >= 3) {
                tm.tm_year -= 1900;
                tm.tm_mon -= 1;
                time_t t = timegm(&tm);
                if (t > get_build_time()) {
                    set_system_time(t);
                    save_last_good_time(t);
                    notify("ps4chrono: set manually");
                    log_msg("Manual time set: %ld", (long)t);
                }
            }
        }
    } else if (strncmp(req, "GET /sync", 9) == 0) {
        need_sync = true;
    } else if (strncmp(req, "GET /quit", 9) == 0) {
        log_msg("Quit requested via web interface");
        FILE* qf = fopen(QUIT_FLAG, "w");
        if (qf) fclose(qf);
    }

    // Serve notification icon
    if (strncmp(req, "GET /notify.png", 15) == 0) {
        char hdr[128];
        int hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.0 200 OK\r\nContent-Type: image/png\r\n"
            "Content-Length: %u\r\n\r\n", g_icon_png_size);
        send(client, hdr, hlen, 0);
        send(client, (const char*)g_icon_png, g_icon_png_size, 0);
        close(client);
        return false;
    }

    // Respond to client
    char response[8192];
    if (strncmp(req, "GET / ", 6) == 0 || strncmp(req, "GET /? ", 7) == 0 ||
        strncmp(req, "GET / HTTP", 10) == 0) {
        build_status_page(response, sizeof(response));
    } else if (strncmp(req, "GET /quit", 9) == 0) {
        snprintf(response, sizeof(response),
            "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n\r\n"
            "<!DOCTYPE html><html><head><title>ps4chrono</title>"
            "<style>body{font-family:sans-serif;background:#1a1a2e;color:#e0e0e0;"
            "display:flex;justify-content:center;align-items:center;height:100vh;margin:0}"
            "</style></head><body>"
            "<h2>ps4chrono stopped</h2>"
            "</body></html>");
    } else if (need_sync) {
        // Sync takes time — show a "please wait" page instead of 302
        // so the browser doesn't hang waiting for a redirect target
        snprintf(response, sizeof(response),
            "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n\r\n"
            "<!DOCTYPE html><html><head><title>ps4chrono</title>"
            "<meta http-equiv='refresh' content='5;url=/'>"
            "<style>body{font-family:sans-serif;background:#1a1a2e;color:#e0e0e0;"
            "display:flex;justify-content:center;align-items:center;height:100vh;margin:0}"
            "</style></head><body>"
            "<h2>&#9889; Syncing... please wait</h2>"
            "</body></html>");
    } else {
        // For add/delete/interval/settime — instant redirect, these are fast
        snprintf(response, sizeof(response),
                 "HTTP/1.0 302 Found\r\nLocation: /\r\n\r\n");
    }

    send(client, response, strlen(response), 0);
    close(client);
    return need_sync;
}

// Drain all pending web requests (non-blocking).
// Returns true if any request asked for sync.
static bool drain_web() {
    if (g_web_sock < 0) return false;

    bool need_sync = false;
    for (int i = 0; i < 8; i++) {  // handle up to 8 queued requests
        struct sockaddr_in ca;
        socklen_t cl = sizeof(ca);
        int client = accept(g_web_sock, (struct sockaddr*)&ca, &cl);
        if (client < 0) break;  // no more pending

        struct timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (handle_web_request(client))
            need_sync = true;
    }
    return need_sync;
}

// ── Main ─────────────────────────────────────────────────
int main() {
    ensure_dir(DATA_DIR);

    // Open log
    g_logfp = fopen(LOG_FILE, "a");
    log_msg("========================================");
    log_msg("ps4chrono v" APP_VERSION " starting");

    // Single instance
    request_quit_old_instance();

    // Wait for old instance to exit (up to 90 sec)
    for (int i = 0; i < 90 && !acquire_lock(); i++) {
        sleep(1);
    }
    if (g_lock_fd < 0) {
        log_msg("Could not acquire lock, exiting");
        if (g_logfp) fclose(g_logfp);
        return 1;
    }

    // Remove quit flag if we created it
    remove_file(QUIT_FLAG);

    log_msg("Lock acquired, we are the active instance");

    // Load config
    load_config();

    // Init web server
    init_web_server();

    // Set notify icon URL (PS4 loads icon via HTTP from our web server)
    snprintf(g_notify_icon_url, sizeof(g_notify_icon_url),
             "http://127.0.0.1:%d/notify.png", WEB_PORT);
    log_msg("Notify icon URL: %s", g_notify_icon_url);

    // Initial sync
    do_sync();

    // Main loop
    time_t last_check = time(nullptr);

    bool sync_requested = false;

    while (!should_quit()) {
        // Drain all pending web requests first (fast, non-blocking)
        if (drain_web())
            sync_requested = true;

        // Do sync only after all web clients have been served
        if (sync_requested) {
            do_sync();
            last_check = time(nullptr);
            sync_requested = false;
            // Drain again in case requests arrived during sync
            drain_web();
            continue;
        }

        // Periodic sync
        time_t now = time(nullptr);
        if (now - last_check >= g_check_interval_sec) {
            log_msg("Periodic check triggered");
            do_sync();
            last_check = time(nullptr);
            // Drain web after periodic sync too
            drain_web();
        }

        usleep(200000); // 200ms sleep to not burn CPU
    }

    log_msg("Quit flag detected, shutting down");
    remove_file(QUIT_FLAG);

    if (g_web_sock >= 0) close(g_web_sock);
    if (g_lock_fd >= 0) close(g_lock_fd);
    remove_file(LOCK_FILE);

    log_msg("ps4chrono stopped");
    if (g_logfp) fclose(g_logfp);
    return 0;
}
