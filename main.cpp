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
#include <pthread.h>
#include <signal.h>
#include <strings.h>
#include <sys/select.h>
#include <atomic>

#include "icon_data.h"

// ── Version ──────────────────────────────────────────────
#define APP_VERSION "0.2.2"
#define APP_NAME    "ps4chrono"

// Build timestamp as fallback minimum time (compiled into binary)
#define BUILD_YEAR  2026
#define BUILD_MONTH 10
#define BUILD_DAY   14

// ── Paths ────────────────────────────────────────────────
// HOST_TEST: build on a PC (g++) for testing, with data in /tmp
#ifdef HOST_TEST
#define DATA_DIR     "/tmp/ps4chrono_test"
#else
#define DATA_DIR     "/data/ps4chrono"
#endif
#define TIME_FILE    DATA_DIR "/lastgood.dat"
#define CONFIG_FILE  DATA_DIR "/config.dat"
#define LOG_FILE     DATA_DIR "/ps4chrono.log"
#define QUIT_FLAG    DATA_DIR "/quit.flag"   // a new copy creates it to ask the running copy to quit
#define OLD_LOCK_FILE DATA_DIR "/lock"       // used by v0.2.1 and older, removed at start

// ── Defaults ─────────────────────────────────────────────
#define DEFAULT_CHECK_INTERVAL_SEC (6 * 3600)  // 6 hours
#define NTP_PORT           123
#define NTP_PACKET_SIZE    48
#define NTP_TIMEOUT_SEC    2
#define WEB_PORT           8585
#define MAX_NTP_SERVERS    8
#define TAKEOVER_WAIT_SEC  60   // how long a new copy waits for the old one to quit
#define MAX_WEB_CLIENTS    8    // simultaneous web connections
#define RETRY_SEC          30   // retry while time is bad or only set from file

// ── Shared state between threads ─────────────────────────
// Main thread: sync. Web thread: accepts connections, one short thread per request.
static pthread_mutex_t g_log_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_cfg_mx = PTHREAD_MUTEX_INITIALIZER;   // servers, interval, config file
static std::atomic<bool> g_quit(false);
static std::atomic<bool> g_sync_req(false);     // "Sync now" pressed
static std::atomic<bool> g_reload_req(false);   // "Restart" pressed: re-read config from disk
static std::atomic<bool> g_syncing(false);
static std::atomic<int>  g_web_clients(0);

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

    pthread_mutex_lock(&g_log_mx);
    if (g_logfp) {
        fprintf(g_logfp, "[%s] %s\n", ts, buf);
        fflush(g_logfp);
    }
    pthread_mutex_unlock(&g_log_mx);
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

#ifdef HOST_TEST
static int sceKernelSendNotificationRequest(int, notify_request_t* r, size_t, int) {
    log_msg("[notify] %s", r->message);
    return 0;
}
#else
extern "C" int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);
#endif

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

// Active server list (protected by g_cfg_mx)
struct server_list_t {
    char servers[MAX_NTP_SERVERS][128];
    int  count;
};

static server_list_t g_servers;
static int g_check_interval_sec = DEFAULT_CHECK_INTERVAL_SEC;

static int get_interval() {
    pthread_mutex_lock(&g_cfg_mx);
    int v = g_check_interval_sec;
    pthread_mutex_unlock(&g_cfg_mx);
    return v;
}

