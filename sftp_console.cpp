// sftp_console.cpp — interactive SFTP console (F4)
// Supports: ls [path], cd <path>, pwd, get <file> [localname],
//           mget <glob>, put <localfile> [remotename], mput <glob>,
//           mkdir <path>, rmdir <path>, rm <file>, rename <old> <new>,
//           chmod <mode> <file>, help, exit/quit/bye
//
// Shares the libssh2 SFTP subsystem already opened by sftp_overlay.cpp
// (sftp_init() is idempotent).  Background transfers run on a worker thread
// so the render loop stays responsive.
//
#ifdef USESSH

#include "sftp_console.h"
#include "sftp_overlay.h"   // sftp_init(), sftp_local_home_dir(), sftp_local_download_dir()
#include "ssh_session.h"
#include "sftp_webserver.h"  // web server status (started via F9 outside the console)
#include "gl_renderer.h"
#include "ft_font.h"

#include <libssh2.h>
#include <libssh2_sftp.h>
#include <SDL2/SDL.h>

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <functional>
#include <sstream>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <ctime>
#include <chrono>

#ifndef _WIN32
#  include <fnmatch.h>
#  include <sys/stat.h>
#  include <dirent.h>
#  include <unistd.h>
#  include <sys/select.h>
#else
#  include <windows.h>
#  include <shlobj.h>
#  include <shlwapi.h>
#  pragma comment(lib, "shlwapi.lib")
#  define fnmatch(p,s,f) (PathMatchSpecA((s),(p)) ? 0 : 1)
#endif

// ============================================================================
// EXTERNAL REFS  (provided by sftp_overlay.cpp / ssh_session.cpp)
// ============================================================================

extern int g_font_size;

// The live SFTP session handle — defined as static inside sftp_overlay.cpp.
// We reach it through the public init/shutdown API; all actual libssh2 SFTP
// calls go through a thin accessor declared below.
static LIBSSH2_SFTP *s_sftp_handle() {
    // sftp_init() returns the cached handle if already open.
    if (!sftp_init()) return nullptr;
    // libssh2_sftp_init() is called inside sftp_init() and stored as a file-
    // static there.  We need the raw pointer for directory/file ops.
    // Obtain it by opening a known-path handle and immediately closing it?
    // Better: expose a thin getter from sftp_overlay.cpp.
    // For now we store our own copy after the first successful init call.
    return nullptr; // replaced below with our own init
}

// We maintain our own SFTP handle so we don't have to touch sftp_overlay internals.
static LIBSSH2_SFTP *s_sftp  = nullptr;

static bool console_sftp_init() {
    if (s_sftp) return true;
    LIBSSH2_SESSION *sess = ssh_get_session();
    if (!sess) return false;
    libssh2_session_set_blocking(sess, 1);
    s_sftp = libssh2_sftp_init(sess);
    libssh2_session_set_blocking(sess, 0);
    return s_sftp != nullptr;
}

static int waitsocket_con(int sock, LIBSSH2_SESSION *sess) {
    struct timeval tv = { 10, 0 };
    fd_set fd, *rfd = nullptr, *wfd = nullptr;
    FD_ZERO(&fd); FD_SET(sock, &fd);
    int dir = libssh2_session_block_directions(sess);
    if (dir & LIBSSH2_SESSION_BLOCK_INBOUND)  rfd = &fd;
    if (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND) wfd = &fd;
    return select(sock + 1, rfd, wfd, nullptr, &tv);
}

// ============================================================================
// STATE
// ============================================================================

bool g_sftp_console_visible = false;

static const int MAX_HISTORY   = 200;
static const int MAX_SCROLLBACK = 2000;
static const int INPUT_MAX     = 1024;
static const int PAD           = 8;

struct ConsoleLine {
    std::string text;
    float r, g, b;  // colour
};

static std::vector<ConsoleLine>  s_lines;
static int                        s_scroll_offset = 0;  // lines from bottom, 0 = at bottom

static char                       s_input[INPUT_MAX] = {};
static int                        s_input_len  = 0;
static int                        s_cursor_pos = 0;     // byte position in s_input

static std::vector<std::string>   s_history;
static int                        s_history_idx = -1;   // -1 = not browsing

// Mouse selection within the scrollback — (line, col) pairs
struct SelPos { int line = -1; int col = 0; };
static SelPos s_sel_start;
static SelPos s_sel_end;
static bool   s_sel_active  = false;  // drag in progress
static bool   s_sel_exists  = false;  // selection retained after mouseup

static char                       s_remote_cwd[4096] = "/";
static char                       s_local_cwd[4096]  = {};

// Background transfer
static std::thread                s_worker;
static std::atomic<bool>          s_busy{false};
static std::atomic<float>         s_progress{0.f};
static std::atomic<bool>          s_cancel{false};
// Written by the worker, read by render every frame — must be guarded, or a
// reassignment (heap realloc for names > SSO length) frees the buffer render
// is drawing from.
static std::string                s_progress_label_;
static std::mutex                 s_label_mtx;
static void set_progress_label(const std::string &l) {
    std::lock_guard<std::mutex> g(s_label_mtx); s_progress_label_ = l;
}
static std::string get_progress_label() {
    std::lock_guard<std::mutex> g(s_label_mtx); return s_progress_label_;
}

// Transfer timeout constants
static const int TRANSFER_TIMEOUT_SECONDS = 30;  // Max time to read/write one chunk

#ifndef _WIN32
// Signal handler for SIGINT — ignore it and let user use 'cancel' command instead
static void sigint_handler(int sig) {
    (void)sig;
    // Do nothing — just ignore Ctrl+C
    // User should type 'cancel' to stop transfers
}
#endif

// Thread-safe output queue: worker pushes here, main thread drains into s_lines each frame.
static std::vector<ConsoleLine>   s_pending;
static SDL_mutex                 *s_pending_mtx = nullptr;

static void ensure_pending_mtx() {
    if (!s_pending_mtx) s_pending_mtx = SDL_CreateMutex();
}

static void pending_push(const char *text, float r, float g, float b) {
    ensure_pending_mtx();
    // Split on newlines
    const char *p = text;
    const char *nl;
    SDL_LockMutex(s_pending_mtx);
    while ((nl = strchr(p, '\n')) != nullptr) {
        s_pending.push_back({ std::string(p, nl), r, g, b });
        p = nl + 1;
    }
    if (*p) s_pending.push_back({ std::string(p), r, g, b });
    SDL_UnlockMutex(s_pending_mtx);
}

// Called from main thread (render / open) to flush pending into s_lines.
static void drain_pending() {
    ensure_pending_mtx();
    SDL_LockMutex(s_pending_mtx);
    for (auto &ln : s_pending) {
        s_lines.push_back(std::move(ln));
        if ((int)s_lines.size() > MAX_SCROLLBACK)
            s_lines.erase(s_lines.begin());
    }
    if (!s_pending.empty()) s_scroll_offset = 0;
    s_pending.clear();
    SDL_UnlockMutex(s_pending_mtx);
}

// ============================================================================
// ROW HEIGHT  (shared with sftp_overlay style)
// ============================================================================

static int rh() { return (int)(g_font_size * 1.8f); }

static float con_text(const char *t, float x, float y, float r, float g, float b, float a = 1.f) {
    return draw_text(t, x, y, g_font_size, g_font_size, r, g, b, a, 0);
}

// ============================================================================
// OUTPUT HELPERS
// ============================================================================

static void push_line_direct(const char *text, float r, float g, float b) {
    const char *p = text, *nl;
    while ((nl = strchr(p, '\n')) != nullptr) {
        s_lines.push_back({ std::string(p, nl), r, g, b });
        p = nl + 1;
    }
    if (*p) s_lines.push_back({ std::string(p), r, g, b });
    if ((int)s_lines.size() > MAX_SCROLLBACK)
        s_lines.erase(s_lines.begin(), s_lines.begin() + (s_lines.size() - MAX_SCROLLBACK));
    s_scroll_offset = 0;
}

static void push_line(const char *text, float r = 0.82f, float g = 0.88f, float b = 0.96f) {
    // Worker thread → pending queue (drained by main thread in render).
    // Main thread → direct (cmd_instant, push_cmd, etc.).
    // We detect which thread we're on by whether s_busy is set; worker always
    // sets s_busy=true before running, main thread never sets it while calling push_line.
    if (s_busy.load())
        pending_push(text, r, g, b);
    else
        push_line_direct(text, r, g, b);
}

