#include "simple_ftp_server.h"

#include <esp_log.h>
#include <lwip/sockets.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#define TAG "FtpServer"
#define FTP_CTRL_BUF_SIZE  512
#define FTP_DATA_BUF_SIZE  32768  // heap-allocated, PSRAM has plenty
#define FTP_CLIENT_STACK    3072
#define FTP_CLIENT_PRIO     3

// ── helpers ──────────────────────────────────────────────────────────────

static int create_listen_socket(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static std::string resolve_path(const std::string& root, const std::string& cwd,
                                const std::string& given) {
    std::string base = root + cwd;
    if (!given.empty() && given[0] == '/') {
        base = root + given;  // absolute path from root
    } else if (!given.empty()) {
        if (base.back() != '/') base += '/';
        base += given;
    }
    // Normalise ".." and "." manually
    // Build a vector of components, skip "." and pop on ".."
    // But must not escape root.
    std::vector<std::string> comps;
    std::string cur;
    for (char ch : base) {
        if (ch == '/') {
            if (!cur.empty()) comps.push_back(cur);
            cur.clear();
        } else {
            cur += ch;
        }
    }
    if (!cur.empty()) comps.push_back(cur);

    std::vector<std::string> resolved;
    for (auto& c : comps) {
        if (c == "..") {
            if (!resolved.empty()) resolved.pop_back();
        } else if (c != ".") {
            resolved.push_back(c);
        }
    }
    std::string out;
    for (auto& c : resolved) {
        out += "/";
        out += c;
    }
    if (out.empty()) out = "/";
    return out;
}

static bool path_within_root(const std::string& path, const std::string& root) {
    // Normalised path must start with root
    if (path.size() < root.size()) return false;
    return memcmp(path.data(), root.data(), root.size()) == 0;
}

static std::string format_size(uint32_t bytes) {
    if (bytes < 1024) return std::to_string(bytes);
    if (bytes < 1024 * 1024) return std::to_string(bytes / 1024) + "K";
    if (bytes < 1024 * 1024 * 1024) return std::to_string(bytes / (1024 * 1024)) + "M";
    return std::to_string(bytes / (1024 * 1024 * 1024)) + "G";
}

// ── session ──────────────────────────────────────────────────────────────

struct FtpSession {
    int ctrl_fd = -1;
    int data_listen_fd = -1;
    int data_fd = -1;
    const std::string& root;
    std::string cwd = "/";
    std::string rnfr_path;
    char ctrl_buf[FTP_CTRL_BUF_SIZE];

    FtpSession(int fd, const std::string& r) : ctrl_fd(fd), root(r) {}
};

static void send_response(int fd, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void send_response(int fd, const char* fmt, ...) {
    char buf[FTP_CTRL_BUF_SIZE];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len < 0) return;
    if (len >= (int)sizeof(buf) - 2) len = sizeof(buf) - 3;
    buf[len] = '\r';
    buf[len + 1] = '\n';
    send(fd, buf, len + 2, 0);
}

static int open_data_connection(FtpSession* s) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int fd = accept(s->data_listen_fd, (struct sockaddr*)&addr, &addr_len);
    close(s->data_listen_fd);
    s->data_listen_fd = -1;
    s->data_fd = fd;
    return fd;
}

static void close_data_connection(FtpSession* s) {
    if (s->data_fd >= 0) { close(s->data_fd); s->data_fd = -1; }
    if (s->data_listen_fd >= 0) { close(s->data_listen_fd); s->data_listen_fd = -1; }
}

// ── command handlers ─────────────────────────────────────────────────────

static void cmd_user(FtpSession* s, const char*) { send_response(s->ctrl_fd, "331 Username ok, need password"); }
static void cmd_pass(FtpSession* s, const char*) { send_response(s->ctrl_fd, "230 Login successful"); }
static void cmd_syst(FtpSession* s, const char*) { send_response(s->ctrl_fd, "215 UNIX Type: L8"); }
static void cmd_feat(FtpSession* s, const char*) {
    send_response(s->ctrl_fd, "211-Features:");
    send_response(s->ctrl_fd, " PASV");
    send_response(s->ctrl_fd, " SIZE");
    send_response(s->ctrl_fd, "211 End");
}
static void cmd_type(FtpSession* s, const char*) { send_response(s->ctrl_fd, "200 Type set to I"); }
static void cmd_pwd(FtpSession* s, const char*)  { send_response(s->ctrl_fd, "257 \"%s\"", s->cwd.c_str()); }