// ── Server name cleanup ──────────────────────────────────
// Turns what a user may type into a plain host name or IP:
//   " 192.168.1.1:123 "  -> "192.168.1.1"
//   "ntp://time.google.com/" -> "time.google.com"
// Returns false if the result is empty or has characters a host name can't have.
static bool clean_server_name(const char* in, char* out, size_t outlen) {
    // trim spaces at both ends
    while (*in == ' ' || *in == '\t') in++;
    size_t len = strlen(in);
    while (len > 0 && (in[len - 1] == ' ' || in[len - 1] == '\t')) len--;

    // skip scheme like http:// ntp:// udp://
    for (size_t i = 0; i + 2 < len; i++) {
        if (in[i] == ':' && in[i + 1] == '/' && in[i + 2] == '/') {
            in += i + 3;
            len -= i + 3;
            break;
        }
    }

    size_t j = 0;
    for (size_t i = 0; i < len && j < outlen - 1; i++) {
        char c = in[i];
        if (c == ':' || c == '/' || c == '?') break;   // port or path
        out[j++] = c;   // spaces inside the name stay and fail the check below
    }
    out[j] = '\0';

    if (j == 0) return false;
    for (size_t i = 0; i < j; i++) {
        char c = out[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

// Adds a server to the list (caller holds g_cfg_mx).
// at_top: user-added servers get the highest priority.
// Returns: 1 added, 0 duplicate, -1 list full
static int add_server_locked(const char* name, bool at_top) {
    for (int i = 0; i < g_servers.count; i++)
        if (strcasecmp(g_servers.servers[i], name) == 0) return 0;
    if (g_servers.count >= MAX_NTP_SERVERS) return -1;
    if (at_top) {
        for (int i = g_servers.count; i > 0; i--)
            memcpy(g_servers.servers[i], g_servers.servers[i - 1], sizeof(g_servers.servers[0]));
        snprintf(g_servers.servers[0], sizeof(g_servers.servers[0]), "%s", name);
    } else {
        snprintf(g_servers.servers[g_servers.count], sizeof(g_servers.servers[0]), "%s", name);
    }
    g_servers.count++;
    return 1;
}

static void load_defaults_locked() {
    g_servers.count = 0;
    for (int i = 0; i < NUM_DEFAULT_SERVERS; i++)
        add_server_locked(g_default_servers[i], false);
}

// ── Config persistence ───────────────────────────────────
// Caller holds g_cfg_mx.
static void save_config_locked() {
    FILE* f = fopen(CONFIG_FILE, "w");
    if (!f) {
        log_msg("ERROR: cannot write config: %s", strerror(errno));
        return;
    }
    fprintf(f, "# ps4chrono settings. Lines starting with # are ignored.\n");
    fprintf(f, "# After editing, press Restart on the web page (http://PS4_IP:%d)\n", WEB_PORT);
    fprintf(f, "# or send ps4chrono.elf again.\n");
    fprintf(f, "#\n");
    fprintf(f, "# interval = seconds between checks (3600 = 1 hour, 21600 = 6 hours, minimum 60)\n");
    fprintf(f, "interval=%d\n", g_check_interval_sec);
    fprintf(f, "#\n");
    fprintf(f, "# server = NTP server name or IP, up to %d. The first one is checked first.\n",
            MAX_NTP_SERVERS);
    for (int i = 0; i < g_servers.count; i++) {
        fprintf(f, "server=%s\n", g_servers.servers[i]);
    }
    fclose(f);
    log_msg("Config saved");
}

static void load_config() {
    pthread_mutex_lock(&g_cfg_mx);
    g_servers.count = 0;
    g_check_interval_sec = DEFAULT_CHECK_INTERVAL_SEC;

    FILE* f = fopen(CONFIG_FILE, "r");
    if (!f) {
        load_defaults_locked();
        log_msg("No config file, creating it with %d default servers", g_servers.count);
        save_config_locked();
        pthread_mutex_unlock(&g_cfg_mx);
        return;
    }

    bool changed = false;   // file had something we fixed: write it back clean
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char* cr = strchr(line, '\r');
        if (cr) *cr = '\0';

        if (strncmp(line, "server=", 7) == 0) {
            char name[128];
            if (!clean_server_name(line + 7, name, sizeof(name))) {
                log_msg("Config: bad server '%s' ignored", line + 7);
                changed = true;
                continue;
            }
            if (strcmp(name, line + 7) != 0) {
                log_msg("Config: server '%s' -> '%s'", line + 7, name);
                changed = true;
            }
            int r = add_server_locked(name, false);
            if (r == 0) { log_msg("Config: duplicate server %s ignored", name); changed = true; }
            if (r < 0)  { log_msg("Config: more than %d servers, %s ignored", MAX_NTP_SERVERS, name); changed = true; }
        } else if (strncmp(line, "interval=", 9) == 0) {
            int val = atoi(line + 9);
            if (val >= 60) g_check_interval_sec = val;
            else { log_msg("Config: interval '%s' too small, using %d", line + 9, g_check_interval_sec); changed = true; }
        }
    }
    fclose(f);

    if (g_servers.count == 0) {
        log_msg("Config: no servers, using defaults");
        load_defaults_locked();
        changed = true;
    }
    log_msg("Config loaded: %d servers, interval %d sec",
            g_servers.count, g_check_interval_sec);
    if (changed) save_config_locked();
    pthread_mutex_unlock(&g_cfg_mx);
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
static bool quit_requested();

static time_t ntp_sync() {
    // Copy the list: the web thread may change it while we wait for servers
    server_list_t list;
    pthread_mutex_lock(&g_cfg_mx);
    memcpy(&list, &g_servers, sizeof(list));
    pthread_mutex_unlock(&g_cfg_mx);

    for (int i = 0; i < list.count; i++) {
        if (quit_requested()) return 0;   // a new copy is waiting for us to exit
        log_msg("Trying NTP server: %s", list.servers[i]);
        time_t t = ntp_query(list.servers[i]);
        if (t > 0) return t;
    }
    return 0;
}

// ── Set system time ──────────────────────────────────────
static bool set_system_time(time_t new_time) {
    struct timeval tv;
    tv.tv_sec = new_time;
    tv.tv_usec = 0;
#ifdef HOST_TEST
    int rc = 0;   // PC test: don't touch the real clock
    (void)tv;
#else
    int rc = settimeofday(&tv, nullptr);
#endif
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
// Returns true when time is fine and confirmed by NTP (or NTP is just
// unreachable but the clock looks OK). Returns false when the clock is
// still bad or was only set from the saved file: then main loop retries soon.
// Retries don't repeat notifications: each one is shown once per problem.
static bool s_offline = false;   // clock was set from lastgood.dat, NTP not confirmed yet
static bool s_warned  = false;   // "date error"/"sync failed" already shown for this problem

static bool do_sync() {
    g_syncing = true;
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
            s_offline = false;
            s_warned = false;
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
            if (s_offline) {
                // time came from the file, keep trying NTP
                g_syncing = false;
                return false;
            }
        }
        g_syncing = false;
        return true;
    }

    // Time is bad — need to fix it
    log_msg("*** System date is BAD, attempting fix ***");
    if (!s_warned) notify("ps4chrono: date error");

    time_t ntp_time = ntp_sync();
    if (ntp_time > 0) {
        if (set_system_time(ntp_time)) {
            notify("ps4chrono: time synced");
            save_last_good_time(ntp_time);
            log_msg("Time fixed via NTP");
            s_offline = false;
            s_warned = false;
            g_syncing = false;
            return true;
        }
        g_syncing = false;
        return false;
    }
    if (quit_requested()) { g_syncing = false; return false; }

    // NTP failed — fallback to last good time + 60 sec
    log_msg("NTP failed, trying fallback...");
    if (last_good > 0 && last_good > get_build_time()) {
        time_t fallback = last_good + 60;
        if (set_system_time(fallback)) {
            if (!s_offline) notify("ps4chrono: offline time set");
            save_last_good_time(fallback);
            log_msg("Time set from file fallback: %ld", (long)fallback);
            s_offline = true;
        }
    } else {
        log_msg("No valid fallback available, cannot fix time");
        if (!s_warned) notify("ps4chrono: sync failed");
    }
    s_warned = true;
    g_syncing = false;
    return false;
}

// ── Single instance ──────────────────────────────────────
// Same way as ps4torrent: the web port is the "lock". If it's busy, another
// copy is running: we create QUIT_FLAG, it notices and exits, the port frees up.
static bool quit_requested() {
    if (g_quit.load()) return true;
    if (file_exists(QUIT_FLAG)) {
        log_msg("Another copy asked us to quit (quit.flag)");
        g_quit = true;
        return true;
    }
    return false;
}

// ── Monotonic clock ──────────────────────────────────────
// Check interval is counted by this clock, so setting the system time
// (1970 -> 2026 or backwards) doesn't fire or delay the periodic check.
static long mono_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec;
}

// ── Web server ───────────────────────────────────────────
// Runs in its own thread, so the page works while a sync is waiting for servers.
static std::atomic<int> g_srv(-1);   // listening socket

static int web_listen_socket(int* err) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { *err = errno; return -1; }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    // no SO_REUSEPORT: a second copy must get "address in use"

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
#ifndef __linux__
    sa.sin_len = sizeof(sa);
#endif
    sa.sin_family = AF_INET;
    sa.sin_port = htons(WEB_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr*)&sa, sizeof(sa)) < 0) { *err = errno; close(s); return -1; }
    if (listen(s, 8) < 0) { *err = errno; close(s); return -1; }
    return s;
}