static void push_ok  (const char *t) { push_line(t, 0.30f, 1.00f, 0.45f); }
static void push_err (const char *t) { push_line(t, 1.00f, 0.38f, 0.35f); }
static void push_info(const char *t) { push_line(t, 0.60f, 0.80f, 1.00f); }
static void push_cmd (const char *t) {
    char buf[INPUT_MAX + 8];
    snprintf(buf, sizeof(buf), "sftp> %s", t);
    push_line(buf, 0.95f, 0.90f, 0.50f);
}

// Helper to recursively create directories (mkdir -p equivalent)
static bool ensure_local_dir(const std::string &path) {
#ifndef _WIN32
    if (path.empty()) return true;
    // Check if already exists
    struct stat st{};
    if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
        return true;
    
    // Recursively create parent
    size_t last_slash = path.rfind('/');
    if (last_slash != std::string::npos && last_slash > 0) {
        std::string parent = path.substr(0, last_slash);
        if (!ensure_local_dir(parent)) return false;
    }
    
    // Create this directory
    return mkdir(path.c_str(), 0755) == 0 || (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
#else
    if (path.empty()) return true;
    // Check if already exists
    DWORD attrs = GetFileAttributesA(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
        return true;
    
    // Recursively create parent
    size_t last_slash = path.rfind('\\');
    if (last_slash == std::string::npos) last_slash = path.rfind('/');
    if (last_slash != std::string::npos && last_slash > 0) {
        std::string parent = path.substr(0, last_slash);
        if (!ensure_local_dir(parent)) return false;
    }
    
    // Create this directory
    return CreateDirectoryA(path.c_str(), nullptr) || 
           (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES && 
            (GetFileAttributesA(path.c_str()) & FILE_ATTRIBUTE_DIRECTORY));
#endif
}

// ============================================================================
// REMOTE HELPERS  (all called on worker thread — must hold session lock)
// ============================================================================

struct SessionLock {
    SessionLock()  { ssh_session_lock(); }
    ~SessionLock() { ssh_session_unlock(); }
};

static std::string remote_realpath(const char *path) {
    LIBSSH2_SESSION *sess = ssh_get_session();
    int sock = ssh_get_socket();
    char resolved[4096] = {};
    int rc;
    while ((rc = libssh2_sftp_realpath(s_sftp, path, resolved, sizeof(resolved)-1))
           == LIBSSH2_ERROR_EAGAIN)
        waitsocket_con(sock, sess);
    return rc >= 0 ? std::string(resolved) : std::string(path);
}

static bool remote_is_dir(const char *path) {
    LIBSSH2_SESSION *sess = ssh_get_session();
    int sock = ssh_get_socket();
    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    int rc;
    while ((rc = libssh2_sftp_stat(s_sftp, path, &attrs)) == LIBSSH2_ERROR_EAGAIN)
        waitsocket_con(sock, sess);
    return rc == 0 && (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
           && LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
}

// Returns "" on success, error string on failure.
static std::string do_get(const char *remote_full, const char *local_full,
                           const std::string &label) {
    (void)label;
    LIBSSH2_SESSION *sess = ssh_get_session();
    int sock = ssh_get_socket();
    SessionLock lk;
    libssh2_session_set_blocking(sess, 0);

    // Stat for size
    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    uint64_t file_size = 0;
    { int rc; while ((rc = libssh2_sftp_stat(s_sftp, remote_full, &attrs))
              == LIBSSH2_ERROR_EAGAIN) waitsocket_con(sock, sess);
      if (rc == 0 && (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE)) file_size = attrs.filesize; }

    LIBSSH2_SFTP_HANDLE *fh = nullptr;
    while (!fh) {
        fh = libssh2_sftp_open(s_sftp, remote_full, LIBSSH2_FXF_READ, 0);
        if (!fh) {
            if (libssh2_session_last_errno(sess) == LIBSSH2_ERROR_EAGAIN)
            { waitsocket_con(sock, sess); continue; }
            char e[256];
            snprintf(e, sizeof(e), "cannot open remote '%s' (sftp err %lu)",
                     remote_full, libssh2_sftp_last_error(s_sftp));
            return e;
        }
    }

    FILE *out = fopen(local_full, "wb");
    if (!out) {
        char e[256];
        snprintf(e, sizeof(e), "cannot create '%s': %s", local_full, strerror(errno));
        while (libssh2_sftp_close(fh) == LIBSSH2_ERROR_EAGAIN) waitsocket_con(sock, sess);
        return e;
    }

    // Allocate buffer on heap to avoid stack overflow on large transfers
    const size_t TRANSFER_BUFFER_SIZE = 1024 * 1024;  // 1MB buffer
    char *buf = (char *)malloc(TRANSFER_BUFFER_SIZE);
    if (!buf) {
        fclose(out);
        while (libssh2_sftp_close(fh) == LIBSSH2_ERROR_EAGAIN) waitsocket_con(sock, sess);
        return "out of memory";
    }
    
    ssize_t n; uint64_t total = 0; bool ok = true;
    s_progress.store(0.f);
    auto last_progress_time = std::chrono::steady_clock::now();
    
    bool cancelled = false, timed_out = false;
    for (;;) {
        if (s_cancel.load()) { ok = false; cancelled = true; break; }
        n = libssh2_sftp_read(fh, buf, TRANSFER_BUFFER_SIZE);
        if (n == LIBSSH2_ERROR_EAGAIN) {
            // No-progress watchdog must also run while we're waiting,
            // otherwise a stalled link spins here forever.
            auto idle = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - last_progress_time).count();
            if (idle > TRANSFER_TIMEOUT_SECONDS) { ok = false; timed_out = true; break; }
            waitsocket_con(sock, sess);
            continue;
        }
        if (n < 0) { 
            ok = false; 
            break; 
        }
        if (n == 0) break;
        last_progress_time = std::chrono::steady_clock::now();
        
        if (fwrite(buf, 1, (size_t)n, out) != (size_t)n) { ok = false; break; }
        total += (uint64_t)n;
        if (file_size) s_progress.store((float)total / (float)file_size);
    }
    fclose(out);
    while (libssh2_sftp_close(fh) == LIBSSH2_ERROR_EAGAIN) waitsocket_con(sock, sess);
    free(buf);

    if (!ok) {
        // Don't leave a truncated file that looks like a good download.
        remove(local_full);
        if (cancelled) return std::string("cancelled: '") + remote_full + "'";
        if (timed_out) return std::string("timed out (no data for ") +
                              std::to_string(TRANSFER_TIMEOUT_SECONDS) + "s): '" + remote_full + "'";
        char e[512];
        snprintf(e, sizeof(e), "transfer failed for '%s' (ssh err %d, sftp err %lu)",
                 remote_full, libssh2_session_last_errno(sess), libssh2_sftp_last_error(s_sftp));
        return e;
    }
    s_progress.store(1.f);
    return "";
}

static std::string do_put(const char *local_full, const char *remote_full) {
    uint64_t file_size = 0;
#ifndef _WIN32
    struct stat st{}; if (stat(local_full, &st) == 0) file_size = (uint64_t)st.st_size;
#else
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExA(local_full, GetFileExInfoStandard, &fa))
        file_size = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
#endif
    FILE *in = fopen(local_full, "rb");
    if (!in) {
        char e[256]; snprintf(e, sizeof(e), "cannot open '%s': %s", local_full, strerror(errno));
        return e;
    }
    LIBSSH2_SESSION *sess = ssh_get_session();
    int sock = ssh_get_socket();
    SessionLock lk;
    libssh2_session_set_blocking(sess, 0);

    LIBSSH2_SFTP_HANDLE *fh = nullptr;
    while (!fh) {
        fh = libssh2_sftp_open(s_sftp, remote_full,
            LIBSSH2_FXF_WRITE|LIBSSH2_FXF_CREAT|LIBSSH2_FXF_TRUNC,
            LIBSSH2_SFTP_S_IRUSR|LIBSSH2_SFTP_S_IWUSR|
            LIBSSH2_SFTP_S_IRGRP|LIBSSH2_SFTP_S_IROTH);
        if (!fh) {
            if (libssh2_session_last_errno(sess) == LIBSSH2_ERROR_EAGAIN)
            { waitsocket_con(sock, sess); continue; }
            fclose(in);
            char e[256]; snprintf(e, sizeof(e), "cannot create remote '%s' (sftp err %lu)",
                                  remote_full, libssh2_sftp_last_error(s_sftp));
            return e;
        }
    }

    // Allocate buffer on heap to avoid stack overflow on large transfers
    const size_t TRANSFER_BUFFER_SIZE = 1024 * 1024;  // 1MB buffer
    char *buf = (char *)malloc(TRANSFER_BUFFER_SIZE);
    if (!buf) {
        fclose(in);
        while (libssh2_sftp_close(fh) == LIBSSH2_ERROR_EAGAIN) waitsocket_con(sock, sess);
        return "out of memory";
    }
    
    size_t nread; uint64_t total = 0; bool ok = true;
    s_progress.store(0.f);
    auto last_progress_time = std::chrono::steady_clock::now();
    
    while ((nread = fread(buf, 1, TRANSFER_BUFFER_SIZE, in)) > 0) {
        size_t sent = 0;
        while (sent < nread) {
            if (s_cancel.load()) { ok = false; goto put_done; }
            ssize_t rc = libssh2_sftp_write(fh, buf + sent, nread - sent);
            if (rc == LIBSSH2_ERROR_EAGAIN) {
                auto idle = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - last_progress_time).count();
                if (idle > TRANSFER_TIMEOUT_SECONDS) { ok = false; goto put_done; }
                waitsocket_con(sock, sess);
                continue;
            }
            if (rc < 0) { ok = false; goto put_done; }
            sent += (size_t)rc; total += (size_t)rc;
            last_progress_time = std::chrono::steady_clock::now();
        }
        if (file_size) s_progress.store((float)total / (float)file_size);
    }
    if (ferror(in)) ok = false;
put_done:
    // Ensure files are closed BEFORE releasing session lock
    fclose(in);
    while (libssh2_sftp_close(fh) == LIBSSH2_ERROR_EAGAIN) waitsocket_con(sock, sess);
    free(buf);
    
    // Lock is released here when lk goes out of scope
    
    if (!ok) {
        if (s_cancel.load()) return std::string("cancelled: '") + local_full + "'";
        return std::string("transfer failed or timeout for '") + local_full + "'";
    }
    s_progress.store(1.f);
    return "";
}