static void cmd_cwd(FtpSession* s, const char* arg) {
    std::string p = resolve_path(s->root, s->cwd, arg);
    if (!path_within_root(p, s->root)) { send_response(s->ctrl_fd, "550 Permission denied"); return; }
    struct stat st;
    if (stat(p.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        send_response(s->ctrl_fd, "550 Not a directory");
        return;
    }
    s->cwd = p.substr(s->root.size());
    if (s->cwd.empty()) s->cwd = "/";
    send_response(s->ctrl_fd, "250 Directory changed");
}

static void cmd_cdup(FtpSession* s, const char*) { cmd_cwd(s, ".."); }

static void cmd_pasv(FtpSession* s, const char*) {
    close_data_connection(s);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { send_response(s->ctrl_fd, "425 Can't open data connection"); return; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = 0;
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(fd, 1) < 0) {
        close(fd);
        send_response(s->ctrl_fd, "425 Can't open data connection");
        return;
    }

    // Get the port that was assigned
    socklen_t len = sizeof(addr);
    getsockname(fd, (struct sockaddr*)&addr, &len);
    uint16_t port = ntohs(addr.sin_port);

    // Get the control connection's local IP
    len = sizeof(addr);
    getsockname(s->ctrl_fd, (struct sockaddr*)&addr, &len);
    uint32_t ip = ntohl(addr.sin_addr.s_addr);
    send_response(s->ctrl_fd, "227 Entering Passive Mode (%lu,%lu,%lu,%lu,%u,%u)",
                  (ip >> 24) & 0xff, (ip >> 16) & 0xff,
                  (ip >> 8) & 0xff, ip & 0xff,
                  port >> 8, port & 0xff);

    s->data_listen_fd = fd;
}

static void cmd_list(FtpSession* s, const char* arg) {
    if (s->data_listen_fd < 0) { send_response(s->ctrl_fd, "425 Use PASV first"); return; }
    send_response(s->ctrl_fd, "150 Opening data connection");

    std::string dir = s->root + s->cwd;
    if (!arg || !arg[0]) arg = "";
    std::string path = resolve_path(s->root, s->cwd, arg);
    if (!path_within_root(path, s->root)) { close_data_connection(s); send_response(s->ctrl_fd, "550 Permission denied"); return; }

    DIR* d = opendir(path.c_str());
    if (!d) { close_data_connection(s); send_response(s->ctrl_fd, "550 Failed to list directory"); return; }

    int data_fd = open_data_connection(s);
    if (data_fd < 0) { closedir(d); send_response(s->ctrl_fd, "426 Connection closed"); return; }

    std::string listing;
    listing.reserve(4096);
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        if (entry->d_name[0] == '.' && (entry->d_name[1] == '\0' ||
            (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) continue;

        std::string full = path + "/" + entry->d_name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;

        char perm = S_ISDIR(st.st_mode) ? 'd' : '-';
        struct tm tm;
        localtime_r(&st.st_mtime, &tm);
        char date[13];
        strftime(date, sizeof(date), "%b %d %Y", &tm);

        char line[320];
        int n = snprintf(line, sizeof(line), "%crw-r--r-- 1 ftp ftp %10lu %s %s\r\n",
                         perm, (unsigned long)st.st_size, date, entry->d_name);
        listing += line;

        if (listing.size() >= 4096) {
            send(data_fd, listing.data(), listing.size(), 0);
            listing.clear();
        }
    }
    closedir(d);
    if (!listing.empty()) send(data_fd, listing.data(), listing.size(), 0);
    close(data_fd); s->data_fd = -1;
    send_response(s->ctrl_fd, "226 Transfer complete");
}

static void cmd_retr(FtpSession* s, const char* arg) {
    if (!arg || !arg[0]) { send_response(s->ctrl_fd, "501 Missing file name"); return; }
    if (s->data_listen_fd < 0) { send_response(s->ctrl_fd, "425 Use PASV first"); return; }

    std::string path = resolve_path(s->root, s->cwd, arg);
    if (!path_within_root(path, s->root)) { send_response(s->ctrl_fd, "550 Permission denied"); return; }

    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) { close_data_connection(s); send_response(s->ctrl_fd, "550 Failed to open file"); return; }

    send_response(s->ctrl_fd, "150 Opening data connection");
    int data_fd = open_data_connection(s);
    if (data_fd < 0) { fclose(fp); send_response(s->ctrl_fd, "426 Connection closed"); return; }

    std::vector<char> buf(FTP_DATA_BUF_SIZE);
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), fp)) > 0) {
        send(data_fd, buf.data(), n, 0);
    }
    fclose(fp);
    close(data_fd); s->data_fd = -1;
    send_response(s->ctrl_fd, "226 Transfer complete");
}