// Recreate the listening socket (after rest mode accept() can keep failing).
// Must be called from the web thread.
static bool web_recreate() {
    int old = g_srv.exchange(-1);
    if (old >= 0) close(old);
    int err = 0;
    int ns = web_listen_socket(&err);
    for (int i = 0; i < 10 && ns < 0; i++) { usleep(100000); ns = web_listen_socket(&err); }
    if (ns < 0) {
        log_msg("Web: cannot recreate socket (errno %d)", err);
        return false;
    }
    g_srv = ns;
    return true;
}

static void send_all(int fd, const char* data, size_t len) {
    while (len > 0) {
        ssize_t n = send(fd, data, len, 0);
        if (n <= 0) return;
        data += n;
        len -= (size_t)n;
    }
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
    char interval_opts[640] = "";
    int interval;

    pthread_mutex_lock(&g_cfg_mx);
    interval = g_check_interval_sec;
    int pos = 0;
    for (int i = 0; i < g_servers.count; i++) {
        pos += snprintf(servers_html + pos, sizeof(servers_html) - pos,
            "<tr><td>%d</td><td>%s</td>"
            "<td><a href='/delete?idx=%d' style='color:#e74c3c'>remove</a></td></tr>",
            i + 1, g_servers.servers[i], i);
        if (pos >= (int)sizeof(servers_html)) break;
    }
    pthread_mutex_unlock(&g_cfg_mx);

    // Interval options
    const int intervals[] = {3600, 10800, 21600, 43200, 86400};
    const char* labels[] = {"1 hour", "3 hours", "6 hours", "12 hours", "24 hours"};
    int ipos = 0;
    bool found = false;
    for (int i = 0; i < 5; i++) {
        if (intervals[i] == interval) found = true;
        ipos += snprintf(interval_opts + ipos, sizeof(interval_opts) - ipos,
            "<option value='%d'%s>%s</option>",
            intervals[i],
            (intervals[i] == interval) ? " selected" : "",
            labels[i]);
    }
    if (!found) {
        // value typed into config.dat by hand
        snprintf(interval_opts + ipos, sizeof(interval_opts) - ipos,
            "<option value='%d' selected>%d sec (config.dat)</option>", interval, interval);
    }

    return snprintf(buf, len,
        "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Cache-Control: no-store\r\n\r\n"
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
        "<b>System time:</b> %s%s<br>"
        "<b>Last good time:</b> %s<br>"
        "<b>Check interval:</b> %d sec<br>"
        "<b>Config file:</b> " CONFIG_FILE
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
        "<a href='/restart'>&#8635; Restart</a> &nbsp; "
        "<a href='/quit' style='color:#f44'>&#9724; Stop</a></div>"
        "<div class='footer'>Created by SergioPoverony and Mr.Claude</div>"
        "</body></html>",
        now_str, g_syncing.load() ? " &nbsp;<i>(syncing...)</i>" : "",
        lg_str, interval, servers_html, interval_opts);
}