// List remote dir, return entries.  Must be called with session lock held.
struct RemoteEntry { std::string name; bool is_dir; uint64_t size; uint32_t perms; time_t mtime; };
static std::vector<RemoteEntry> list_remote_dir(const char *path) {
    LIBSSH2_SESSION *sess = ssh_get_session();
    int sock = ssh_get_socket();
    
    // Use blocking mode for directory listing to ensure complete enumeration
    bool was_blocking = (libssh2_session_get_blocking(sess) != 0);
    if (!was_blocking) libssh2_session_set_blocking(sess, 1);

    LIBSSH2_SFTP_HANDLE *dh = libssh2_sftp_opendir(s_sftp, path);
    
    std::vector<RemoteEntry> out;
    if (!dh) {
        if (!was_blocking) libssh2_session_set_blocking(sess, 0);
        return out;
    }

    char name[512], longentry[1024];
    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    // In blocking mode, readdir will wait until it gets data or EOF
    while (libssh2_sftp_readdir_ex(dh, name, sizeof(name), longentry, sizeof(longentry), &attrs) > 0) {
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        RemoteEntry e;
        e.name   = name;
        e.is_dir = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
        e.size   = (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) ? attrs.filesize : 0;
        e.perms  = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) ? attrs.permissions : 0;
        e.mtime  = (attrs.flags & LIBSSH2_SFTP_ATTR_ACMODTIME)   ? (time_t)attrs.mtime : 0;
        out.push_back(std::move(e));
    }
    libssh2_sftp_closedir(dh);
    
    // Restore original blocking mode
    if (!was_blocking) libssh2_session_set_blocking(sess, 0);
    
    return out;
}

// ============================================================================
// LOCAL CWD HELPERS
// ============================================================================

static void local_chdir(const char *path) {
#ifndef _WIN32
    if (chdir(path) == 0) getcwd(s_local_cwd, sizeof(s_local_cwd));
#else
    SetCurrentDirectoryA(path);
    GetCurrentDirectoryA(sizeof(s_local_cwd), s_local_cwd);
#endif
}

static void refresh_local_cwd() {
#ifndef _WIN32
    if (!getcwd(s_local_cwd, sizeof(s_local_cwd)))
        strncpy(s_local_cwd, sftp_local_home_dir().c_str(), sizeof(s_local_cwd)-1);
#else
    if (!GetCurrentDirectoryA(sizeof(s_local_cwd), s_local_cwd))
        strncpy(s_local_cwd, sftp_local_home_dir().c_str(), sizeof(s_local_cwd)-1);
#endif
}

// ============================================================================
// BUILD A FULL REMOTE PATH  (respects CWD for relative paths)
// ============================================================================

static std::string remote_abs(const std::string &arg) {
    if (arg.empty()) return s_remote_cwd;
    if (arg[0] == '/') return arg;
    std::string base(s_remote_cwd);
    if (base.back() != '/') base += '/';
    return base + arg;
}

static std::string local_abs(const std::string &arg) {
    if (arg.empty()) return s_local_cwd;
#ifndef _WIN32
    if (arg[0] == '/') return arg;
    if (arg[0] == '~') return sftp_local_home_dir() + arg.substr(1);
#else
    if (arg.size() >= 2 && arg[1] == ':') return arg;
#endif
    return std::string(s_local_cwd) + "/" + arg;
}

// ============================================================================
// FORMAT HELPERS
// ============================================================================

static void fmt_size(uint64_t sz, char *buf, int n) {
    if      (sz >= 1024ULL*1024*1024) snprintf(buf,n,"%.1f GB", sz/(1024.*1024*1024));
    else if (sz >= 1024ULL*1024)      snprintf(buf,n,"%.1f MB", sz/(1024.*1024));
    else if (sz >= 1024ULL)           snprintf(buf,n,"%.1f KB", sz/1024.);
    else                              snprintf(buf,n,"%llu B",  (unsigned long long)sz);
}

static void fmt_perms(uint32_t p, char *buf) {
    buf[0] = LIBSSH2_SFTP_S_ISDIR(p) ? 'd' : '-';
    buf[1] = (p & 0400) ? 'r' : '-'; buf[2] = (p & 0200) ? 'w' : '-'; buf[3] = (p & 0100) ? 'x' : '-';
    buf[4] = (p & 0040) ? 'r' : '-'; buf[5] = (p & 0020) ? 'w' : '-'; buf[6] = (p & 0010) ? 'x' : '-';
    buf[7] = (p & 0004) ? 'r' : '-'; buf[8] = (p & 0002) ? 'w' : '-'; buf[9] = (p & 0001) ? 'x' : '-';
    buf[10] = '\0';
}

// ============================================================================
// TOKENISER
// ============================================================================

static std::vector<std::string> tokenise(const char *line) {
    std::vector<std::string> toks;
    const char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        std::string tok;
        bool quoted = false;
        char qchar  = 0;
        if (*p == '"' || *p == '\'') { quoted = true; qchar = *p++; }
        while (*p) {
            if (quoted) {
                if (*p == qchar) { p++; break; }
                if (*p == '\\' && *(p+1)) { p++; tok += *p++; continue; }
            } else {
                if (*p == ' ' || *p == '\t') break;
            }
            tok += *p++;
        }
        if (!tok.empty()) toks.push_back(tok);
    }
    return toks;
}

// ============================================================================
// COMMAND DISPATCH  (called on worker thread for I/O commands, on main for
// instant commands like pwd / help / lcd)
// ============================================================================

static void cmd_help() {
    push_info("Commands:");
    push_info("  ls [glob] [-l]          List remote directory (supports *, ?)");
    push_info("  cd <path>               Change remote directory");
    push_info("  pwd                     Print remote directory");
    push_info("  lcd [path]              Change / print local directory");
    push_info("  lpwd                    Print local directory");
    push_info("  lls [path]              List local directory");
    push_info("  lmkdir <path>           Create local directory");
    push_info("  get <file> [localname]  Download file");
    push_info("  mget [-r] <glob>        Download matching files (-r for recursive)");
    push_info("  put <file> [remotename] Upload file");
    push_info("  mput <glob>             Upload matching local files");
    push_info("  mkdir <path>            Create remote directory");
    push_info("  rmdir <path>            Remove remote directory");
    push_info("  rm <file>               Delete remote file");
    push_info("  rename <old> <new>      Rename/move remote path");
    push_info("  chmod <mode> <file>     Change permissions (octal)");
    push_info("  progress                Show last transfer progress");
    push_info("  cancel                  Cancel current transfer");
    push_info("  clear                   Clear screen");
    push_info("  exit / quit / bye       Close console");
    push_info("");
    push_info("Hotkeys:");
}