static void cmd_stor(FtpSession* s, const char* arg) {
    if (!arg || !arg[0]) { send_response(s->ctrl_fd, "501 Missing file name"); return; }
    if (s->data_listen_fd < 0) { send_response(s->ctrl_fd, "425 Use PASV first"); return; }

    std::string path = resolve_path(s->root, s->cwd, arg);
    if (!path_within_root(path, s->root)) { send_response(s->ctrl_fd, "550 Permission denied"); return; }

    FILE* fp = fopen(path.c_str(), "wb");
    if (!fp) {
        ESP_LOGE(TAG, "STOR fopen(%s) failed: %s", path.c_str(), strerror(errno));
        close_data_connection(s);
        send_response(s->ctrl_fd, "550 Failed to create file");
        return;
    }

    send_response(s->ctrl_fd, "150 Opening data connection");
    int data_fd = open_data_connection(s);
    if (data_fd < 0) { fclose(fp); send_response(s->ctrl_fd, "426 Connection closed"); return; }

    std::vector<char> buf(FTP_DATA_BUF_SIZE);
    int n;
    while ((n = recv(data_fd, buf.data(), buf.size(), 0)) > 0) {
        fwrite(buf.data(), 1, n, fp);
    }
    fclose(fp);
    close(data_fd); s->data_fd = -1;
    send_response(s->ctrl_fd, "226 Transfer complete");
}

static void cmd_dele(FtpSession* s, const char* arg) {
    if (!arg || !arg[0]) { send_response(s->ctrl_fd, "501 Missing file name"); return; }
    std::string path = resolve_path(s->root, s->cwd, arg);
    if (!path_within_root(path, s->root)) { send_response(s->ctrl_fd, "550 Permission denied"); return; }
    if (unlink(path.c_str()) != 0) { send_response(s->ctrl_fd, "550 Delete failed"); return; }
    send_response(s->ctrl_fd, "250 Deleted");
}

static void cmd_mkd(FtpSession* s, const char* arg) {
    if (!arg || !arg[0]) { send_response(s->ctrl_fd, "501 Missing directory name"); return; }
    std::string path = resolve_path(s->root, s->cwd, arg);
    if (!path_within_root(path, s->root)) { send_response(s->ctrl_fd, "550 Permission denied"); return; }
    if (mkdir(path.c_str(), 0777) != 0) { send_response(s->ctrl_fd, "550 Create directory failed"); return; }
    send_response(s->ctrl_fd, "257 \"%s\" created", arg);
}

static void cmd_rmd(FtpSession* s, const char* arg) {
    if (!arg || !arg[0]) { send_response(s->ctrl_fd, "501 Missing directory name"); return; }
    std::string path = resolve_path(s->root, s->cwd, arg);
    if (!path_within_root(path, s->root)) { send_response(s->ctrl_fd, "550 Permission denied"); return; }
    if (rmdir(path.c_str()) != 0) { send_response(s->ctrl_fd, "550 Remove directory failed"); return; }
    send_response(s->ctrl_fd, "250 Directory removed");
}

static void cmd_size(FtpSession* s, const char* arg) {
    if (!arg || !arg[0]) { send_response(s->ctrl_fd, "501 Missing file name"); return; }
    std::string path = resolve_path(s->root, s->cwd, arg);
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) {
        send_response(s->ctrl_fd, "550 Not a regular file");
        return;
    }
    send_response(s->ctrl_fd, "213 %lu", (unsigned long)st.st_size);
}

static void cmd_rnfr(FtpSession* s, const char* arg) {
    if (!arg || !arg[0]) { send_response(s->ctrl_fd, "501 Missing file name"); return; }
    std::string path = resolve_path(s->root, s->cwd, arg);
    if (!path_within_root(path, s->root)) { send_response(s->ctrl_fd, "550 Permission denied"); return; }
    struct stat st;
    if (stat(path.c_str(), &st) != 0) { send_response(s->ctrl_fd, "550 File not found"); return; }
    s->rnfr_path = path;
    send_response(s->ctrl_fd, "350 Ready for RNTO");
}

static void cmd_rnto(FtpSession* s, const char* arg) {
    if (!arg || !arg[0]) { send_response(s->ctrl_fd, "501 Missing file name"); return; }
    if (s->rnfr_path.empty()) { send_response(s->ctrl_fd, "503 Use RNFR first"); return; }
    std::string path = resolve_path(s->root, s->cwd, arg);
    if (!path_within_root(path, s->root)) { send_response(s->ctrl_fd, "550 Permission denied"); return; }
    if (rename(s->rnfr_path.c_str(), path.c_str()) != 0) { send_response(s->ctrl_fd, "550 Rename failed"); return; }
    s->rnfr_path.clear();
    send_response(s->ctrl_fd, "250 Renamed");
}

// ── client handler ───────────────────────────────────────────────────────