static void build_message_page(char* buf, size_t len, const char* msg, int refresh_sec) {
    char refresh[96] = "";
    if (refresh_sec > 0)
        snprintf(refresh, sizeof(refresh),
                 "<meta http-equiv='refresh' content='%d;url=/'>", refresh_sec);
    snprintf(buf, len,
        "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Cache-Control: no-store\r\n\r\n"
        "<!DOCTYPE html><html><head><title>ps4chrono</title>%s"
        "<style>body{font-family:sans-serif;background:#1a1a2e;color:#e0e0e0;"
        "display:flex;justify-content:center;align-items:center;height:100vh;margin:0}"
        "</style></head><body><h2>%s</h2></body></html>",
        refresh, msg);
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
    while (p[i] && p[i] != '&' && p[i] != ' ' && p[i] != '\r' && i < 255) {
        i++;
    }
    char encoded[256];
    snprintf(encoded, sizeof(encoded), "%.*s", (int)i, p);
    url_decode(encoded, val, vlen);
    return true;
}

// "GET /path?..." or "GET /path HTTP" — exact path match
static bool path_is(const char* req, const char* path) {
    if (strncmp(req, "GET ", 4) != 0) return false;
    size_t n = strlen(path);
    if (strncmp(req + 4, path, n) != 0) return false;
    char c = req[4 + n];
    return c == ' ' || c == '?';
}