// Executed synchronously (no blocking I/O)
static bool cmd_instant(const std::vector<std::string> &toks) {
    if (toks.empty()) return true;
    const std::string &cmd = toks[0];

    if (cmd == "pwd") {
        push_ok(s_remote_cwd);
        return true;
    }
    if (cmd == "lpwd") {
        push_ok(s_local_cwd);
        return true;
    }
    if (cmd == "lcd") {
        if (toks.size() == 1) {
            push_ok(s_local_cwd);
        } else {
            local_chdir(local_abs(toks[1]).c_str());
            push_ok(s_local_cwd);
        }
        return true;
    }
    if (cmd == "lls") {
        const char *dir = toks.size() > 1 ? toks[1].c_str() : s_local_cwd;
#ifndef _WIN32
        DIR *d = opendir(dir);
        if (!d) { push_err(("lls: " + std::string(strerror(errno))).c_str()); return true; }
        struct dirent *de; std::vector<std::string> names;
        while ((de = readdir(d))) { if (strcmp(de->d_name,".")&&strcmp(de->d_name,"..")) names.push_back(de->d_name); }
        closedir(d);
        std::sort(names.begin(), names.end());
        for (auto &n : names) push_line(n.c_str());
#else
        char pat[4096]; snprintf(pat, sizeof(pat), "%s\\*", dir);
        WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
        std::vector<std::string> names;
        if (h != INVALID_HANDLE_VALUE) {
            do { if (strcmp(fd.cFileName,".")&&strcmp(fd.cFileName,"..")) names.push_back(fd.cFileName); }
            while(FindNextFileA(h,&fd)); FindClose(h);
        }
        std::sort(names.begin(), names.end());
        for (auto &n : names) push_line(n.c_str());
#endif
        return true;
    }
    if (cmd == "lmkdir") {
        if (toks.size() < 2) { push_err("usage: lmkdir <path>"); return true; }
        std::string path = local_abs(toks[1]);
#ifndef _WIN32
        if (mkdir(path.c_str(), 0755) != 0)
            push_err(("lmkdir: " + std::string(strerror(errno))).c_str());
        else
            push_ok(("Created " + path).c_str());
#else
        if (!CreateDirectoryA(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            push_err(("lmkdir: failed to create " + path).c_str());
        else
            push_ok(("Created " + path).c_str());
#endif
        return true;
    }
    if (cmd == "clear") {
        s_lines.clear(); s_scroll_offset = 0;
        s_sel_exists = false; s_sel_active = false;
        return true;
    }
    if (cmd == "help" || cmd == "?") {
        cmd_help();
        return true;
    }
    if (cmd == "exit" || cmd == "quit" || cmd == "bye") {
        sftp_console_close();
        return true;
    }
    if (cmd == "progress") {
        char buf[64]; snprintf(buf, sizeof(buf), "%.0f%%", s_progress.load() * 100.f);
        push_info(buf);
        return true;
    }
    if (cmd == "cancel") {
        if (s_busy.load()) { s_cancel.store(true); push_info("Cancelling transfer..."); }
        else push_info("No transfer in progress");
        return true;
    }

    // Everything below talks to the server on the UI thread.  While a transfer
    // owns the session lock that would freeze the window for the whole download
    // (Windows then flags it "Not responding" and may kill it), so refuse.
    if (cmd == "ls" || cmd == "cd" || cmd == "mkdir" || cmd == "rmdir" ||
        cmd == "rm" || cmd == "rename" || cmd == "chmod") {
        if (s_busy.load()) {
            push_err("Busy — wait for the transfer to finish (or type 'cancel')");
            return true;
        }
    }

    if (!console_sftp_init()) { push_err("SFTP subsystem not available"); return true; }

    if (cmd == "ls") {
        bool long_fmt = false;
        std::string path = s_remote_cwd;
        std::string glob_pattern;
        
        // Parse arguments: detect -l flag and glob patterns
        for (int i = 1; i < (int)toks.size(); i++) {
            if (toks[i] == "-l") {
                long_fmt = true;
            } else if (toks[i].find('*') != std::string::npos || 
                       toks[i].find('?') != std::string::npos) {
                // It's a glob pattern — extract directory and pattern
                glob_pattern = toks[i];
                size_t last_slash = glob_pattern.rfind('/');
                if (last_slash != std::string::npos) {
                    // Pattern like "/path/*.mp4" — split into dir and pattern
                    path = remote_abs(glob_pattern.substr(0, last_slash));
                    glob_pattern = glob_pattern.substr(last_slash + 1);
                } else {
                    // Pattern like "*.mp4" — use current directory
                    path = s_remote_cwd;
                }
            } else {
                // Regular path argument
                path = remote_abs(toks[i]);
            }
        }
        
        SessionLock lk;
        LIBSSH2_SESSION *sess = ssh_get_session();
        libssh2_session_set_blocking(sess, 0);
        auto entries = list_remote_dir(path.c_str());
        
        // On first use after open, the SFTP subsystem may need one round-trip
        // to settle — retry once if we got nothing back from a valid directory.
        if (entries.empty() && remote_is_dir(path.c_str()))
            entries = list_remote_dir(path.c_str());
        
        if (entries.empty() && !remote_is_dir(path.c_str())) {
            push_err(("ls: cannot access " + path).c_str());
        } else if (!glob_pattern.empty()) {
            // Filter entries by glob pattern
            entries.erase(
                std::remove_if(entries.begin(), entries.end(),
                    [&](const RemoteEntry &e) {
                        return fnmatch(glob_pattern.c_str(), e.name.c_str(), 0) != 0;
                    }),
                entries.end()
            );
            
            if (entries.empty()) {
                push_info("ls: no matches");
            } else {
                // Sort and display matching entries
                std::sort(entries.begin(), entries.end(), 
                    [](const RemoteEntry &a, const RemoteEntry &b) {
                        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
                        return a.name < b.name;
                    });
                for (auto &e : entries) {
                    if (long_fmt) {
                        char perm[12], sz[24], tmbuf[32];
                        fmt_perms(e.perms, perm);
                        fmt_size(e.size, sz, sizeof(sz));
                        if (e.mtime) {
                            struct tm *tm = gmtime(&e.mtime);
                            strftime(tmbuf, sizeof(tmbuf), "%Y-%m-%d %H:%M", tm);
                        } else {
                            strcpy(tmbuf, "                ");
                        }
                        char line2[1024];
                        snprintf(line2, sizeof(line2), "%s  %8s  %s  %s%s",
                                 perm, sz, tmbuf, e.name.c_str(), e.is_dir ? "/" : "");
                        push_line(line2, e.is_dir ? 0.90f : 0.82f, 
                                         e.is_dir ? 0.90f : 0.82f,
                                         e.is_dir ? 0.50f : 0.96f);
                    } else {
                        std::string disp = e.is_dir ? (e.name + "/") : e.name;
                        push_line(disp.c_str(), e.is_dir ? 0.90f : 0.82f,
                                                e.is_dir ? 0.90f : 0.82f,
                                                e.is_dir ? 0.50f : 0.96f);
                    }
                }
            }
        } else {
            // No glob pattern — display all entries as before
            std::sort(entries.begin(), entries.end(), 
                [](const RemoteEntry &a, const RemoteEntry &b) {
                    if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
                    return a.name < b.name;
                });
            for (auto &e : entries) {
                if (long_fmt) {
                    char perm[12], sz[24], tmbuf[32];
                    fmt_perms(e.perms, perm);
                    fmt_size(e.size, sz, sizeof(sz));
                    if (e.mtime) {
                        struct tm *tm = gmtime(&e.mtime);
                        strftime(tmbuf, sizeof(tmbuf), "%Y-%m-%d %H:%M", tm);
                    } else {
                        strcpy(tmbuf, "                ");
                    }
                    char line2[1024];
                    snprintf(line2, sizeof(line2), "%s  %8s  %s  %s%s",
                             perm, sz, tmbuf, e.name.c_str(), e.is_dir ? "/" : "");
                    push_line(line2, e.is_dir ? 0.90f : 0.82f,
                                     e.is_dir ? 0.90f : 0.82f,
                                     e.is_dir ? 0.50f : 0.96f);
                } else {
                    std::string disp = e.is_dir ? (e.name + "/") : e.name;
                    push_line(disp.c_str(), e.is_dir ? 0.90f : 0.82f,
                                            e.is_dir ? 0.90f : 0.82f,
                                            e.is_dir ? 0.50f : 0.96f);
                }
            }
        }
        
        return true;
    }

    if (cmd == "cd") {
        if (toks.size() < 2) { push_err("usage: cd <path>"); return true; }
        SessionLock lk;
        LIBSSH2_SESSION *sess = ssh_get_session();
        libssh2_session_set_blocking(sess, 0);
        std::string target = remote_abs(toks[1]);
        std::string real   = remote_realpath(target.c_str());
        if (!remote_is_dir(real.c_str()))
            push_err(("cd: not a directory: " + real).c_str());
        else {
            strncpy(s_remote_cwd, real.c_str(), sizeof(s_remote_cwd)-1);
            push_ok(s_remote_cwd);
        }
        return true;
    }

    if (cmd == "mkdir") {
        if (toks.size() < 2) { push_err("usage: mkdir <path>"); return true; }
        std::string path = remote_abs(toks[1]);
        SessionLock lk;
        LIBSSH2_SESSION *sess = ssh_get_session();
        int sock = ssh_get_socket();
        libssh2_session_set_blocking(sess, 0);
        int rc;
        while ((rc = libssh2_sftp_mkdir(s_sftp, path.c_str(),
                LIBSSH2_SFTP_S_IRWXU|LIBSSH2_SFTP_S_IRGRP|LIBSSH2_SFTP_S_IXGRP|
                LIBSSH2_SFTP_S_IROTH|LIBSSH2_SFTP_S_IXOTH))
               == LIBSSH2_ERROR_EAGAIN) waitsocket_con(sock, sess);
        if (rc) push_err(("mkdir failed (sftp " + std::to_string(libssh2_sftp_last_error(s_sftp)) + ")").c_str());
        else    push_ok(("Created " + path).c_str());
        return true;
    }

    if (cmd == "rmdir") {
        if (toks.size() < 2) { push_err("usage: rmdir <path>"); return true; }
        std::string path = remote_abs(toks[1]);
        SessionLock lk;
        LIBSSH2_SESSION *sess = ssh_get_session();
        int sock = ssh_get_socket();
        libssh2_session_set_blocking(sess, 0);
        int rc;
        while ((rc = libssh2_sftp_rmdir(s_sftp, path.c_str()))
               == LIBSSH2_ERROR_EAGAIN) waitsocket_con(sock, sess);
        if (rc) push_err(("rmdir failed (sftp " + std::to_string(libssh2_sftp_last_error(s_sftp)) + ")").c_str());
        else    push_ok(("Removed " + path).c_str());
        return true;
    }

    if (cmd == "rm") {
        if (toks.size() < 2) { push_err("usage: rm <file>"); return true; }
        std::string path = remote_abs(toks[1]);
        SessionLock lk;
        LIBSSH2_SESSION *sess = ssh_get_session();
        int sock = ssh_get_socket();
        libssh2_session_set_blocking(sess, 0);
        int rc;
        while ((rc = libssh2_sftp_unlink(s_sftp, path.c_str()))
               == LIBSSH2_ERROR_EAGAIN) waitsocket_con(sock, sess);
        if (rc) push_err(("rm failed (sftp " + std::to_string(libssh2_sftp_last_error(s_sftp)) + ")").c_str());
        else    push_ok(("Removed " + path).c_str());
        return true;
    }

    if (cmd == "rename") {
        if (toks.size() < 3) { push_err("usage: rename <old> <new>"); return true; }
        std::string src = remote_abs(toks[1]), dst = remote_abs(toks[2]);
        SessionLock lk;
        LIBSSH2_SESSION *sess = ssh_get_session();
        int sock = ssh_get_socket();
        libssh2_session_set_blocking(sess, 0);
        int rc;
        while ((rc = libssh2_sftp_rename(s_sftp, src.c_str(), dst.c_str()))
               == LIBSSH2_ERROR_EAGAIN) waitsocket_con(sock, sess);
        if (rc) push_err(("rename failed (sftp " + std::to_string(libssh2_sftp_last_error(s_sftp)) + ")").c_str());
        else    push_ok((src + " → " + dst).c_str());
        return true;
    }

    if (cmd == "chmod") {
        if (toks.size() < 3) { push_err("usage: chmod <mode> <file>"); return true; }
        unsigned long mode = strtoul(toks[1].c_str(), nullptr, 8);
        std::string path = remote_abs(toks[2]);
        SessionLock lk;
        LIBSSH2_SESSION *sess = ssh_get_session();
        int sock = ssh_get_socket();
        libssh2_session_set_blocking(sess, 0);
        LIBSSH2_SFTP_ATTRIBUTES attrs{};
        attrs.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS;
        attrs.permissions = (uint32_t)mode;
        int rc;
        while ((rc = libssh2_sftp_setstat(s_sftp, path.c_str(), &attrs))
               == LIBSSH2_ERROR_EAGAIN) waitsocket_con(sock, sess);
        if (rc) push_err(("chmod failed (sftp " + std::to_string(libssh2_sftp_last_error(s_sftp)) + ")").c_str());
        else    push_ok(("chmod " + toks[1] + " " + path).c_str());
        return true;
    }

    return false;
}

// Dispatched on a worker thread — only for bulk transfers (get/put/mget/mput)
static void cmd_worker(std::vector<std::string> toks) {
    if (toks.empty()) { s_busy.store(false); return; }
    const std::string &cmd = toks[0];

    // ------------------------------------------------------------------ get
    if (cmd == "get") {
        if (toks.size() < 2) { push_err("usage: get <remotefile> [localname]"); s_busy.store(false); return; }
        std::string rpath = remote_abs(toks[1]);
        std::string fname = toks[1].find('/') != std::string::npos
            ? toks[1].substr(toks[1].rfind('/')+1) : toks[1];
        std::string lpath = toks.size() >= 3 ? local_abs(toks[2])
                                              : std::string(s_local_cwd) + "/" + fname;
        set_progress_label("get " + fname);
        s_progress.store(0.f);
        std::string err = do_get(rpath.c_str(), lpath.c_str(), fname);
        if (err.empty()) {
            push_ok(("Downloaded → " + lpath).c_str());
        } else {
            push_err(err.c_str());
        }
        s_busy.store(false); return;
    }

    // ------------------------------------------------------------------ mget (with -r support for nested patterns)
    if (cmd == "mget") {
        bool recursive = false;
        std::string pattern;
        
        // Parse flags and pattern
        for (int i = 1; i < (int)toks.size(); i++) {
            if (toks[i] == "-r" || toks[i] == "-R") {
                recursive = true;
            } else if (pattern.empty()) {
                pattern = toks[i];
            }
        }
        
        if (pattern.empty()) { 
            push_err("usage: mget [-r] <glob>"); 
            s_busy.store(false); 
            return; 
        }
        
        SessionLock lk;
        LIBSSH2_SESSION *sess = ssh_get_session();
        libssh2_session_set_blocking(sess, 0);
        
        int got = 0;
        
        // Check if pattern contains path separator (e.g., "*/*.py" or "subdir/*")
        bool pattern_has_path = (pattern.find('/') != std::string::npos);
        
        if (!recursive) {
            // Non-recursive: only search current directory
            auto entries = list_remote_dir(s_remote_cwd);
            lk.~SessionLock();
            new (&lk) SessionLock();
            
            for (auto &e : entries) {
                if (s_cancel.load()) break;
                if (e.is_dir) continue;
                if (fnmatch(pattern.c_str(), e.name.c_str(), 0) != 0) continue;
                
                std::string rpath = std::string(s_remote_cwd);
                if (rpath.back() != '/') rpath += "/";
                rpath += e.name;
                
                std::string lpath = std::string(s_local_cwd) + "/" + e.name;
                
                set_progress_label("mget " + e.name);
                s_progress.store(0.f);
                
                std::string err = do_get(rpath.c_str(), lpath.c_str(), e.name);
                if (err.empty()) {
                    char buf[1024]; snprintf(buf, sizeof(buf), "  ↓ %s", e.name.c_str());
                    push_ok(buf); got++;
                } else {
                    push_err(err.c_str());
                }
            }
        } else {
            // Recursive mode: walk directory tree to find all matching files
            std::function<void(const std::string &)> walk_and_get = 
                [&](const std::string &dir) {
                auto entries = list_remote_dir(dir.c_str());
                if (entries.empty()) return;
                
                lk.~SessionLock();
                new (&lk) SessionLock();
                
                for (auto &e : entries) {
                    if (s_cancel.load()) return;
                    // Handle directories
                    if (e.is_dir) {
                        std::string subdir = dir + "/" + e.name;
                        walk_and_get(subdir);
                        continue;
                    }
                    
                    // Build the relative path from remote_cwd for pattern matching
                    std::string rel_path;
                    if (dir == s_remote_cwd) {
                        rel_path = e.name;
                    } else {
                        rel_path = dir;
                        if (rel_path.find(s_remote_cwd) == 0) {
                            rel_path = rel_path.substr(strlen(s_remote_cwd));
                            if (!rel_path.empty() && rel_path[0] == '/') {
                                rel_path = rel_path.substr(1);
                            }
                        }
                        rel_path += "/" + e.name;
                    }
                    
                    // Match pattern:
                    // - If pattern has '/', match against full relative path (e.g., "subdir/file.py" against "*/*.py")
                    // - If pattern is simple (e.g., "*" or "*.mp4"), match against just the filename
                    bool matches = false;
                    if (pattern_has_path) {
                        // Pattern has path separators: match full relative path
                        matches = (fnmatch(pattern.c_str(), rel_path.c_str(), 0) == 0);
                    } else {
                        // Simple pattern: match just the filename
                        matches = (fnmatch(pattern.c_str(), e.name.c_str(), 0) == 0);
                    }
                    if (!matches) continue;
                    
                    // Construct full remote path
                    std::string rpath;
                    if (dir.back() == '/') {
                        rpath = dir + e.name;
                    } else {
                        rpath = dir + "/" + e.name;
                    }
                    
                    // Build local path preserving directory structure
                    std::string lpath;
                    if (dir == s_remote_cwd) {
                        lpath = std::string(s_local_cwd) + "/" + e.name;
                    } else {
                        std::string rel_from_cwd = dir;
                        if (rel_from_cwd.find(s_remote_cwd) == 0) {
                            rel_from_cwd = rel_from_cwd.substr(strlen(s_remote_cwd));
                            if (!rel_from_cwd.empty() && rel_from_cwd[0] == '/') {
                                rel_from_cwd = rel_from_cwd.substr(1);
                            }
                        }
                        lpath = std::string(s_local_cwd) + "/" + rel_from_cwd + "/" + e.name;
                        
                        // Create subdirectory structure
                        std::string local_subdir = std::string(s_local_cwd) + "/" + rel_from_cwd;
                        ensure_local_dir(local_subdir);
                    }
                    
                    set_progress_label("mget " + e.name);
                    s_progress.store(0.f);
                    
                    std::string err = do_get(rpath.c_str(), lpath.c_str(), e.name);
                    if (err.empty()) {
                        char buf[1024]; 
                        snprintf(buf, sizeof(buf), "  ↓ %s", rel_path.c_str());
                        push_ok(buf); 
                        got++;
                    } else {
                        push_err(err.c_str());
                    }
                }
            };  // end walk_and_get lambda
            
            walk_and_get(s_remote_cwd);
        }
        
        if (got == 0) push_info("mget: no matching files");
        s_busy.store(false); 
        return;
    }

    // ------------------------------------------------------------------ put
    if (cmd == "put") {
        if (toks.size() < 2) { push_err("usage: put <localfile> [remotename]"); s_busy.store(false); return; }
        std::string lpath = local_abs(toks[1]);
        std::string fname = toks[1].find('/') != std::string::npos
            ? toks[1].substr(toks[1].rfind('/')+1) : toks[1];
        std::string rpath = toks.size() >= 3 ? remote_abs(toks[2])
                                              : std::string(s_remote_cwd) + "/" + fname;
        set_progress_label("put " + fname);
        s_progress.store(0.f);
        std::string err = do_put(lpath.c_str(), rpath.c_str());
        if (err.empty()) {
            push_ok(("Uploaded → " + rpath).c_str());
        } else {
            push_err(err.c_str());
        }
        s_busy.store(false); return;
    }

    // ------------------------------------------------------------------ mput
    if (cmd == "mput") {
        if (toks.size() < 2) { push_err("usage: mput <glob>"); s_busy.store(false); return; }
        const std::string &pattern = toks[1];
        int put = 0;
#ifndef _WIN32
        DIR *d = opendir(s_local_cwd);
        std::vector<std::string> names;
        if (d) {
            struct dirent *de;
            while ((de = readdir(d))) {
                if (!strcmp(de->d_name,".") || !strcmp(de->d_name,"..")) continue;
                if (fnmatch(pattern.c_str(), de->d_name, 0) == 0) names.push_back(de->d_name);
            }
            closedir(d);
        }
#else
        char pat[4096]; snprintf(pat, sizeof(pat), "%s\\*", s_local_cwd);
        WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
        std::vector<std::string> names;
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!strcmp(fd.cFileName,".") || !strcmp(fd.cFileName,"..")) continue;
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    if (fnmatch(pattern.c_str(), fd.cFileName, 0) == 0) names.push_back(fd.cFileName);
            } while (FindNextFileA(h,&fd));
            FindClose(h);
        }
#endif
        for (auto &name : names) {
            if (s_cancel.load()) break;
            std::string lpath = std::string(s_local_cwd) + "/" + name;
            std::string rpath = std::string(s_remote_cwd) + "/" + name;
            set_progress_label("mput " + name);
            s_progress.store(0.f);
            std::string err = do_put(lpath.c_str(), rpath.c_str());
            if (err.empty()) {
                char buf[1024]; snprintf(buf, sizeof(buf), "  ↑ %s", name.c_str());
                push_ok(buf); put++;
            } else push_err(err.c_str());
        }
        if (put == 0) push_info("mput: no matching files");
        s_busy.store(false); return;
    }

    push_err(("Unknown command: " + cmd + "  (type 'help')").c_str());
    s_busy.store(false);
}