static void handle_client(void* arg) {
    auto* s = static_cast<FtpSession*>(arg);
    send_response(s->ctrl_fd, "220 Xiaozhi FTP ready");

    while (true) {
        int n = recv(s->ctrl_fd, s->ctrl_buf, FTP_CTRL_BUF_SIZE - 1, 0);
        if (n <= 0) break;
        s->ctrl_buf[n] = '\0';

        // Strip CRLF
        while (n > 0 && (s->ctrl_buf[n - 1] == '\r' || s->ctrl_buf[n - 1] == '\n')) {
            s->ctrl_buf[--n] = '\0';
        }

        // Split into command and argument
        char* space = strchr(s->ctrl_buf, ' ');
        const char* arg = space ? space + 1 : "";
        if (space) *space = '\0';

        // Make command uppercase
        for (char* p = s->ctrl_buf; *p; p++) {
            if (*p >= 'a' && *p <= 'z') *p -= 32;
        }

        const char* cmd = s->ctrl_buf;
        if (strcmp(cmd, "USER") == 0)      cmd_user(s, arg);
        else if (strcmp(cmd, "PASS") == 0) cmd_pass(s, arg);
        else if (strcmp(cmd, "SYST") == 0) cmd_syst(s, arg);
        else if (strcmp(cmd, "FEAT") == 0) cmd_feat(s, arg);
        else if (strcmp(cmd, "PWD") == 0)  cmd_pwd(s, arg);
        else if (strcmp(cmd, "TYPE") == 0) cmd_type(s, arg);
        else if (strcmp(cmd, "PASV") == 0) cmd_pasv(s, arg);
        else if (strcmp(cmd, "LIST") == 0) cmd_list(s, arg);
        else if (strcmp(cmd, "NLST") == 0) cmd_list(s, arg);
        else if (strcmp(cmd, "RETR") == 0) cmd_retr(s, arg);
        else if (strcmp(cmd, "STOR") == 0) cmd_stor(s, arg);
        else if (strcmp(cmd, "DELE") == 0) cmd_dele(s, arg);
        else if (strcmp(cmd, "MKD") == 0)  cmd_mkd(s, arg);
        else if (strcmp(cmd, "RMD") == 0)  cmd_rmd(s, arg);
        else if (strcmp(cmd, "SIZE") == 0) cmd_size(s, arg);
        else if (strcmp(cmd, "CWD") == 0)  cmd_cwd(s, arg);
        else if (strcmp(cmd, "CDUP") == 0) cmd_cdup(s, arg);
        else if (strcmp(cmd, "RNFR") == 0) cmd_rnfr(s, arg);
        else if (strcmp(cmd, "RNTO") == 0) cmd_rnto(s, arg);
        else if (strcmp(cmd, "OPTS") == 0) send_response(s->ctrl_fd, "200 OK");
        else if (strcmp(cmd, "NOOP") == 0) send_response(s->ctrl_fd, "200 OK");
        else if (strcmp(cmd, "QUIT") == 0) { send_response(s->ctrl_fd, "221 Bye"); break; }
        else send_response(s->ctrl_fd, "502 Command not implemented");
    }

    close_data_connection(s);
    if (s->ctrl_fd >= 0) close(s->ctrl_fd);
    delete s;
    vTaskDelete(nullptr);
}

// ── server ────────────────────────────────────────────────────────────────

SimpleFtpServer::SimpleFtpServer() = default;

SimpleFtpServer::~SimpleFtpServer() { Stop(); }

bool SimpleFtpServer::Start(uint16_t port, const char* root_path) {
    if (running_) return true;
    root_path_ = root_path;

    listen_fd_ = create_listen_socket(port);
    if (listen_fd_ < 0) {
        ESP_LOGE(TAG, "Failed to listen on port %u", port);
        return false;
    }

    running_ = true;
    auto ret = xTaskCreate([](void* arg) {
        static_cast<SimpleFtpServer*>(arg)->AcceptLoop();
        vTaskDelete(nullptr);
    }, "ftp_accept", 3072, this, FTP_CLIENT_PRIO, &accept_task_);

    if (ret != pdPASS) {
        running_ = false;
        close(listen_fd_);
        listen_fd_ = -1;
        ESP_LOGE(TAG, "Failed to create accept task");
        return false;
    }

    ESP_LOGI(TAG, "FTP server started on port %u, root=%s", port, root_path_.c_str());
    return true;
}

void SimpleFtpServer::Stop() {
    if (!running_) return;
    running_ = false;
    close(listen_fd_);
    listen_fd_ = -1;
    // Wait for accept task to exit
    while (accept_task_ != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGI(TAG, "FTP server stopped");
}

void SimpleFtpServer::AcceptLoop() {
    while (running_) {
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        int client_fd = accept(listen_fd_, (struct sockaddr*)&addr, &addr_len);
        if (client_fd < 0) {
            if (running_) vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        auto* session = new FtpSession(client_fd, root_path_);
        xTaskCreate(handle_client, "ftp_client", FTP_CLIENT_STACK,
                    session, FTP_CLIENT_PRIO, nullptr);
    }
    accept_task_ = nullptr;
}