// Handle one HTTP request (runs in its own short thread)
static void handle_web_request(int client) {
    // Read until end of headers (a request can arrive in several pieces)
    char req[2048];
    size_t n = 0;
    while (n < sizeof(req) - 1) {
        ssize_t r = recv(client, req + n, sizeof(req) - 1 - n, 0);
        if (r <= 0) break;
        n += (size_t)r;
        req[n] = '\0';
        if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n")) break;
    }
    if (n == 0) return;
    req[n] = '\0';

    static char response[8192];   // only used under resp_mx
    static pthread_mutex_t resp_mx = PTHREAD_MUTEX_INITIALIZER;

    // Notification icon (PS4 loads it from us by HTTP)
    if (path_is(req, "/notify.png")) {
        char hdr[128];
        int hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.0 200 OK\r\nContent-Type: image/png\r\n"
            "Content-Length: %u\r\n\r\n", (unsigned)g_icon_png_size);
        send_all(client, hdr, hlen);
        send_all(client, (const char*)g_icon_png, g_icon_png_size);
        return;
    }

    const char* redirect = "HTTP/1.0 302 Found\r\nLocation: /\r\n\r\n";

    if (path_is(req, "/add")) {
        char raw[128], server[128];
        if (get_param(req, "s", raw, sizeof(raw)) && raw[0]) {
            if (!clean_server_name(raw, server, sizeof(server))) {
                log_msg("Add server: '%s' is not a valid name, ignored", raw);
            } else {
                if (strcmp(raw, server) != 0)
                    log_msg("Add server: '%s' cleaned to '%s'", raw, server);
                pthread_mutex_lock(&g_cfg_mx);
                int r = add_server_locked(server, true);
                if (r == 1) save_config_locked();
                pthread_mutex_unlock(&g_cfg_mx);
                if (r == 1)      log_msg("Added server: %s (priority 1)", server);
                else if (r == 0) log_msg("Server %s already in list, skipping", server);
                else             log_msg("Server list is full (%d), %s not added", MAX_NTP_SERVERS, server);
            }
        }
        send_all(client, redirect, strlen(redirect));
    } else if (path_is(req, "/delete")) {
        char idx_str[16];
        if (get_param(req, "idx", idx_str, sizeof(idx_str))) {
            int idx = atoi(idx_str);
            pthread_mutex_lock(&g_cfg_mx);
            if (idx >= 0 && idx < g_servers.count) {
                log_msg("Removed server: %s", g_servers.servers[idx]);
                for (int i = idx; i < g_servers.count - 1; i++)
                    memcpy(g_servers.servers[i], g_servers.servers[i + 1], sizeof(g_servers.servers[0]));
                g_servers.count--;
                if (g_servers.count == 0) {
                    log_msg("Last server removed, restoring defaults");
                    load_defaults_locked();
                }
                save_config_locked();
            }
            pthread_mutex_unlock(&g_cfg_mx);
        }
        send_all(client, redirect, strlen(redirect));
    } else if (path_is(req, "/interval")) {
        char val[16];
        if (get_param(req, "v", val, sizeof(val))) {
            int v = atoi(val);
            if (v >= 60) {
                pthread_mutex_lock(&g_cfg_mx);
                g_check_interval_sec = v;
                save_config_locked();
                pthread_mutex_unlock(&g_cfg_mx);
                log_msg("Interval set to %d sec", v);
            }
        }
        send_all(client, redirect, strlen(redirect));
    } else if (path_is(req, "/settime")) {
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
        send_all(client, redirect, strlen(redirect));
    } else if (path_is(req, "/sync")) {
        g_sync_req = true;
        pthread_mutex_lock(&resp_mx);
        build_message_page(response, sizeof(response), "&#9889; Syncing... please wait", 5);
        send_all(client, response, strlen(response));
        pthread_mutex_unlock(&resp_mx);
    } else if (path_is(req, "/restart")) {
        log_msg("Restart requested via web interface");
        g_reload_req = true;
        pthread_mutex_lock(&resp_mx);
        build_message_page(response, sizeof(response),
                           "&#8635; Restarting: reading config.dat...", 3);
        send_all(client, response, strlen(response));
        pthread_mutex_unlock(&resp_mx);
    } else if (path_is(req, "/quit")) {
        log_msg("Quit requested via web interface");
        pthread_mutex_lock(&resp_mx);
        build_message_page(response, sizeof(response), "ps4chrono stopped", 0);
        send_all(client, response, strlen(response));
        pthread_mutex_unlock(&resp_mx);
        g_quit = true;   // after the answer is sent
    } else if (path_is(req, "/")) {
        pthread_mutex_lock(&resp_mx);
        build_status_page(response, sizeof(response));
        send_all(client, response, strlen(response));
        pthread_mutex_unlock(&resp_mx);
    } else {
        const char* nf = "HTTP/1.0 404 Not Found\r\nContent-Type: text/plain\r\n\r\nnot found\n";
        send_all(client, nf, strlen(nf));
    }
}