// ============================================================================
// EXECUTE  — route instant vs worker
// ============================================================================

static void execute(const char *line) {
    if (!*line) return;
    push_cmd(line);
    drain_pending();  // flush echo immediately so it appears this frame

    // history
    if (s_history.empty() || s_history.back() != line) {
        s_history.push_back(line);
        if ((int)s_history.size() > MAX_HISTORY)
            s_history.erase(s_history.begin());
    }
    s_history_idx = -1;

    auto toks = tokenise(line);
    if (toks.empty()) return;

    if (cmd_instant(toks)) return;

    // Needs network — dispatch to worker
    if (s_busy.load()) { push_err("Busy — wait for current transfer to finish"); drain_pending(); return; }
    if (!console_sftp_init()) { push_err("SFTP subsystem not available"); drain_pending(); return; }
    s_busy.store(true);
    s_cancel.store(false);
    if (s_worker.joinable()) {
        s_worker.join();   // blocks until previous command's output is all pushed
        drain_pending();   // flush that output into s_lines immediately
    }
    s_worker = std::thread(cmd_worker, std::move(toks));
}

// ============================================================================
// PUBLIC API
// ============================================================================

void sftp_console_open(int /*win_w*/, int /*win_h*/) {
    ensure_pending_mtx();
    g_sftp_console_visible = true;
    refresh_local_cwd();

    if (s_lines.empty()) {
        push_info("SFTP Console — type 'help' for commands, 'exit' to close");
        push_info("Use 'cancel' command to stop transfers (Ctrl+C is disabled)");
    }

    // Populate remote CWD from active session if we can
    if (ssh_active() && !s_busy.load() && console_sftp_init()) {
        SessionLock lk;
        LIBSSH2_SESSION *sess = ssh_get_session();
        libssh2_session_set_blocking(sess, 0);
        std::string real = remote_realpath(".");
        if (!real.empty()) strncpy(s_remote_cwd, real.c_str(), sizeof(s_remote_cwd)-1);
    }

    char prompt[256];
    snprintf(prompt, sizeof(prompt), "Remote: %s   Local: %s", s_remote_cwd, s_local_cwd);
    push_info(prompt);
}