static void* client_thread(void* arg) {
    int fd = (int)(intptr_t)arg;
    handle_web_request(fd);
    close(fd);
    g_web_clients.fetch_sub(1);
    return nullptr;
}

static void* web_thread(void*) {
    unsigned accept_fails = 0, select_fails = 0;
    time_t last_heal_log = 0;

    while (!g_quit.load()) {
        int s = g_srv.load();
        if (s < 0) {
            usleep(500000);
            web_recreate();
            continue;
        }
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(s, &rf);
        struct timeval tv = {0, 200000};
        int r = select(s + 1, &rf, nullptr, nullptr, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (++select_fails >= 10) {
                time_t now = time(nullptr);
                if (now - last_heal_log >= 10) { last_heal_log = now; log_msg("Web: select keeps failing (errno %d), recreating socket", errno); }
                web_recreate();
                select_fails = 0;
            } else {
                usleep(100000);
            }
            continue;
        }
        select_fails = 0;
        if (r == 0) continue;

        struct sockaddr_in ca;
        socklen_t cl = sizeof(ca);
        int c = accept(s, (struct sockaddr*)&ca, &cl);
        if (c < 0) {
            int e = errno;
            if (e == EINTR || e == ECONNABORTED || e == EAGAIN || e == EWOULDBLOCK) continue;
            if (e == EMFILE || e == ENFILE) { usleep(200000); continue; }
            if (++accept_fails >= 3) {
                // after rest mode the socket can break: make a new one
                time_t now = time(nullptr);
                if (now - last_heal_log >= 10) { last_heal_log = now; log_msg("Web: accept keeps failing (errno %d), recreating socket", e); }
                web_recreate();
                accept_fails = 0;
            } else {
                usleep(100000);
            }
            continue;
        }
        accept_fails = 0;

        // Blocking socket with timeouts: a slow browser can't hang us forever
        int fl = fcntl(c, F_GETFL, 0);
        if (fl >= 0) fcntl(c, F_SETFL, fl & ~O_NONBLOCK);
        struct timeval to = {5, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));

        if (g_web_clients.fetch_add(1) >= MAX_WEB_CLIENTS) {
            g_web_clients.fetch_sub(1);
            close(c);
            continue;
        }
        // One short thread per request: browsers open idle connections in advance,
        // they must not block the others
        pthread_t th;
        if (pthread_create(&th, nullptr, client_thread, (void*)(intptr_t)c) == 0) {
            pthread_detach(th);
        } else {
            handle_web_request(c);
            close(c);
            g_web_clients.fetch_sub(1);
        }
    }

    int s = g_srv.exchange(-1);
    if (s >= 0) close(s);   // closed by this thread so the port is really freed
    return nullptr;
}

// ── Main ─────────────────────────────────────────────────
int main() {
    signal(SIGPIPE, SIG_IGN);
    ensure_dir(DATA_DIR);

    // Take the web port first: if it's busy, another copy is running
    int err = 0;
    int s = web_listen_socket(&err);
    int waited = 0;
    if (s < 0 && err == EADDRINUSE) {
        // Ask the running copy to quit and wait for the port.
        // The flag is written again every time: an old copy (v0.2.1 and older)
        // deletes it on exit, and there may be several of them.
        for (waited = 0; waited < TAKEOVER_WAIT_SEC * 2 && s < 0; waited++) {
            FILE* qf = fopen(QUIT_FLAG, "w");
            if (qf) { fputs("quit\n", qf); fclose(qf); }
            usleep(500000);
            s = web_listen_socket(&err);
        }
    }

    g_logfp = fopen(LOG_FILE, "a");
    log_msg("========================================");
    log_msg("ps4chrono v" APP_VERSION " starting");

    if (s < 0) {
        remove_file(QUIT_FLAG);
        if (err == EADDRINUSE)
            log_msg("Another copy is running and did not stop within %d sec, exiting", TAKEOVER_WAIT_SEC);
        else
            log_msg("Cannot start web server (errno %d), exiting", err);
        if (g_logfp) fclose(g_logfp);
        return 1;
    }
    remove_file(QUIT_FLAG);     // leftover flag must not stop this copy
    remove_file(OLD_LOCK_FILE); // not used anymore
    g_srv = s;
    if (waited > 0)
        log_msg("Previous copy was asked to quit and replaced (waited %d ms)", waited * 500);
    log_msg("Web server started on port %d", WEB_PORT);

    load_config();

    snprintf(g_notify_icon_url, sizeof(g_notify_icon_url),
             "http://127.0.0.1:%d/notify.png", WEB_PORT);
    log_msg("Notify icon URL: %s", g_notify_icon_url);

    pthread_t web_th;
    if (pthread_create(&web_th, nullptr, web_thread, nullptr) != 0) {
        log_msg("Cannot start web thread, exiting");
        close(s);
        if (g_logfp) fclose(g_logfp);
        return 1;
    }

    // Initial sync (web page already works)
    bool ok = do_sync();
    long next_check = mono_sec() + (ok ? get_interval() : RETRY_SEC);
    if (!ok && !quit_requested())
        log_msg("Time not confirmed by NTP, retrying every %d sec", RETRY_SEC);

    while (!quit_requested()) {
        bool run = false;

        if (g_reload_req.exchange(false)) {
            log_msg("Restart: reading config again");
            load_config();
            run = true;
        }
        if (g_sync_req.exchange(false)) run = true;
        if (!run && mono_sec() >= next_check) {
            log_msg(ok ? "Periodic check triggered" : "Retry check triggered");
            run = true;
        }

        if (run) {
            bool was_ok = ok;
            ok = do_sync();
            next_check = mono_sec() + (ok ? get_interval() : RETRY_SEC);
            if (!ok && was_ok && !quit_requested())
                log_msg("Time not confirmed by NTP, retrying every %d sec", RETRY_SEC);
        }

        usleep(200000);
    }

    log_msg("Shutting down");
    g_quit = true;
    pthread_join(web_th, nullptr);   // web thread closes the port
    remove_file(QUIT_FLAG);
    log_msg("ps4chrono stopped");
    if (g_logfp) fclose(g_logfp);
    return 0;
}