void sftp_console_close() {
    g_sftp_console_visible = false;
}

void sftp_console_join() {
    s_cancel.store(true);
    if (s_worker.joinable()) s_worker.join();
    if (s_sftp) { libssh2_sftp_shutdown(s_sftp); s_sftp = nullptr; }
}

// ============================================================================
// KEYBOARD
// ============================================================================

bool sftp_console_keydown(SDL_Keysym ks, const char *text_input) {
    if (!g_sftp_console_visible) return false;

    SDL_Keycode sym = ks.sym;

    // Global hotkeys always pass through to the main loop
    if (sym == SDLK_F11) return false;

    if (sym == SDLK_ESCAPE) { sftp_console_close(); return true; }

    // Return — execute
    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
        execute(s_input);
        s_input[0]   = '\0';
        s_input_len  = 0;
        s_cursor_pos = 0;
        return true;
    }

    // Backspace
    if (sym == SDLK_BACKSPACE) {
        if (s_cursor_pos > 0) {
            memmove(s_input + s_cursor_pos - 1, s_input + s_cursor_pos,
                    s_input_len - s_cursor_pos + 1);
            s_cursor_pos--;
            s_input_len--;
        }
        return true;
    }

    // Delete
    if (sym == SDLK_DELETE) {
        if (s_cursor_pos < s_input_len) {
            memmove(s_input + s_cursor_pos, s_input + s_cursor_pos + 1,
                    s_input_len - s_cursor_pos);
            s_input_len--;
        }
        return true;
    }

    // Left / Right / Home / End
    if (sym == SDLK_LEFT  && s_cursor_pos > 0)             { s_cursor_pos--; return true; }
    if (sym == SDLK_RIGHT && s_cursor_pos < s_input_len)   { s_cursor_pos++; return true; }
    if (sym == SDLK_HOME)                                   { s_cursor_pos = 0; return true; }
    if (sym == SDLK_END)                                    { s_cursor_pos = s_input_len; return true; }

    // History Up / Down
    if (sym == SDLK_UP) {
        int n = (int)s_history.size();
        if (n == 0) return true;
        if (s_history_idx < 0) s_history_idx = n - 1;
        else if (s_history_idx > 0) s_history_idx--;
        const std::string &h = s_history[s_history_idx];
        strncpy(s_input, h.c_str(), INPUT_MAX - 1);
        s_input_len  = (int)strlen(s_input);
        s_cursor_pos = s_input_len;
        return true;
    }
    if (sym == SDLK_DOWN) {
        int n = (int)s_history.size();
        if (s_history_idx < 0) return true;
        if (s_history_idx < n - 1) {
            s_history_idx++;
            const std::string &h = s_history[s_history_idx];
            strncpy(s_input, h.c_str(), INPUT_MAX - 1);
            s_input_len  = (int)strlen(s_input);
            s_cursor_pos = s_input_len;
        } else {
            s_history_idx = -1;
            s_input[0] = '\0'; s_input_len = 0; s_cursor_pos = 0;
        }
        return true;
    }

    // Scroll buffer
    if (sym == SDLK_PAGEUP)   { s_scroll_offset += 10; return true; }
    if (sym == SDLK_PAGEDOWN) { s_scroll_offset = std::max(0, s_scroll_offset - 10); return true; }

    // Ctrl+V — paste clipboard into input
    if ((ks.mod & KMOD_CTRL) && sym == SDLK_v) {
        char *clip = SDL_GetClipboardText();
        if (clip && *clip) {
            int add = (int)strlen(clip);
            // Strip newlines — paste as single line
            for (int i = 0; i < add; i++) if (clip[i] == '\n' || clip[i] == '\r') clip[i] = ' ';
            if (s_input_len + add < INPUT_MAX - 1) {
                memmove(s_input + s_cursor_pos + add, s_input + s_cursor_pos,
                        s_input_len - s_cursor_pos + 1);
                memcpy(s_input + s_cursor_pos, clip, add);
                s_input_len  += add;
                s_cursor_pos += add;
            }
        }
        if (clip) SDL_free(clip);
        return true;
    }

    // Ctrl+C — do nothing in SFTP console (ignore it)
    if ((ks.mod & KMOD_CTRL) && sym == SDLK_c) {
        return true;  // Consume the key but do nothing
    }

    // Ctrl+L — clear
    if ((ks.mod & KMOD_CTRL) && sym == SDLK_l) {
        s_lines.clear(); s_scroll_offset = 0;
        return true;
    }

    // Printable text
    if (text_input && *text_input) {
        int add = (int)strlen(text_input);
        if (s_input_len + add < INPUT_MAX - 1) {
            memmove(s_input + s_cursor_pos + add, s_input + s_cursor_pos,
                    s_input_len - s_cursor_pos + 1);
            memcpy(s_input + s_cursor_pos, text_input, add);
            s_input_len  += add;
            s_cursor_pos += add;
        }
        return true;
    }

    return true; // consume all keys while console is open
}

// ============================================================================
// GEOMETRY HELPERS  (shared between mouse handlers and render)
// ============================================================================

// Compute the scrollback viewport geometry — same math as render.
// Returns false if mouse is outside the scrollback area.
static bool console_layout(int win_h,
                            float &scroll_top_out, int &visible_out,
                            int &first_out, float &progress_y_out) {
    int row = rh();
    float title_h = (float)(row + PAD);
    float input_h = (float)(row + PAD * 2);
    float input_y = (float)(win_h) - input_h;

    float progress_y = input_y;
    if (s_busy.load() || s_progress.load() > 0.f)
        progress_y = input_y - 4.f - 2.f;

    float scroll_top = title_h;
    float scroll_bot = progress_y - 2.f;
    float area_h     = scroll_bot - scroll_top;
    int   visible    = (int)(area_h / row);

    int total_lines = (int)s_lines.size();
    int max_offset  = std::max(0, total_lines - visible);
    if (s_scroll_offset > max_offset) s_scroll_offset = max_offset;
    int first = std::max(0, total_lines - visible - s_scroll_offset);

    scroll_top_out  = scroll_top;
    visible_out     = visible;
    first_out       = first;
    progress_y_out  = progress_y;
    return true;
}

// Convert a pixel Y coordinate to a line index in s_lines (-1 if outside).
static int pixel_to_line(int py, int win_h) {
    float scroll_top; int visible, first; float progress_y;
    console_layout(win_h, scroll_top, visible, first, progress_y);
    int row = rh();
    if (py < (int)scroll_top || py >= (int)(scroll_top + visible * row)) return -1;
    int i = (int)((py - scroll_top) / row);
    int idx = first + i;
    if (idx < 0 || idx >= (int)s_lines.size()) return -1;
    return idx;
}

// Compare two SelPos values — returns -1, 0, +1
static int sel_cmp(SelPos a, SelPos b) {
    if (a.line != b.line) return a.line < b.line ? -1 : 1;
    if (a.col  != b.col)  return a.col  < b.col  ? -1 : 1;
    return 0;
}

// Convert pixel X within a line to a character column index.
// Uses the same mono-spaced assumption as the renderer: each char is
// approximately g_font_size * 0.6 wide.  For proportional fonts a proper
// measurement loop would be needed, but this matches what draw_text produces.
static int pixel_to_col(int px, const std::string &text) {
    float char_w = g_font_size * 0.6f;
    float text_x = (float)PAD;
    int col = (int)((px - text_x) / char_w);
    if (col < 0) col = 0;
    if (col > (int)text.size()) col = (int)text.size();
    return col;
}

static void console_copy_selection() {
    if (!s_sel_exists) return;
    SelPos a = s_sel_start, b = s_sel_end;
    if (sel_cmp(a, b) > 0) std::swap(a, b);
    if (a.line < 0 || a.line >= (int)s_lines.size()) return;

    std::string text;
    for (int li = a.line; li <= b.line && li < (int)s_lines.size(); li++) {
        const std::string &ln = s_lines[li].text;
        int from = (li == a.line) ? std::min(a.col, (int)ln.size()) : 0;
        int to   = (li == b.line) ? std::min(b.col + 1, (int)ln.size()) : (int)ln.size();
        if (from > to) std::swap(from, to);
        if (!text.empty()) text += '\n';
        text += ln.substr(from, to - from);
    }
    if (!text.empty())
        SDL_SetClipboardText(text.c_str());
}

// ============================================================================
// MOUSE
// ============================================================================

bool sftp_console_mousedown(int x, int y, int button) {
    if (!g_sftp_console_visible) return false;

    // Right-click — paste clipboard into input line
    if (button == SDL_BUTTON_RIGHT) {
        char *clip = SDL_GetClipboardText();
        if (clip && *clip) {
            int add = (int)strlen(clip);
            for (int i = 0; i < add; i++)
                if (clip[i] == '\n' || clip[i] == '\r') clip[i] = ' ';
            if (s_input_len + add < INPUT_MAX - 1) {
                memmove(s_input + s_cursor_pos + add, s_input + s_cursor_pos,
                        s_input_len - s_cursor_pos + 1);
                memcpy(s_input + s_cursor_pos, clip, add);
                s_input_len  += add;
                s_cursor_pos += add;
            }
        }
        if (clip) SDL_free(clip);
        return true;
    }

    // Left-click — start selection (clears existing)
    s_sel_exists = false;
    s_sel_active = false;

    extern int g_console_last_win_h;
    int line = pixel_to_line(y, g_console_last_win_h);
    if (line < 0) return true;
    int col = pixel_to_col(x, s_lines[line].text);
    s_sel_start = { line, col };
    s_sel_end   = { line, col };
    s_sel_active = true;
    return true;
}

bool sftp_console_mousemotion(int x, int y, bool lbutton) {
    if (!g_sftp_console_visible) return false;
    if (!lbutton || !s_sel_active) return true;
    extern int g_console_last_win_h;
    int line = pixel_to_line(y, g_console_last_win_h);
    if (line < 0) return true;
    int col = pixel_to_col(x, s_lines[line].text);
    s_sel_end    = { line, col };
    s_sel_exists = (sel_cmp(s_sel_start, s_sel_end) != 0);
    return true;
}

bool sftp_console_mouseup(int x, int y) {
    if (!g_sftp_console_visible) return false;
    (void)x; (void)y;
    if (s_sel_active) {
        s_sel_active = false;
        s_sel_exists = (sel_cmp(s_sel_start, s_sel_end) != 0);
        if (s_sel_exists)
            console_copy_selection();
    }
    return true;
}

void sftp_console_scroll(int delta_y) {
    // delta_y > 0 = wheel up = scroll toward older lines (increase offset)
    int step = (delta_y > 0) ? 3 : -3;
    s_scroll_offset = std::max(0, s_scroll_offset + step);
    // Upper clamp is applied in render against total_lines - visible
}

// Last win_h seen by render — used by mouse handlers.
int g_console_last_win_h = 480;

// ============================================================================
// RENDER
// ============================================================================

void sftp_console_render(int win_w, int win_h) {
    if (!g_sftp_console_visible) return;

    drain_pending();   // flush worker output into s_lines before drawing

    g_console_last_win_h = win_h;
    int row = rh();

    // Full-screen background
    draw_rect(0, 0, (float)win_w, (float)win_h, 0.05f, 0.06f, 0.08f, 1.f);

    // Title bar
    float title_h = (float)(row + PAD);
    draw_rect(0, 0, (float)win_w, title_h, 0.09f, 0.10f, 0.16f, 1.f);
    draw_rect(0, title_h - 1, (float)win_w, 1, 0.25f, 0.45f, 0.75f, 1.f);
    {
        char title[1024];
        snprintf(title, sizeof(title),
                 "SFTP Console (F4)   remote: %s   local: %s   "
                 "PgUp/PgDn: scroll   Up/Down: history   Esc: close",
                 s_remote_cwd, s_local_cwd);
        con_text(title, (float)PAD, title_h * 0.72f, 0.65f, 0.82f, 1.0f);
    }

    // Input row (bottom)
    float input_h  = (float)(row + PAD * 2);
    float input_y  = win_h - input_h;
    draw_rect(0, input_y, (float)win_w, 1, 0.20f, 0.30f, 0.55f, 1.f);
    draw_rect(0, input_y + 1, (float)win_w, input_h - 1, 0.07f, 0.08f, 0.11f, 1.f);

    // Progress bar (just above input when busy)
    float progress_y = input_y;
    if (s_busy.load() || s_progress.load() > 0.f) {
        float bar_h  = 4.f;
        progress_y   = input_y - bar_h - 2;
        float filled = (float)win_w * s_progress.load();
        draw_rect(0, progress_y, (float)win_w, bar_h, 0.12f, 0.14f, 0.20f, 1.f);
        if (filled > 0)
            draw_rect(0, progress_y, filled, bar_h, 0.20f, 0.80f, 0.45f, 1.f);
        std::string label = get_progress_label();
        if (!label.empty()) {
            con_text(label.c_str(), (float)PAD, progress_y - 4, 0.50f, 0.80f, 0.55f);
        }
    }

    // Scrollback area
    float scroll_top = title_h;
    float scroll_bot = progress_y - 2;
    float area_h     = scroll_bot - scroll_top;
    int   visible    = (int)(area_h / row);

    // Clamp scroll offset
    int total_lines  = (int)s_lines.size();
    int max_offset   = std::max(0, total_lines - visible);
    if (s_scroll_offset > max_offset) s_scroll_offset = max_offset;

    int first = std::max(0, total_lines - visible - s_scroll_offset);
    SelPos sel_a = s_sel_start, sel_b = s_sel_end;
    if (sel_cmp(sel_a, sel_b) > 0) std::swap(sel_a, sel_b);
    bool has_sel = s_sel_exists || s_sel_active;
    float char_w = g_font_size * 0.6f;
    for (int i = 0; i < visible; i++) {
        int idx = first + i;
        if (idx >= total_lines) break;
        const auto &ln = s_lines[idx];
        float y = scroll_top + (float)(i * row);

        // Selection highlight
        if (has_sel && idx >= sel_a.line && idx <= sel_b.line) {
            int from_col = (idx == sel_a.line) ? sel_a.col : 0;
            int to_col   = (idx == sel_b.line) ? sel_b.col + 1 : (int)ln.text.size();
            if (from_col > to_col) std::swap(from_col, to_col);
            float hx = PAD + from_col * char_w;
            float hw = (to_col - from_col) * char_w;
            if (hw < char_w) hw = char_w; // always show at least one char width
            draw_rect(hx, y, hw, (float)row, 0.25f, 0.50f, 0.90f, 0.45f);
        }

        con_text(ln.text.c_str(), (float)PAD, y + row * 0.72f, ln.r, ln.g, ln.b);
    }

    // Prompt + input line
    float prompt_x = (float)PAD;
    float base_y   = input_y + input_h * 0.65f;

    float px = con_text("sftp> ", prompt_x, base_y, 0.40f, 0.75f, 0.40f);

    // Text before cursor
    char before[INPUT_MAX];
    strncpy(before, s_input, s_cursor_pos);
    before[s_cursor_pos] = '\0';
    float cx = con_text(before, px, base_y, 0.95f, 0.95f, 0.80f);

    // Cursor block
    bool blink = (SDL_GetTicks() / 500) % 2 == 0;
    if (blink || s_busy.load()) {
        float cw = (float)g_font_size * 0.6f;
        draw_rect(cx, input_y + PAD, cw, (float)(row), 0.70f, 0.85f, 1.0f, 0.65f);
    }

    // Text after cursor
    if (s_cursor_pos < s_input_len) {
        float after_x = cx + (float)g_font_size * 0.6f;
        con_text(s_input + s_cursor_pos, after_x, base_y, 0.95f, 0.95f, 0.80f);
    }

    // Busy indicator
    if (s_busy.load()) {
        static const char *spin = "|/-\\";
        int frame = (SDL_GetTicks() / 120) % 4;
        char ind[4] = { spin[frame], '\0' };
        con_text(ind, win_w - PAD - g_font_size, base_y, 0.30f, 0.90f, 0.50f);
    }

    gl_flush_verts();
}


bool sftp_console_transfer_in_progress() {
    return s_busy.load();
}

void sftp_console_cancel_transfer() {
    if (s_busy.load()) s_cancel.store(true);
}


void sftp_console_reset_after_fork() {
    // Clean up inherited console state from parent process.
    // The SDL_mutex is not safe to use after fork if the parent had it locked.
    if (s_pending_mtx) {
        SDL_DestroyMutex(s_pending_mtx);
        s_pending_mtx = nullptr;
    }
    
    // Reset console state
    s_lines.clear();
    s_scroll_offset = 0;
    memset(s_input, 0, sizeof(s_input));
    s_input_len = 0;
    s_cursor_pos = 0;
    s_history.clear();
    s_history_idx = -1;
    s_sel_active = false;
    s_sel_exists = false;
    g_sftp_console_visible = false;
    
    // Reset worker state
    s_busy.store(false);
    s_cancel.store(false);
    s_progress.store(0.f);
    s_pending.clear();
}

#endif // USESSH
