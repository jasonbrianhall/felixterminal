#ifdef USESSH

#include "sftp_webserver.h"
#include "ssh_session.h"
#include "sftp_overlay.h"

#include "index.hpp"  // Embedded index.html (renamed so it doesn't get checked in with git add *.h)
#include "cookies.hpp"
#include "theme.hpp"
#include "fileFilter.hpp"
#include "imageDetection.hpp"
#include "audioDetection.hpp"
#include "videoDetection.hpp"
#include "zoomPan.hpp"
#include "slideshowAutoplay.hpp"
#include "panDragHandlers.hpp"
#include "zipReader.hpp"
#include "cdgPlayer.hpp"
#include "audioVisualizer.hpp"
#include "videoPlayer.hpp"
#include "karaokePlayer.hpp"
#include "karaoke-library.hpp" // Embedded karaoke-library.js (dynamically imported by karaokePlayer.js)
#include "crypto-js.hpp"   // Crypto JS for Karaoke
#include "404.hpp" // Embedded 404.html
#include "favicon.h" // Embedded favicon.ico
#include "midi_render.h" // In-process MIDI -> WAV synth (OPL3 via DBOPL)
#include "voc_render.h"  // In-process VOC -> WAV converter
#include "au_render.h"   // In-process AU/SND -> WAV converter
#include "aiff_render.h" // In-process AIFF -> WAV converter

#include <libssh2.h>
#include <libssh2_sftp.h>
#include <SDL2/SDL.h>
#include "miniz.h"

#include <thread>
#include <atomic>
#include <memory>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <vector>
#include <signal.h>
#if defined(_MSC_VER) && !defined(__MINGW32__)
#include "msvc_posix_compat.h"  // dirent.h doesn't exist under MSVC
#else
#include <dirent.h>
#endif
#include <sys/stat.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef int socklen_t;
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #define closesocket close
    #define INVALID_SOCKET (-1)
    #define SOCKET_ERROR (-1)
#endif

// ============================================================================
// Globals
// ============================================================================

static std::atomic<bool> g_webserver_running(false);
static std::atomic<bool> g_webserver_should_exit(false);
static std::thread *g_webserver_thread = nullptr;
static int g_listen_socket = INVALID_SOCKET;
static int g_tunnel_counter = 0;
static SDL_mutex *g_tunnel_mutex = nullptr;
static int g_webserver_port = 53716;  // Track which port we're actually using
static std::string g_webserver_bind_addr = "127.0.0.1";  // Track bind address
static int g_webserver_configured_port = 53716;  // User-requested port (no auto-increment)
static bool g_webserver_use_auto_port = true;  // Whether to auto-increment port on failure

// ============================================================================
// Signal Handler Setup
// ============================================================================

static void setup_signal_handlers() {
#ifndef _WIN32
    // Ignore SIGPIPE — when client closes socket, don't kill the process
    signal(SIGPIPE, SIG_IGN);
    SDL_Log("[WebServer] SIGPIPE handler installed");
#endif
}

// ============================================================================
// Socket Utility Functions
// ============================================================================

// Check if a socket is still connected using SO_ERROR
// This is more reliable than send(sock, nullptr, 0, 0)
static bool socket_is_valid(int sock) {
    if (sock == INVALID_SOCKET || sock < 0) {
        return false;
    }
    
    int error = 0;
    socklen_t len = sizeof(error);
    
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&error, &len) < 0) {
        return false;
    }
    
    return error == 0;
}

// Safe send with full error checking
// Returns: number of bytes sent (>0), 0 if connection closed gracefully, -1 on error
static int socket_send_safe(int sock, const char *data, int len) {

    if (!socket_is_valid(sock)) {
        return -1;
    }

    if (len <= 0) {
        return 0;
    }

    int total_sent = 0;

    while (total_sent < len) {
        int result = send(sock, data + total_sent, len - total_sent, 0);


        if (result > 0) {
            total_sent += result;
            continue;
        }

#ifdef _WIN32
        int err = WSAGetLastError();

        if (err == WSAEWOULDBLOCK) {
            SDL_Delay(1);
            continue;
        }

        return -1;
#else
        int err = errno;

        if (err == EAGAIN || err == EWOULDBLOCK) {
            SDL_Delay(1);
            continue;
        }

        return -1;
#endif
    }

    return total_sent;
}

// Receive with timeout awareness
// Returns: number of bytes received (>0), 0 if connection closed, -1 on error
static int socket_recv_safe(int sock, char *buffer, int len) {

    if (sock == INVALID_SOCKET || len <= 0) {
        return -1;
    }

    int result = recv(sock, buffer, len, 0);

#ifdef _WIN32
    if (result < 0) {
        int err = WSAGetLastError();

        if (err == WSAEWOULDBLOCK) {
            return 0;   // no data yet
        }

        return -1;      // real error
    }
#else
    if (result < 0) {
        int err = errno;

        if (err == EAGAIN || err == EWOULDBLOCK) {
            return 0;   // no data yet
        }

        return -1;      // real error
    }
#endif

    if (result == 0) {
        return 0;       // EOF
    }

    return result;      // bytes read
}



// ============================================================================
// Fine-grained session locking
//
// The web server shares the SAME underlying SSH transport/session as the
// terminal and the F4 SFTP console (ssh_get_session()). Opening a second,
// fully re-authenticated SSH connection just for the web server is
// fragile — it repeats the handshake and auth on a background thread,
// which can trip server-side connection-rate limits or racy libssh2/OpenSSL
// global state — and it's unnecessary, since each libssh2_sftp_init() call
// already opens its own independent SFTP subsystem/channel on top of the
// one transport.
//
// What actually keeps the web server from freezing the terminal (or vice
// versa) is lock *duration*, not connection count: the session must be
// locked for any libssh2 call, but that lock must be held only around each
// individual call — never across an entire request or file transfer, or a
// large upload/download would block keystrokes for its whole duration.
// SftpOp locks the session, flips it to blocking mode so this one call
// doesn't need a manual EAGAIN retry loop, and un-flips + unlocks the
// instant it goes out of scope.
// ============================================================================
struct SftpOp {
    LIBSSH2_SESSION *sess;
    SftpOp(LIBSSH2_SESSION *s) : sess(s) {
        ssh_session_lock();
        libssh2_session_set_blocking(sess, 1);
    }
    ~SftpOp() {
        libssh2_session_set_blocking(sess, 0);
        ssh_session_unlock();
    }
};

// ============================================================================
// SFTP File Operations
// ============================================================================

struct RemoteFile {
    char name[512];
    bool is_dir;
    uint64_t size;
    time_t modified;
};

static bool sftp_get_file(LIBSSH2_SFTP *sftp, const char *remote_path,
                          std::vector<uint8_t> &data) {
    LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_open(sftp, remote_path,
                                                     LIBSSH2_FXF_READ,
                                                     LIBSSH2_SFTP_S_IRUSR);
    if (!handle) return false;

    char buffer[65536];
    int bytes_read;

    while ((bytes_read = libssh2_sftp_read(handle, buffer, sizeof(buffer))) > 0) {
        data.insert(data.end(), (uint8_t *)buffer, (uint8_t *)buffer + bytes_read);
    }

    libssh2_sftp_close_handle(handle);
    return bytes_read == 0;
}

// ============================================================================
// Local filesystem mode — serves a directory on disk directly, with no SSH/
// SFTP involved at all. Used when there's no active SSH session (F9).
// Read-only: browsing + download only, same as the SFTP-backed browser.
// ============================================================================

static bool           g_local_mode = false;
static std::string    g_local_root;   // absolute, no trailing slash (except "/")

// Confines url_path (always absolute, e.g. "/sub/dir") to g_local_root and
// rejects any attempt to escape it via "..". Returns false on rejection.
static bool local_resolve_path(const std::string &url_path, std::string &out_fs_path) {
    if (url_path.find("..") != std::string::npos) return false;

    std::string rel = url_path;
    if (!rel.empty() && rel[0] == '/') rel.erase(0, 1);

    out_fs_path = g_local_root;
    if (!rel.empty()) {
        if (out_fs_path.empty() || out_fs_path.back() != '/') out_fs_path += '/';
        out_fs_path += rel;
    }
    if (out_fs_path.empty()) out_fs_path = "/";
    return true;
}

static bool local_list_directory(const char *fs_path, std::vector<RemoteFile> &entries) {
    DIR *dir = opendir(fs_path);
    if (!dir) {
        SDL_Log("[WebServer] local opendir failed for path: %s", fs_path);
        return false;
    }

    struct dirent *de;
    while ((de = readdir(dir)) != nullptr) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;

        std::string child = fs_path;
        if (child.empty() || child.back() != '/') child += '/';
        child += de->d_name;

        struct stat st;
        if (stat(child.c_str(), &st) != 0) continue;

        RemoteFile entry;
        strncpy(entry.name, de->d_name, sizeof(entry.name) - 1);
        entry.name[sizeof(entry.name) - 1] = '\0';
        entry.is_dir   = S_ISDIR(st.st_mode);
        entry.size     = (uint64_t)st.st_size;
        entry.modified = st.st_mtime;
        entries.push_back(entry);
    }

    closedir(dir);
    return true;
}

static bool sftp_list_directory(LIBSSH2_SFTP *sftp, const char *path, 
                                std::vector<RemoteFile> &entries) {
    LIBSSH2_SFTP_HANDLE *dir = libssh2_sftp_opendir(sftp, path);
    if (!dir) {
        SDL_Log("[SFTP] opendir failed for path: %s", path);
        return false;
    }

    LIBSSH2_SFTP_ATTRIBUTES attrs;
    char name[512];
    int rc;

    while (1) {
        rc = libssh2_sftp_readdir(dir, name, sizeof(name) - 1, &attrs);
        
        if (rc > 0) {
            // Skip . and .. entries
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
                continue;
            }
            
            RemoteFile entry;
            strncpy(entry.name, name, sizeof(entry.name) - 1);
            entry.name[sizeof(entry.name) - 1] = '\0';
            entry.is_dir = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) &&
                           LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
            entry.size = (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) ? attrs.filesize : 0;
            entry.modified = (attrs.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) ? attrs.mtime : 0;
            entries.push_back(entry);
        } else if (rc == 0) {
            break;
        } else {
            SDL_Log("[SFTP] readdir error: %d for path: %s", rc, path);
            libssh2_sftp_closedir(dir);
            return false;
        }
    }

    libssh2_sftp_closedir(dir);
    return true;
}

// ============================================================================
// HTTP Response Builders
// ============================================================================

// gzip-compress a buffer (miniz gives raw deflate; we wrap it in a gzip
// header/trailer ourselves since Content-Encoding: gzip needs that framing,
// not bare zlib). Returns false (leaving out untouched) if compression fails.
// Gzip compression removed — server runs on localhost only.


// Client opts in to server-side MIDI->WAV rendering via this header.
// Without it, .mid/.midi/.kar files are served as-is (plain download) —
// only our own music player sends this header, so a raw request just
// gets the raw file.
static bool client_wants_midi_render(const char *raw_request) {
    return strstr(raw_request, "X-Render-Midi:") != nullptr;
}

static std::string http_response_headers(int status_code, const char *content_type,
                                        size_t content_length, const char *filename = nullptr, const char *felix_type = nullptr) {
    char buf[768];
    const char *status_text = "OK";
    if (status_code == 400) status_text = "Bad Request";
    else if (status_code == 404) status_text = "Not Found";
    else if (status_code == 405) status_text = "Method Not Allowed";
    else if (status_code == 416) status_text = "Range Not Satisfiable";
    else if (status_code == 500) status_text = "Internal Server Error";

    // Always use inline disposition to let browser try to open/view the file
    if (felix_type) {
        snprintf(buf, sizeof(buf),
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Cache-Control: max-age=60\r\n"
            "Felix-file-type: %s\r\n"
            "Connection: close\r\n"
            "\r\n",
            status_code, status_text, content_type, content_length, felix_type);
    } else {
        snprintf(buf, sizeof(buf),
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Cache-Control: max-age=60\r\n"
            "Connection: close\r\n"
            "\r\n",
            status_code, status_text, content_type, content_length);
    }
    
    return buf;
}

// Response headers for public/static files with aggressive caching
// Embedded files are immutable, so we can cache them for a very long time (1 year)
static std::string http_public_file_headers(const char *content_type, size_t content_length) {
    char buf[512];
    snprintf(buf, sizeof(buf),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: public, max-age=31536000, immutable\r\n"
        "Connection: close\r\n"
        "\r\n",
        content_type, content_length);
    return buf;
}

// Response builder for a 302 redirect (e.g. normalizing double slashes in a path).
static std::string http_redirect_response(const char *location) {
    char buf[1536];
    snprintf(buf, sizeof(buf),
        "HTTP/1.1 302 Found\r\n"
        "Location: %s\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n",
        location);
    return buf;
}

// Collapses runs of consecutive '/' into a single '/'. Returns true if the
// path was changed (i.e. it contained a double slash that needed fixing).
static bool normalize_path(const char *raw_path, std::string &out) {
    out.clear();
    bool changed = false;
    bool prev_slash = false;
    for (const char *p = raw_path; *p; ++p) {
        if (*p == '/') {
            if (prev_slash) {
                changed = true;
                continue;
            }
            prev_slash = true;
        } else {
            prev_slash = false;
        }
        out += *p;
    }
    if (out.empty()) out = "/";
    return changed;
}

// Response builder for file downloads: always advertises Accept-Ranges so
// players know they can seek, and emits either a full 200 or a partial
// 206 (with Content-Range) when range_start/range_end are given.
static std::string http_file_response_headers(bool is_partial, uint64_t range_start,
                                              uint64_t range_end, uint64_t total_size,
                                              const char *content_type) {
    char buf[768];
    if (is_partial) {
        snprintf(buf, sizeof(buf),
            "HTTP/1.1 206 Partial Content\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %llu\r\n"
            "Content-Range: bytes %llu-%llu/%llu\r\n"
            "Accept-Ranges: bytes\r\n"
            "Felix-file-type: file\r\n"
            "Connection: close\r\n"
            "\r\n",
            content_type,
            (unsigned long long)(range_end - range_start + 1),
            (unsigned long long)range_start, (unsigned long long)range_end,
            (unsigned long long)total_size);
    } else {
        snprintf(buf, sizeof(buf),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %llu\r\n"
            "Accept-Ranges: bytes\r\n"
            "Felix-file-type: file\r\n"
            "Connection: close\r\n"
            "\r\n",
            content_type, (unsigned long long)total_size);
    }
    return buf;
}

// Parses a "Range: bytes=START-END" header (END and even START may be
// omitted per RFC 7233, e.g. "bytes=500-" or "bytes=-500" for last 500
// bytes). Returns false if no Range header is present. On success, fills
// in the resolved (inclusive) byte range clamped to [0, file_size-1].
static bool parse_range_header(const char *request_buf, uint64_t file_size,
                                uint64_t &range_start, uint64_t &range_end) {
    const char *range_hdr = strstr(request_buf, "Range:");
    if (!range_hdr) range_hdr = strstr(request_buf, "range:");
    if (!range_hdr) return false;

    const char *eq = strchr(range_hdr, '=');
    if (!eq) return false;
    eq++;

    unsigned long long start = 0, end = 0;
    bool has_start = false, has_end = false;

    if (*eq == '-') {
        // suffix form: bytes=-N  => last N bytes
        has_end = (sscanf(eq, "-%llu", &end) == 1);
        if (!has_end || file_size == 0) return false;
        range_end = file_size - 1;
        range_start = (end >= file_size) ? 0 : (file_size - end);
        return true;
    }

    int matched = sscanf(eq, "%llu-%llu", &start, &end);
    if (matched < 1) return false;
    has_start = true;
    has_end = (matched == 2);

    range_start = start;
    range_end = has_end ? end : (file_size ? file_size - 1 : 0);

    if (file_size > 0 && range_end >= file_size) range_end = file_size - 1;
    if (range_start > range_end) return false;

    (void)has_start;
    return true;
}

static std::string json_escape(const std::string &s) {
    std::string result;
    for (char c : s) {
        if (c == '"') result += "\\\"";
        else if (c == '\\') result += "\\\\";
        else if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else if (c == '\t') result += "\\t";
        else result += c;
    }
    return result;
}

// Local user's home directory (F9 / no-SSH mode). Different env var per OS.
static std::string get_local_home_dir() {
#ifdef _WIN32
    const char *p = getenv("USERPROFILE");
    std::string h = p ? p : "C:/";
#else
    const char *p = getenv("HOME");
    std::string h = p ? p : "/";
#endif
    for (auto &c : h) if (c == '\\') c = '/';
    return h;
}

// Local mode is served chrooted under g_local_root, so the "Home" link only
// works if the OS home directory actually falls under that root. Returns
// false (no link) rather than a broken/misleading path otherwise.
static bool local_home_url_path(std::string &out_url_path) {
    std::string home = get_local_home_dir();
    std::string root = g_local_root;
    if (home.size() >= root.size() && home.compare(0, root.size(), root) == 0) {
        std::string rel = home.substr(root.size());
        if (rel.empty() || rel[0] != '/') rel = "/" + rel;
        out_url_path = rel;
        return true;
    }
    return false;
}

// Remote user's home directory (SFTP mode) — realpath(".") resolves relative
// to the SFTP subsystem's starting directory, which is the login home dir.
static std::string get_remote_home_dir(LIBSSH2_SESSION *sess, LIBSSH2_SFTP *sftp) {
    char buf[1024];
    int rc;
    { SftpOp op(sess); rc = libssh2_sftp_realpath(sftp, ".", buf, sizeof(buf) - 1); }
    if (rc > 0) {
        buf[rc] = '\0';
        return std::string(buf);
    }
    return std::string("/");
}

static std::string build_directory_json(const std::vector<RemoteFile> &entries,
                                         const std::string *home_path = nullptr) {
    std::string json = "{\"entries\":[";
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i > 0) json += ",";
        char buf[1024];
        snprintf(buf, sizeof(buf),
            "{\"name\":\"%s\",\"type\":\"%s\",\"size\":%llu,\"modified\":%ld}",
            json_escape(entries[i].name).c_str(),
            entries[i].is_dir ? "directory" : "file",
            (unsigned long long)entries[i].size,
            (long)entries[i].modified);
        json += buf;
    }
    json += "]";
    if (home_path) {
        json += ",\"home\":\"" + json_escape(*home_path) + "\"";
    } else {
        json += ",\"home\":null";
    }
    json += "}";
    return json;
}

static std::string get_index_html() {
    return std::string((const char *)index_html, index_html_len);
}

static std::string get_404() {
    return std::string((const char *)__404_html, __404_html_len);
}

static std::string get_favicon() {
    return std::string((const char *)favicon_ico, favicon_ico_len);
}

// ============================================================================
// Public Files Enumeration
// ============================================================================

enum class PublicFile {
    COOKIES_JS,
    THEME_JS,
    FILE_FILTER_JS,
    IMAGE_DETECTION_JS,
    AUDIO_DETECTION_JS,
    VIDEO_DETECTION_JS,
    ZOOM_PAN_JS,
    SLIDESHOW_AUTOPLAY_JS,
    PAN_DRAG_HANDLERS_JS,
    ZIP_READER_JS,
    CDG_PLAYER_JS,
    AUDIO_VISUALIZER_JS,
    VIDEO_PLAYER_JS,
    KARAOKEPLAYER_JS,
    KARAOKE_LIBRARY_JS,
    CRYPTO_JS,
    UNKNOWN
};

// Maps URL path to public file type
static PublicFile resolve_public_file(const char *path) {
    if (strcmp(path, "/public/cookies.js") == 0) return PublicFile::COOKIES_JS;
    if (strcmp(path, "/public/theme.js") == 0) return PublicFile::THEME_JS;
    if (strcmp(path, "/public/fileFilter.js") == 0) return PublicFile::FILE_FILTER_JS;
    if (strcmp(path, "/public/imageDetection.js") == 0) return PublicFile::IMAGE_DETECTION_JS;
    if (strcmp(path, "/public/audioDetection.js") == 0) return PublicFile::AUDIO_DETECTION_JS;
    if (strcmp(path, "/public/videoDetection.js") == 0) return PublicFile::VIDEO_DETECTION_JS;
    if (strcmp(path, "/public/zoomPan.js") == 0) return PublicFile::ZOOM_PAN_JS;
    if (strcmp(path, "/public/slideshowAutoplay.js") == 0) return PublicFile::SLIDESHOW_AUTOPLAY_JS;
    if (strcmp(path, "/public/panDragHandlers.js") == 0) return PublicFile::PAN_DRAG_HANDLERS_JS;
    if (strcmp(path, "/public/zipReader.js") == 0) return PublicFile::ZIP_READER_JS;
    if (strcmp(path, "/public/cdgPlayer.js") == 0) return PublicFile::CDG_PLAYER_JS;
    if (strcmp(path, "/public/audioVisualizer.js") == 0) return PublicFile::AUDIO_VISUALIZER_JS;
    if (strcmp(path, "/public/videoPlayer.js") == 0) return PublicFile::VIDEO_PLAYER_JS;
    if (strcmp(path, "/public/karaokePlayer.js") == 0) return PublicFile::KARAOKEPLAYER_JS;
    if (strcmp(path, "/public/karaoke-library.js") == 0) return PublicFile::KARAOKE_LIBRARY_JS;
    if (strcmp(path, "/public/crypto-js.min.js") == 0) return PublicFile::CRYPTO_JS;

    return PublicFile::UNKNOWN;
}

// Retrieves embedded file content by type
static std::string get_public_file_content(PublicFile file) {
    switch (file) {
        case PublicFile::COOKIES_JS:
            return std::string((const char *)cookies_js, cookies_js_len);
        case PublicFile::THEME_JS:
            return std::string((const char *)theme_js, theme_js_len);
        case PublicFile::FILE_FILTER_JS:
            return std::string((const char *)fileFilter_js, fileFilter_js_len);
        case PublicFile::IMAGE_DETECTION_JS:
            return std::string((const char *)imageDetection_js, imageDetection_js_len);
        case PublicFile::AUDIO_DETECTION_JS:
            return std::string((const char *)audioDetection_js, audioDetection_js_len);
        case PublicFile::VIDEO_DETECTION_JS:
            return std::string((const char *)videoDetection_js, videoDetection_js_len);
        case PublicFile::ZOOM_PAN_JS:
            return std::string((const char *)zoomPan_js, zoomPan_js_len);
        case PublicFile::SLIDESHOW_AUTOPLAY_JS:
            return std::string((const char *)slideshowAutoplay_js, slideshowAutoplay_js_len);
        case PublicFile::PAN_DRAG_HANDLERS_JS:
            return std::string((const char *)panDragHandlers_js, panDragHandlers_js_len);
        case PublicFile::ZIP_READER_JS:
            return std::string((const char *)zipReader_js, zipReader_js_len);
        case PublicFile::CDG_PLAYER_JS:
            return std::string((const char *)cdgPlayer_js, cdgPlayer_js_len);
        case PublicFile::AUDIO_VISUALIZER_JS:
            return std::string((const char *)audioVisualizer_js, audioVisualizer_js_len);
        case PublicFile::VIDEO_PLAYER_JS:
            return std::string((const char *)videoPlayer_js, videoPlayer_js_len);
        case PublicFile::KARAOKEPLAYER_JS:
            return std::string((const char *)karaokePlayer_js, karaokePlayer_js_len);
        case PublicFile::KARAOKE_LIBRARY_JS:
            return std::string((const char *)karaoke_library_js, karaoke_library_js_len);
        case PublicFile::CRYPTO_JS:
            return std::string((const char *)crypto_js_min_js, crypto_js_min_js_len);
        default:
            return "";
    }
}

// Gets MIME type for public file
static const char *get_public_file_mime_type(PublicFile file) {
    switch (file) {
        case PublicFile::COOKIES_JS:
        case PublicFile::THEME_JS:
        case PublicFile::FILE_FILTER_JS:
        case PublicFile::IMAGE_DETECTION_JS:
        case PublicFile::AUDIO_DETECTION_JS:
        case PublicFile::VIDEO_DETECTION_JS:
        case PublicFile::ZOOM_PAN_JS:
        case PublicFile::SLIDESHOW_AUTOPLAY_JS:
        case PublicFile::PAN_DRAG_HANDLERS_JS:
        case PublicFile::ZIP_READER_JS:
        case PublicFile::CDG_PLAYER_JS:
        case PublicFile::AUDIO_VISUALIZER_JS:
        case PublicFile::VIDEO_PLAYER_JS:
        case PublicFile::KARAOKEPLAYER_JS:
        case PublicFile::KARAOKE_LIBRARY_JS:
        case PublicFile::CRYPTO_JS:
            return "application/javascript";
        default:
            return "application/octet-stream";
    }
}

// Serves a public file response with aggressive caching for static assets
// Embedded files are immutable, so browsers can safely cache them long-term
static void serve_public_file(int client_socket, int tunnel_id, PublicFile file) {
    if (file == PublicFile::UNKNOWN) {
        std::string data = get_404();
        std::string response = http_response_headers(404, "text/html", data.length());
        response += data;
        socket_send_safe(client_socket, response.c_str(), response.length());
        SDL_Log("[Tunnel %d] Public file not found", tunnel_id);
        return;
    }

    std::string content = get_public_file_content(file);
    const char *mime_type = get_public_file_mime_type(file);
    
    // Use aggressive caching headers for static embedded assets
    std::string response = http_public_file_headers(mime_type, content.length());
    socket_send_safe(client_socket, response.c_str(), response.length());
    socket_send_safe(client_socket, content.c_str(), content.length());
    SDL_Log("[Tunnel %d] Served public file (%zu bytes) with 1-year cache", tunnel_id, content.length());
}

// Case-insensitive string comparison for extensions
static int strcasecmp_ext(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }
    return *a - *b;
}

static const char* get_mime_type(const char *filename) {
    if (!filename) return "application/octet-stream";
    
    const char *ext = strrchr(filename, '.');
    if (!ext) return "application/octet-stream";
    
    // Check for compound extensions first (e.g., .tar.gz)
    if (strcasecmp_ext(ext, ".tar.gz") == 0 || strcasecmp_ext(ext, ".tgz") == 0) 
        return "application/gzip";
    
    // Image formats
    if (strcasecmp_ext(ext, ".png") == 0) return "image/png";
    if (strcasecmp_ext(ext, ".jpg") == 0 || strcasecmp_ext(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp_ext(ext, ".gif") == 0) return "image/gif";
    if (strcasecmp_ext(ext, ".webp") == 0) return "image/webp";
    if (strcasecmp_ext(ext, ".svg") == 0) return "image/svg+xml";
    if (strcasecmp_ext(ext, ".bmp") == 0) return "image/bmp";
    if (strcasecmp_ext(ext, ".ico") == 0) return "image/x-icon";
    
    // Document formats
    if (strcasecmp_ext(ext, ".pdf") == 0) return "application/pdf";
    if (strcasecmp_ext(ext, ".txt") == 0) return "text/plain";
    if (strcasecmp_ext(ext, ".html") == 0 || strcasecmp_ext(ext, ".htm") == 0) return "text/html";
    if (strcasecmp_ext(ext, ".css") == 0) return "text/css";
    if (strcasecmp_ext(ext, ".js") == 0) return "application/javascript";
    if (strcasecmp_ext(ext, ".json") == 0) return "application/json";
    if (strcasecmp_ext(ext, ".xml") == 0) return "application/xml";
    
    // Archive formats
    if (strcasecmp_ext(ext, ".zip") == 0) return "application/zip";
    if (strcasecmp_ext(ext, ".gz") == 0) return "application/gzip";
    if (strcasecmp_ext(ext, ".tar") == 0) return "application/x-tar";
    if (strcasecmp_ext(ext, ".rar") == 0) return "application/x-rar-compressed";
    if (strcasecmp_ext(ext, ".7z") == 0) return "application/x-7z-compressed";
    
    // Audio formats
    if (strcasecmp_ext(ext, ".mp3") == 0) return "audio/mpeg";
    if (strcasecmp_ext(ext, ".wav") == 0) return "audio/wav";
    if (strcasecmp_ext(ext, ".flac") == 0) return "audio/flac";
    if (strcasecmp_ext(ext, ".m4a") == 0) return "audio/mp4";
    if (strcasecmp_ext(ext, ".aac") == 0) return "audio/aac";
    if (strcasecmp_ext(ext, ".ogg") == 0 || strcasecmp_ext(ext, ".oga") == 0) return "audio/ogg";
    if (strcasecmp_ext(ext, ".mid") == 0 || strcasecmp_ext(ext, ".midi") == 0) return "audio/midi";
    if (strcasecmp_ext(ext, ".kar") == 0) return "audio/midi";
    if (strcasecmp_ext(ext, ".weba") == 0) return "audio/webm";
    if (strcasecmp_ext(ext, ".opus") == 0) return "audio/opus";
    
    // Video formats
    if (strcasecmp_ext(ext, ".mp4") == 0) return "video/mp4";
    if (strcasecmp_ext(ext, ".webm") == 0) return "video/webm";
    if (strcasecmp_ext(ext, ".mkv") == 0) return "video/x-matroska";
    if (strcasecmp_ext(ext, ".avi") == 0) return "video/x-msvideo";
    if (strcasecmp_ext(ext, ".mov") == 0) return "video/quicktime";
    if (strcasecmp_ext(ext, ".flv") == 0) return "video/x-flv";
    if (strcasecmp_ext(ext, ".ogv") == 0) return "video/ogg";
    
    return "application/octet-stream";
}

// True for MIDI files, which we render to WAV in-process (via midi_render.cpp)
// instead of streaming raw, since browsers can't play MIDI natively.
static bool is_midi_extension(const char *filename) {
    if (!filename) return false;
    const char *ext = strrchr(filename, '.');
    if (!ext) return false;
    return strcasecmp_ext(ext, ".mid") == 0 || strcasecmp_ext(ext, ".midi") == 0 ||
           strcasecmp_ext(ext, ".kar") == 0;
}

// Render MIDI bytes (already read into memory) to WAV and send as the
// response body. Sent in one shot (no range support) since these are
// freshly synthesized, not seekable source files.
static void serve_rendered_midi_wav(int client_socket, int tunnel_id,
                                     const std::vector<uint8_t> &midi_bytes) {
    std::vector<uint8_t> wav;
    if (!render_midi_to_wav(midi_bytes, wav)) {
        const char *msg = "Failed to render MIDI file";
        std::string response = http_response_headers(500, "text/plain", strlen(msg));
        response += msg;
        socket_send_safe(client_socket, response.c_str(), response.length());
        SDL_Log("[Tunnel %d] MIDI render failed", tunnel_id);
        return;
    }

    std::string response = http_response_headers(200, "audio/wav", wav.size());
    int header_sent = socket_send_safe(client_socket, response.c_str(), response.length());
    if (header_sent > 0) {
        socket_send_safe(client_socket, (const char *)wav.data(), (int)wav.size());
    }
    SDL_Log("[Tunnel %d] Served MIDI rendered to WAV (%zu bytes)", tunnel_id, wav.size());
}

// Client opts in to server-side VOC->WAV rendering via this header.
// Without it, .voc files are served as-is (plain download).
static bool client_wants_voc_render(const char *raw_request) {
    return strstr(raw_request, "X-Render-Voc:") != nullptr;
}

// True for VOC files, which we render to WAV in-process (via voc_render.cpp)
// instead of streaming raw, since browsers can't play VOC natively.
static bool is_voc_extension(const char *filename) {
    if (!filename) return false;
    const char *ext = strrchr(filename, '.');
    if (!ext) return false;
    return strcasecmp_ext(ext, ".voc") == 0;
}

// Render VOC bytes (already read into memory) to WAV and send as the
// response body. Sent in one shot (no range support) since these are
// freshly converted, not seekable source files.
static void serve_rendered_voc_wav(int client_socket, int tunnel_id,
                                    const std::vector<uint8_t> &voc_bytes) {
    std::vector<uint8_t> wav;
    if (!render_voc_to_wav(voc_bytes, wav)) {
        const char *msg = "Failed to render VOC file";
        std::string response = http_response_headers(500, "text/plain", strlen(msg));
        response += msg;
        socket_send_safe(client_socket, response.c_str(), response.length());
        SDL_Log("[Tunnel %d] VOC render failed", tunnel_id);
        return;
    }

    std::string response = http_response_headers(200, "audio/wav", wav.size());
    int header_sent = socket_send_safe(client_socket, response.c_str(), response.length());
    if (header_sent > 0) {
        socket_send_safe(client_socket, (const char *)wav.data(), (int)wav.size());
    }
    SDL_Log("[Tunnel %d] Served VOC rendered to WAV (%zu bytes)", tunnel_id, wav.size());
}

// AU/SND file rendering
static bool client_wants_au_render(const char *raw_request) {
    return strstr(raw_request, "X-Render-Au:") != nullptr;
}

static bool is_au_extension(const char *filename) {
    if (!filename) return false;
    const char *ext = strrchr(filename, '.');
    if (!ext) return false;
    return strcasecmp_ext(ext, ".au") == 0 || strcasecmp_ext(ext, ".snd") == 0;
}

static void serve_rendered_au_wav(int client_socket, int tunnel_id,
                                   const std::vector<uint8_t> &au_bytes) {
    std::vector<uint8_t> wav;
    if (!render_au_to_wav(au_bytes, wav)) {
        const char *msg = "Failed to convert AU file";
        std::string response = http_response_headers(500, "text/plain", strlen(msg));
        response += msg;
        socket_send_safe(client_socket, response.c_str(), response.length());
        SDL_Log("[Tunnel %d] AU convert failed", tunnel_id);
        return;
    }

    std::string response = http_response_headers(200, "audio/wav", wav.size());
    int header_sent = socket_send_safe(client_socket, response.c_str(), response.length());
    if (header_sent > 0) {
        socket_send_safe(client_socket, (const char *)wav.data(), (int)wav.size());
    }
    SDL_Log("[Tunnel %d] Served AU converted to WAV (%zu bytes)", tunnel_id, wav.size());
}

// AIFF file rendering
static bool client_wants_aiff_render(const char *raw_request) {
    return strstr(raw_request, "X-Render-Aiff:") != nullptr;
}

static bool is_aiff_extension(const char *filename) {
    if (!filename) return false;
    const char *ext = strrchr(filename, '.');
    if (!ext) return false;
    return strcasecmp_ext(ext, ".aiff") == 0 || strcasecmp_ext(ext, ".aif") == 0;
}

static void serve_rendered_aiff_wav(int client_socket, int tunnel_id,
                                     const std::vector<uint8_t> &aiff_bytes) {
    std::vector<uint8_t> wav;
    if (!render_aiff_to_wav(aiff_bytes, wav)) {
        const char *msg = "Failed to convert AIFF file";
        std::string response = http_response_headers(500, "text/plain", strlen(msg));
        response += msg;
        socket_send_safe(client_socket, response.c_str(), response.length());
        SDL_Log("[Tunnel %d] AIFF convert failed", tunnel_id);
        return;
    }

    std::string response = http_response_headers(200, "audio/wav", wav.size());
    int header_sent = socket_send_safe(client_socket, response.c_str(), response.length());
    if (header_sent > 0) {
        socket_send_safe(client_socket, (const char *)wav.data(), (int)wav.size());
    }
    SDL_Log("[Tunnel %d] Served AIFF converted to WAV (%zu bytes)", tunnel_id, wav.size());
}

// Audio format conversion (M4A, WMA, MP2, etc.) — platform-specific
#ifdef _WIN32
    #include "convertm4atowavwin.h"
    // Windows uses convertAudioToWavInMemory (generic for M4A/WMA)
    static bool convert_audio_to_wav_memory(const std::vector<uint8_t>& audio_data, 
                                             std::vector<uint8_t>& wav_data,
                                             const char* file_extension) {
        return convertAudioToWavInMemory(audio_data, wav_data, file_extension);
    }
#else
    #include "convertm4atowavlin.h"
    // Linux uses convertM4aToWavInMemory (ffmpeg-based, generic)
    static bool convert_audio_to_wav_memory(const std::vector<uint8_t>& audio_data, 
                                             std::vector<uint8_t>& wav_data,
                                             const char* file_extension) {
        return convertM4aToWavInMemory(audio_data, wav_data);
    }
#endif

// Helper to detect if a file needs conversion
static bool is_convertible_audio_format(const char* filename) {
    if (!filename) return false;
    const char* ext = strrchr(filename, '.');
    if (!ext) return false;
    // Check for M4A, WMA, MP2, MPA
    return (strcasecmp_ext(ext, ".m4a") == 0 || strcasecmp_ext(ext, ".m4p") == 0 ||
            strcasecmp_ext(ext, ".wma") == 0 || strcasecmp_ext(ext, ".mp2") == 0 || 
            strcasecmp_ext(ext, ".mpa") == 0);
}

static bool client_wants_audio_convert(const char *raw_request) {
    return strstr(raw_request, "X-Convert-Audio:") != nullptr;
}

static void serve_converted_audio_wav(int client_socket, int tunnel_id,
                                       const std::vector<uint8_t> &audio_bytes,
                                       const char *original_filename) {
    std::vector<uint8_t> wav;
    
    const char* ext = strrchr(original_filename, '.');
    if (!ext) {
        const char *msg = "Failed to detect audio format";
        std::string response = http_response_headers(400, "text/plain", strlen(msg));
        response += msg;
        socket_send_safe(client_socket, response.c_str(), response.length());
        SDL_Log("[Tunnel %d] Audio convert failed: cannot detect format", tunnel_id);
        return;
    }
    
    const char *format = ext + 1;  // Remove leading dot
    
    if (!convert_audio_to_wav_memory(audio_bytes, wav, format)) {
        const char *msg = "Failed to convert audio file";
        std::string response = http_response_headers(500, "text/plain", strlen(msg));
        response += msg;
        socket_send_safe(client_socket, response.c_str(), response.length());
        SDL_Log("[Tunnel %d] Audio convert failed", tunnel_id);
        return;
    }

    std::string response = http_response_headers(200, "audio/wav", wav.size());
    int header_sent = socket_send_safe(client_socket, response.c_str(), response.length());
    if (header_sent > 0) {
        socket_send_safe(client_socket, (const char *)wav.data(), (int)wav.size());
    }
    SDL_Log("[Tunnel %d] Served %s converted to WAV (%zu bytes)", tunnel_id, original_filename, wav.size());
}

static std::string url_encode(const std::string &str) {
    std::string encoded;
    for (char c : str) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || 
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            // Unreserved characters — don't encode
            encoded += c;
        } else if (c == ' ') {
            encoded += '+';
        } else if (c == '(' || c == ')') {
            // Encode parentheses as %28 and %29
            encoded += (c == '(') ? "%28" : "%29";
        } else {
            // Encode everything else as %XX
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
            encoded += buf;
        }
    }
    return encoded;
}

static std::string url_decode(const std::string &str) {
    std::string decoded;
    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == '%' && i + 2 < str.length()) {
            int value = 0;
            sscanf(str.c_str() + i + 1, "%2x", &value);
            decoded += static_cast<char>(value);
            i += 2;
        } else {
            decoded += str[i];
        }
    }
    return decoded;
}

// ============================================================================
// Streaming ZIP creation for POST /api/mget — builds the archive with
// miniz's callback-based writer so member files are compressed and written
// straight out over the socket as HTTP chunks. Nothing beyond a small
// in-flight buffer is ever held in memory, even for large/many-file zips.
// ============================================================================

// miniz write callback: wraps each chunk of zip output in HTTP chunked
// framing ("<hex-size>\r\n<data>\r\n") and sends it immediately.
struct ZipSocketWriteCtx {
    int socket;
    bool failed = false;
};

static size_t zip_socket_write_cb(void *pOpaque, mz_uint64 file_ofs, const void *pBuf, size_t n) {
    (void)file_ofs;
    ZipSocketWriteCtx *ctx = (ZipSocketWriteCtx *)pOpaque;
    if (ctx->failed) return 0;

    char size_line[32];
    int size_line_len = snprintf(size_line, sizeof(size_line), "%zx\r\n", n);
    if (socket_send_safe(ctx->socket, size_line, size_line_len) <= 0) { ctx->failed = true; return 0; }
    if (n > 0 && socket_send_safe(ctx->socket, (const char *)pBuf, (int)n) <= 0) { ctx->failed = true; return 0; }
    if (socket_send_safe(ctx->socket, "\r\n", 2) <= 0) { ctx->failed = true; return 0; }
    return n;
}

// miniz read callback backed by a local FILE* (local filesystem mode).
static size_t zip_local_read_cb(void *pOpaque, mz_uint64 file_ofs, void *pBuf, size_t n) {
    FILE *f = (FILE *)pOpaque;
#ifdef _WIN32
    _fseeki64(f, (int64_t)file_ofs, SEEK_SET);
#else
    fseeko(f, (off_t)file_ofs, SEEK_SET);
#endif
    return fread(pBuf, 1, n, f);
}

// miniz read callback backed by an open SFTP handle. Reads happen
// sequentially in practice (single-pass compression), so this only seeks
// when miniz asks for an offset other than the one already reached.
struct SftpZipReadCtx {
    LIBSSH2_SFTP_HANDLE *handle;
    LIBSSH2_SESSION *sess;
    uint64_t pos = 0;
};

static size_t zip_sftp_read_cb(void *pOpaque, mz_uint64 file_ofs, void *pBuf, size_t n) {
    SftpZipReadCtx *ctx = (SftpZipReadCtx *)pOpaque;
    if (file_ofs != ctx->pos) {
        SftpOp op(ctx->sess);
        libssh2_sftp_seek64(ctx->handle, file_ofs);
        ctx->pos = file_ofs;
    }
    size_t total = 0;
    while (total < n) {
        int r;
        { SftpOp op(ctx->sess); r = libssh2_sftp_read(ctx->handle, (char *)pBuf + total, n - total); }
        if (r <= 0) break;
        total += (size_t)r;
    }
    ctx->pos += total;
    return total;
}

// Just the filename portion of a path, used as the in-zip entry name so
// mget'd files land flat in the archive rather than nested under their
// full server-side path.
static std::string zip_entry_name(const std::string &path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Parses {"files": ["...", "..."]} into decoded paths with a lightweight scan.
static std::vector<std::string> parse_files_array(const char *body) {
    std::vector<std::string> out;
    const char *files_ptr = body ? strstr(body, "\"files\":") : nullptr;
    if (!files_ptr) return out;
    files_ptr = strchr(files_ptr, '[');
    while (files_ptr && *files_ptr && *files_ptr != ']') {
        const char *path_start = strchr(files_ptr, '"');
        if (!path_start) break;
        path_start++;
        const char *path_end = strchr(path_start, '"');
        if (!path_end) break;
        out.push_back(url_decode(std::string(path_start, path_end)));
        files_ptr = path_end + 1;
    }
    return out;
}

// Chunked zip response headers, shared by local and SFTP mode.
static bool send_mget_zip_headers(int client_socket) {
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/zip\r\n"
        "Content-Disposition: attachment; filename=\"download.zip\"\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n"
        "\r\n";
    return socket_send_safe(client_socket, response.c_str(), (int)response.length()) > 0;
}

// Builds & streams the mget zip for local-filesystem mode.
static void handle_mget_local(int client_socket, int tunnel_id, const std::vector<std::string> &requested) {
    if (!send_mget_zip_headers(client_socket)) return;

    ZipSocketWriteCtx write_ctx{ client_socket };
    mz_zip_archive zip = {};
    zip.m_pWrite = zip_socket_write_cb;
    zip.m_pIO_opaque = &write_ctx;
    mz_zip_writer_init(&zip, 0);

    int added = 0;
    for (const auto &req_path : requested) {
        if (write_ctx.failed) break;
        std::string fs_path;
        if (!local_resolve_path(req_path, fs_path)) continue;

        FILE *f = fopen(fs_path.c_str(), "rb");
        if (!f) { SDL_Log("[Tunnel %d] mget: skip unreadable %s", tunnel_id, fs_path.c_str()); continue; }

        struct stat st;
        bool have_stat = (stat(fs_path.c_str(), &st) == 0);
        uint64_t size = have_stat ? (uint64_t)st.st_size : 0;
        std::string entry_name = zip_entry_name(req_path);

        MZ_TIME_T mtime = have_stat ? (MZ_TIME_T)st.st_mtime : (MZ_TIME_T)0;
        mz_bool ok = mz_zip_writer_add_read_buf_callback(
            &zip, entry_name.c_str(), zip_local_read_cb, f, size,
            have_stat ? &mtime : nullptr, nullptr, 0, MZ_DEFAULT_COMPRESSION, nullptr, 0, nullptr, 0);
        fclose(f);
        if (ok) added++;
        else SDL_Log("[Tunnel %d] mget: failed to add %s to zip", tunnel_id, fs_path.c_str());
    }

    if (!write_ctx.failed) mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    if (!write_ctx.failed) socket_send_safe(client_socket, "0\r\n\r\n", 5);
    SDL_Log("[Tunnel %d] mget (local): zipped %d/%zu file(s)", tunnel_id, added, requested.size());
}

// Builds & streams the mget zip for SFTP-backed remote mode.
static void handle_mget_remote(int client_socket, int tunnel_id, LIBSSH2_SESSION *sess,
                                LIBSSH2_SFTP *sftp, const std::vector<std::string> &requested) {
    if (!send_mget_zip_headers(client_socket)) return;

    ZipSocketWriteCtx write_ctx{ client_socket };
    mz_zip_archive zip = {};
    zip.m_pWrite = zip_socket_write_cb;
    zip.m_pIO_opaque = &write_ctx;
    mz_zip_writer_init(&zip, 0);

    int added = 0;
    for (const auto &req_path : requested) {
        if (write_ctx.failed) break;

        LIBSSH2_SFTP_ATTRIBUTES attrs;
        LIBSSH2_SFTP_HANDLE *handle;
        int stat_rc;
        {
            SftpOp op(sess);
            stat_rc = libssh2_sftp_stat(sftp, req_path.c_str(), &attrs);
            handle = libssh2_sftp_open(sftp, req_path.c_str(), LIBSSH2_FXF_READ, LIBSSH2_SFTP_S_IRUSR);
        }
        if (!handle) { SDL_Log("[Tunnel %d] mget: skip unreadable %s", tunnel_id, req_path.c_str()); continue; }

        uint64_t size = (stat_rc == 0 && (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE)) ? attrs.filesize : 0;
        bool have_mtime = (stat_rc == 0 && (attrs.flags & LIBSSH2_SFTP_ATTR_ACMODTIME));
        std::string entry_name = zip_entry_name(req_path);
        SftpZipReadCtx read_ctx{ handle, sess };

        MZ_TIME_T mtime = have_mtime ? (MZ_TIME_T)attrs.mtime : (MZ_TIME_T)0;
        mz_bool ok = mz_zip_writer_add_read_buf_callback(
            &zip, entry_name.c_str(), zip_sftp_read_cb, &read_ctx, size,
            have_mtime ? &mtime : nullptr, nullptr, 0, MZ_DEFAULT_COMPRESSION, nullptr, 0, nullptr, 0);

        { SftpOp op(sess); libssh2_sftp_close_handle(handle); }
        if (ok) added++;
        else SDL_Log("[Tunnel %d] mget: failed to add %s to zip", tunnel_id, req_path.c_str());
    }

    if (!write_ctx.failed) mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    if (!write_ctx.failed) socket_send_safe(client_socket, "0\r\n\r\n", 5);
    SDL_Log("[Tunnel %d] mget (remote): zipped %d/%zu file(s)", tunnel_id, added, requested.size());
}

// ============================================================================
// HTTP Request Handler
// ============================================================================

// Serves GET (dir listing / file download w/ range) and POST /api/listfiles
// straight off the local filesystem — read-only, no SSH/SFTP involved.
static void handle_http_request_local(int client_socket, int tunnel_id,
                                       const char *method, const char *path,
                                       const char *buffer, int bytes_read) {
    (void)bytes_read;

    // Serve /public/* files from embedded content (don't list in directory)
    if (strcmp(method, "GET") == 0 && strstr(path, "/public/") == path) {
        PublicFile file = resolve_public_file(path);
        serve_public_file(client_socket, tunnel_id, file);
        SDL_Log("[Tunnel %d] Served public file (local mode)", tunnel_id);
        return;
    }

    if (strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) {
        std::string decoded_path = url_decode(path);
        bool is_head_request = (strcmp(method, "HEAD") == 0);
        std::string fs_path;
        if (!local_resolve_path(decoded_path, fs_path)) {
            std::string response = http_response_headers(400, "text/plain", 7);
            response += "Bad path";
            socket_send_safe(client_socket, response.c_str(), response.length());
            SDL_Log("[Tunnel %d] Rejected path traversal attempt: %s", tunnel_id, decoded_path.c_str());
            return;
        }

        struct stat st;
        bool exists = (stat(fs_path.c_str(), &st) == 0);
        bool is_directory = exists && S_ISDIR(st.st_mode);

        if (!exists) {
            std::string data =  get_404();
            std::string response = http_response_headers(404, "text/html", data.length());
            response+=data;
            socket_send_safe(client_socket, response.c_str(), response.length());
            SDL_Log("[Tunnel %d] File not found: %s", tunnel_id, fs_path.c_str());
        }
        else if (is_directory) {
            std::string html = get_index_html();
            std::string response = http_response_headers(200, "text/html", html.length());
            response += html;
            socket_send_safe(client_socket, response.c_str(), response.length());
            SDL_Log("[Tunnel %d] Served directory listing HTML: %s", tunnel_id, fs_path.c_str());
        }
        else if (is_midi_extension(fs_path.c_str()) && client_wants_midi_render(buffer)) {
            FILE *mf = fopen(fs_path.c_str(), "rb");
            if (!mf) {
                std::string data = get_404();
                std::string response = http_response_headers(404, "text/html", data.length());
                response += data;
                socket_send_safe(client_socket, response.c_str(), response.length());
                SDL_Log("[Tunnel %d] Failed to open MIDI: %s", tunnel_id, fs_path.c_str());
            } else {
                std::vector<uint8_t> midi_bytes((size_t)st.st_size);
                size_t rd = fread(midi_bytes.data(), 1, midi_bytes.size(), mf);
                fclose(mf);
                midi_bytes.resize(rd);
                serve_rendered_midi_wav(client_socket, tunnel_id, midi_bytes);
            }
        }
        else if (is_voc_extension(fs_path.c_str()) && client_wants_voc_render(buffer)) {
            FILE *vf = fopen(fs_path.c_str(), "rb");
            if (!vf) {
                std::string data = get_404();
                std::string response = http_response_headers(404, "text/html", data.length());
                response += data;
                socket_send_safe(client_socket, response.c_str(), response.length());
                SDL_Log("[Tunnel %d] Failed to open VOC: %s", tunnel_id, fs_path.c_str());
            } else {
                std::vector<uint8_t> voc_bytes((size_t)st.st_size);
                size_t rd = fread(voc_bytes.data(), 1, voc_bytes.size(), vf);
                fclose(vf);
                voc_bytes.resize(rd);
                serve_rendered_voc_wav(client_socket, tunnel_id, voc_bytes);
            }
        }
        else if (is_au_extension(fs_path.c_str()) && client_wants_au_render(buffer)) {
            FILE *af = fopen(fs_path.c_str(), "rb");
            if (!af) {
                std::string data = get_404();
                std::string response = http_response_headers(404, "text/html", data.length());
                response += data;
                socket_send_safe(client_socket, response.c_str(), response.length());
                SDL_Log("[Tunnel %d] Failed to open AU: %s", tunnel_id, fs_path.c_str());
            } else {
                std::vector<uint8_t> au_bytes((size_t)st.st_size);
                size_t rd = fread(au_bytes.data(), 1, au_bytes.size(), af);
                fclose(af);
                au_bytes.resize(rd);
                serve_rendered_au_wav(client_socket, tunnel_id, au_bytes);
            }
        }
        else if (is_aiff_extension(fs_path.c_str()) && client_wants_aiff_render(buffer)) {
            FILE *aff = fopen(fs_path.c_str(), "rb");
            if (!aff) {
                std::string data = get_404();
                std::string response = http_response_headers(404, "text/html", data.length());
                response += data;
                socket_send_safe(client_socket, response.c_str(), response.length());
                SDL_Log("[Tunnel %d] Failed to open AIFF: %s", tunnel_id, fs_path.c_str());
            } else {
                std::vector<uint8_t> aiff_bytes((size_t)st.st_size);
                size_t rd = fread(aiff_bytes.data(), 1, aiff_bytes.size(), aff);
                fclose(aff);
                aiff_bytes.resize(rd);
                serve_rendered_aiff_wav(client_socket, tunnel_id, aiff_bytes);
            }
        }
        else if (is_convertible_audio_format(fs_path.c_str()) && client_wants_audio_convert(buffer)) {
            FILE *af = fopen(fs_path.c_str(), "rb");
            if (!af) {
                std::string data = get_404();
                std::string response = http_response_headers(404, "text/html", data.length());
                response += data;
                socket_send_safe(client_socket, response.c_str(), response.length());
                SDL_Log("[Tunnel %d] Failed to open audio: %s", tunnel_id, fs_path.c_str());
            } else {
                std::vector<uint8_t> audio_bytes((size_t)st.st_size);
                size_t rd = fread(audio_bytes.data(), 1, audio_bytes.size(), af);
                fclose(af);
                audio_bytes.resize(rd);
                serve_converted_audio_wav(client_socket, tunnel_id, audio_bytes, fs_path.c_str());
            }
        }
        else {
            FILE *f = fopen(fs_path.c_str(), "rb");
            if (!f) {
                std::string data  = get_404();
                std::string response = http_response_headers(404, "text/html", data.length());
                response+=data;
                socket_send_safe(client_socket, response.c_str(), response.length());
                SDL_Log("[Tunnel %d] Failed to open: %s", tunnel_id, fs_path.c_str());
                return;
            }

            uint64_t file_size = (uint64_t)st.st_size;
            const char *content_type = get_mime_type(fs_path.c_str());

            uint64_t range_start = 0, range_end = (file_size ? file_size - 1 : 0);
            bool is_partial = parse_range_header(buffer, file_size, range_start, range_end);
            if (is_partial) {
#ifdef _WIN32
                _fseeki64(f, (int64_t)range_start, SEEK_SET);
#else
                fseeko(f, (off_t)range_start, SEEK_SET);
#endif
            }

            std::string response = http_file_response_headers(is_partial, range_start, range_end,
                                                                file_size, content_type);
            int header_sent = socket_send_safe(client_socket, response.c_str(), response.length());

            if (header_sent > 0 && !is_head_request) {
                const int BUFFER_SIZE = 262144;  // 256KB
                char *fbuffer = new char[BUFFER_SIZE];
                uint64_t total_sent = 0;
                uint64_t bytes_wanted = is_partial ? (range_end - range_start + 1) : file_size;

                while (total_sent < bytes_wanted) {
                    if (!socket_is_valid(client_socket)) {
                        SDL_Log("[Tunnel %d] Socket invalid during download: %s (sent %llu/%llu bytes)",
                                tunnel_id, fs_path.c_str(), total_sent, bytes_wanted);
                        break;
                    }

                    size_t to_read = (bytes_wanted - total_sent > (uint64_t)BUFFER_SIZE)
                                    ? (size_t)BUFFER_SIZE
                                    : (size_t)(bytes_wanted - total_sent);
                    size_t n = fread(fbuffer, 1, to_read, f);
                    if (n == 0) break;

                    int sent = socket_send_safe(client_socket, fbuffer, (int)n);
                    if (sent <= 0) break;
                    total_sent += sent;
                }

                delete[] fbuffer;
                SDL_Log("[Tunnel %d] Downloaded %s%s: %s (%llu bytes)", tunnel_id,
                        is_partial ? "range of " : "", "file", fs_path.c_str(), total_sent);
            } else if (is_head_request && header_sent > 0) {
                SDL_Log("[Tunnel %d] HEAD: sent headers for %s (size: %llu bytes)", tunnel_id, fs_path.c_str(), (unsigned long long)file_size);
            }

            fclose(f);
        }
    }
    else if (strcmp(method, "POST") == 0 && strstr(path, "/api/listfiles") != nullptr) {
        char *body_start = strstr((char *)buffer, "\r\n\r\n");
        if (!body_start) body_start = strstr((char *)buffer, "\n\n");
        if (body_start && *body_start == '\n') body_start += 2;
        else if (body_start) body_start += 4;
        else body_start = (char *)buffer;

        char dir_path[1024] = "/";
        char *dir_ptr = strstr(body_start, "\"dir\":");
        if (dir_ptr) {
            dir_ptr += 6;
            while (*dir_ptr && (*dir_ptr == ' ' || *dir_ptr == '"')) dir_ptr++;
            int i = 0;
            while (i < 1023 && *dir_ptr && *dir_ptr != '"' && *dir_ptr != '}') {
                dir_path[i++] = *dir_ptr++;
            }
            dir_path[i] = '\0';
        }

        std::string decoded_dir = url_decode(dir_path);
        std::string fs_path;
        std::vector<RemoteFile> entries;
        if (local_resolve_path(decoded_dir, fs_path)) {
            local_list_directory(fs_path.c_str(), entries);
        }

        SDL_Log("[Tunnel %d] Listed directory: %s (%zu entries)", tunnel_id, fs_path.c_str(), entries.size());

        std::string home_path;
        bool have_home = local_home_url_path(home_path);
        std::string json = build_directory_json(entries, have_home ? &home_path : nullptr);
        std::string response = http_response_headers(200, "application/json", json.length());
        response += json;
        socket_send_safe(client_socket, response.c_str(), response.length());
    }
    else if (strcmp(method, "POST") == 0 && strstr(path, "/api/mget") != nullptr) {
        char *body_start = strstr((char *)buffer, "\r\n\r\n");
        if (!body_start) body_start = strstr((char *)buffer, "\n\n");
        if (body_start && *body_start == '\n') body_start += 2;
        else if (body_start) body_start += 4;
        else body_start = (char *)buffer;

        std::vector<std::string> requested = parse_files_array(body_start);
        handle_mget_local(client_socket, tunnel_id, requested);
    }
    else {
        std::string response = http_response_headers(400, "text/plain", 18);
        response += "Method not allowed";
        socket_send_safe(client_socket, response.c_str(), response.length());
        SDL_Log("[Tunnel %d] Method not allowed (local mode, read-only): %s", tunnel_id, method);
    }
}

static void handle_http_request(int client_socket, int tunnel_id) {
    bool transfer_was_cancelled = false;
    
    // Read HTTP request (first 16KB max)
    char buffer[16384];
    memset(buffer, 0, sizeof(buffer));
    
    int bytes_read = socket_recv_safe(client_socket, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0) {
        closesocket(client_socket);
        SDL_Log("[Tunnel %d] recv() failed or empty request", tunnel_id);
        return;
    }
    
    // Parse request line
    char method[16] = {};
    char path[1024] = {};
    sscanf(buffer, "%15s %1023s", method, path);
    
    // Strip query string and fragment from path (keep just the path part)
    char *query_or_fragment = strchr(path, '?');
    if (!query_or_fragment) query_or_fragment = strchr(path, '#');
    if (query_or_fragment) *query_or_fragment = '\0';

    // If the path contains double (or more) slashes, e.g.
    // "//home/jbhall/Music", redirect to the normalized single-slash form
    // so the browser's address bar and any relative links end up clean.
    if (strcmp(method, "GET") == 0) {
        std::string normalized;
        if (normalize_path(path, normalized)) {
            std::string response = http_redirect_response(normalized.c_str());
            socket_send_safe(client_socket, response.c_str(), response.length());
            closesocket(client_socket);
            SDL_Log("[Tunnel %d] Redirecting '%s' -> '%s'", tunnel_id, path, normalized.c_str());
            return;
        }
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/favicon.ico") == 0) {
        std::string data = get_favicon();
        std::string response = http_public_file_headers("image/x-icon", data.length());
        response += data;
        socket_send_safe(client_socket, response.c_str(), response.length());
        closesocket(client_socket);
        SDL_Log("[Tunnel %d] Served embedded favicon.ico with 1-year cache", tunnel_id);
        return;
    }

    if (strcmp(method, "GET") == 0 && strstr(path, "/public/") == path) {
        PublicFile file = resolve_public_file(path);
        serve_public_file(client_socket, tunnel_id, file);
        closesocket(client_socket);
        return;
    }

    if (g_local_mode) {
        handle_http_request_local(client_socket, tunnel_id, method, path, buffer, bytes_read);
        closesocket(client_socket);
        SDL_Log("[Tunnel %d] Closed", tunnel_id);
        return;
    }

    // Get SSH session — this is the same session/transport the terminal and
    // the F4 SFTP console use. Lock only long enough to read the pointer.
    ssh_session_lock();
    LIBSSH2_SESSION *sess = ssh_get_session();
    ssh_session_unlock();

    if (!sess) {
        const char *msg = "SSH session not established";
        std::string response = http_response_headers(500, "text/plain", strlen(msg));
        response += msg;
        socket_send_safe(client_socket, response.c_str(), response.length());
        closesocket(client_socket);
        SDL_Log("[Tunnel %d] No SSH session available", tunnel_id);
        return;
    }

    // libssh2_sftp_init() opens a new "sftp" subsystem channel on the
    // existing SSH connection. Under load (many tunnels opening at once)
    // the server can transiently refuse a new channel even though the SSH
    // session itself is fine, so retry a few times before giving up.
    LIBSSH2_SFTP *sftp = nullptr;
    const int MAX_SFTP_INIT_ATTEMPTS = 3;
    for (int attempt = 1; attempt <= MAX_SFTP_INIT_ATTEMPTS && !sftp; attempt++) {
        SftpOp op(sess);
        sftp = libssh2_sftp_init(sess);
        if (!sftp && attempt < MAX_SFTP_INIT_ATTEMPTS) {
            SDL_Log("[Tunnel %d] SFTP init failed (attempt %d/%d), retrying",
                    tunnel_id, attempt, MAX_SFTP_INIT_ATTEMPTS);
            SDL_Delay(200);
        }
    }

    if (!sftp) {
        const char *msg = "SFTP initialization failed";
        std::string response = http_response_headers(500, "text/plain", strlen(msg));
        response += msg;
        socket_send_safe(client_socket, response.c_str(), response.length());
        closesocket(client_socket);
        SDL_Log("[Tunnel %d] SFTP init failed after %d attempts", tunnel_id, MAX_SFTP_INIT_ATTEMPTS);
        return;
    }

    if (strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) {
        std::string decoded_path = url_decode(path);
        bool is_head_request = (strcmp(method, "HEAD") == 0);
        SDL_Log("[Tunnel %d] %s: encoded='%s' decoded='%s'", tunnel_id, method, path, decoded_path.c_str());
        LIBSSH2_SFTP_ATTRIBUTES attrs;
        int rc;
        { SftpOp op(sess); rc = libssh2_sftp_stat(sftp, decoded_path.c_str(), &attrs); }

        bool is_directory = (rc == 0 && (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && 
                            LIBSSH2_SFTP_S_ISDIR(attrs.permissions));
        
        if (rc != 0 && decoded_path != "/") {
            std::string data = get_404();
            std::string response = http_response_headers(404, "text/html", data.length());
            response += data;
            socket_send_safe(client_socket, response.c_str(), response.length());
            SDL_Log("[Tunnel %d] File not found: %s", tunnel_id, decoded_path.c_str());
        }
        else if (is_directory || decoded_path == "/") {
            std::string html = get_index_html();
            std::string response = http_response_headers(200, "text/html", html.length(), nullptr, "dir");
            response += html;
            socket_send_safe(client_socket, response.c_str(), response.length());
            SDL_Log("[Tunnel %d] Served directory listing HTML: %s", tunnel_id, decoded_path.c_str());
        }
        else if (is_midi_extension(decoded_path.c_str()) && client_wants_midi_render(buffer)) {
            std::vector<uint8_t> midi_bytes;
            bool got_file;
            { SftpOp op(sess); got_file = sftp_get_file(sftp, decoded_path.c_str(), midi_bytes); }
            if (!got_file) {
                std::string data = get_404();
                std::string response = http_response_headers(404, "text/html", data.length());
                response += data;
                socket_send_safe(client_socket, response.c_str(), response.length());
                SDL_Log("[Tunnel %d] Failed to fetch MIDI: %s", tunnel_id, decoded_path.c_str());
            } else {
                serve_rendered_midi_wav(client_socket, tunnel_id, midi_bytes);
            }
        }
        else if (is_voc_extension(decoded_path.c_str()) && client_wants_voc_render(buffer)) {
            std::vector<uint8_t> voc_bytes;
            bool got_file;
            { SftpOp op(sess); got_file = sftp_get_file(sftp, decoded_path.c_str(), voc_bytes); }
            if (!got_file) {
                std::string data = get_404();
                std::string response = http_response_headers(404, "text/html", data.length());
                response += data;
                socket_send_safe(client_socket, response.c_str(), response.length());
                SDL_Log("[Tunnel %d] Failed to fetch VOC: %s", tunnel_id, decoded_path.c_str());
            } else {
                serve_rendered_voc_wav(client_socket, tunnel_id, voc_bytes);
            }
        }
        else if (is_au_extension(decoded_path.c_str()) && client_wants_au_render(buffer)) {
            std::vector<uint8_t> au_bytes;
            bool got_file;
            { SftpOp op(sess); got_file = sftp_get_file(sftp, decoded_path.c_str(), au_bytes); }
            if (!got_file) {
                std::string data = get_404();
                std::string response = http_response_headers(404, "text/html", data.length());
                response += data;
                socket_send_safe(client_socket, response.c_str(), response.length());
                SDL_Log("[Tunnel %d] Failed to fetch AU: %s", tunnel_id, decoded_path.c_str());
            } else {
                serve_rendered_au_wav(client_socket, tunnel_id, au_bytes);
            }
        }
        else if (is_aiff_extension(decoded_path.c_str()) && client_wants_aiff_render(buffer)) {
            std::vector<uint8_t> aiff_bytes;
            bool got_file;
            { SftpOp op(sess); got_file = sftp_get_file(sftp, decoded_path.c_str(), aiff_bytes); }
            if (!got_file) {
                std::string data = get_404();
                std::string response = http_response_headers(404, "text/html", data.length());
                response += data;
                socket_send_safe(client_socket, response.c_str(), response.length());
                SDL_Log("[Tunnel %d] Failed to fetch AIFF: %s", tunnel_id, decoded_path.c_str());
            } else {
                serve_rendered_aiff_wav(client_socket, tunnel_id, aiff_bytes);
            }
        }
        else if (is_convertible_audio_format(decoded_path.c_str()) && client_wants_audio_convert(buffer)) {
            std::vector<uint8_t> audio_bytes;
            bool got_file;
            { SftpOp op(sess); got_file = sftp_get_file(sftp, decoded_path.c_str(), audio_bytes); }
            if (!got_file) {
                std::string data = get_404();
                std::string response = http_response_headers(404, "text/html", data.length());
                response += data;
                socket_send_safe(client_socket, response.c_str(), response.length());
                SDL_Log("[Tunnel %d] Failed to fetch audio: %s", tunnel_id, decoded_path.c_str());
            } else {
                serve_converted_audio_wav(client_socket, tunnel_id, audio_bytes, decoded_path.c_str());
            }
        }
        else {
            // Get file size first using stat
            LIBSSH2_SFTP_ATTRIBUTES file_attrs;
            int stat_rc;
            LIBSSH2_SFTP_HANDLE *file_handle;
            {
                SftpOp op(sess);
                stat_rc = libssh2_sftp_stat(sftp, decoded_path.c_str(), &file_attrs);
                file_handle = libssh2_sftp_open(sftp, decoded_path.c_str(),
                                                 LIBSSH2_FXF_READ,
                                                 LIBSSH2_SFTP_S_IRUSR);
            }
            uint64_t file_size = 0;
            if (stat_rc == 0 && (file_attrs.flags & LIBSSH2_SFTP_ATTR_SIZE)) {
                file_size = file_attrs.filesize;
            }

            if (file_handle) {
                // Extract filename from path
                const char *filename = decoded_path.c_str();
                const char *last_slash = strrchr(decoded_path.c_str(), '/');
                if (last_slash) {
                    filename = last_slash + 1;
                }
                
                const char *content_type = get_mime_type(decoded_path.c_str());

                uint64_t range_start = 0, range_end = (file_size ? file_size - 1 : 0);
                bool is_partial = parse_range_header(buffer, file_size, range_start, range_end);

                if (is_partial) {
                    SftpOp op(sess);
                    libssh2_sftp_seek64(file_handle, range_start);
                }

                std::string response = http_file_response_headers(is_partial, range_start, range_end,
                                                                    file_size, content_type);
                int header_sent = socket_send_safe(client_socket, response.c_str(), response.length());
                
                if (header_sent > 0 && !is_head_request) {
                    // Stream file in 256KB chunks
                    const int BUFFER_SIZE = 262144;  // 256KB
                    char *fbuffer = new char[BUFFER_SIZE];
                    uint64_t total_sent = 0;
                    uint64_t bytes_wanted = is_partial ? (range_end - range_start + 1) : file_size;
                    
                    while (total_sent < bytes_wanted) {
                        // CRITICAL: Check if socket is still valid BEFORE reading
                        if (!socket_is_valid(client_socket)) {
                            SDL_Log("[Tunnel %d] Socket invalid during download: %s (sent %llu/%llu bytes)", 
                                    tunnel_id, decoded_path.c_str(), total_sent, bytes_wanted);
                            transfer_was_cancelled = true;
                            break;
                        }
                        
                        int bytes_to_read = (bytes_wanted - total_sent > BUFFER_SIZE) 
                                          ? BUFFER_SIZE 
                                          : (int)(bytes_wanted - total_sent);
                        
                        int bytes_read;
                        { SftpOp op(sess); bytes_read = libssh2_sftp_read(file_handle, fbuffer, bytes_to_read); }
                        if (bytes_read <= 0) {
                            SDL_Log("[Tunnel %d] SFTP read ended: %d", tunnel_id, bytes_read);
                            break;
                        }
                        
                        int bytes_sent = socket_send_safe(client_socket, fbuffer, bytes_read);
                        if (bytes_sent <= 0) {
                            SDL_Log("[Tunnel %d] Download cancelled by client: %s (sent %llu/%llu bytes)", 
                                    tunnel_id, decoded_path.c_str(), total_sent, bytes_wanted);
                            transfer_was_cancelled = true;
                            break;
                        }
                        
                        total_sent += bytes_sent;
                    }
                    
                    if (!transfer_was_cancelled) {
                        SDL_Log("[Tunnel %d] Downloaded %s%s: %s (%llu bytes)", 
                                tunnel_id, is_partial ? "range of " : "", "file",
                                decoded_path.c_str(), (unsigned long long)total_sent);
                    }
                    
                    delete[] fbuffer;
                } else if (is_head_request && header_sent > 0) {
                    SDL_Log("[Tunnel %d] HEAD: sent headers for %s (size: %llu bytes)", 
                            tunnel_id, decoded_path.c_str(), (unsigned long long)file_size);
                } else {
                    SDL_Log("[Tunnel %d] Failed to send headers for: %s", tunnel_id, decoded_path.c_str());
                }
                
                { SftpOp op(sess); libssh2_sftp_close_handle(file_handle); }
            } else {
                std::string response = http_response_headers(500, "text/plain", 14);
                response += "Download error";
                socket_send_safe(client_socket, response.c_str(), response.length());
                SDL_Log("[Tunnel %d] Failed to download: %s", tunnel_id, decoded_path.c_str());
            }
        }
    }
    else if (strcmp(method, "POST") == 0 && strstr(path, "/api/mget") != nullptr) {
        char *body_start = strstr(buffer, "\r\n\r\n");
        if (!body_start) body_start = strstr(buffer, "\n\n");
        if (body_start && *body_start == '\n') body_start += 2;
        else if (body_start) body_start += 4;

        std::vector<std::string> requested = parse_files_array(body_start);
        handle_mget_remote(client_socket, tunnel_id, sess, sftp, requested);
    }
    else if (strcmp(method, "POST") == 0 && strstr(path, "/api/listfiles") != nullptr) {
        char *body_start = strstr(buffer, "\r\n\r\n");
        if (!body_start) body_start = strstr(buffer, "\n\n");
        if (body_start && *body_start == '\n') body_start += 2;
        else if (body_start) body_start += 4;
        else body_start = buffer;

        char dir_path[1024] = "/";
        char *dir_ptr = strstr(body_start ? body_start : buffer, "\"dir\":");
        if (dir_ptr) {
            dir_ptr += 6;
            while (*dir_ptr && (*dir_ptr == ' ' || *dir_ptr == '"')) dir_ptr++;
            int i = 0;
            while (i < 1023 && *dir_ptr && *dir_ptr != '"' && *dir_ptr != '}') {
                dir_path[i++] = *dir_ptr++;
            }
            dir_path[i] = '\0';
        }

        // URL decode the path
        std::string decoded_dir = url_decode(dir_path);
        
        std::vector<RemoteFile> entries;
        { SftpOp op(sess); sftp_list_directory(sftp, decoded_dir.c_str(), entries); }

        SDL_Log("[Tunnel %d] Listed directory: encoded='%s' decoded='%s' (%zu entries)", tunnel_id, dir_path, decoded_dir.c_str(), entries.size());

        std::string home_path = get_remote_home_dir(sess, sftp);
        std::string json = build_directory_json(entries, &home_path);
        std::string response = http_response_headers(200, "application/json", json.length());
        response += json;
        socket_send_safe(client_socket, response.c_str(), response.length());
    }
    else {
        std::string response = http_response_headers(400, "text/plain", 18);
        response += "Method not allowed";
        socket_send_safe(client_socket, response.c_str(), response.length());
        SDL_Log("[Tunnel %d] Method not allowed: %s", tunnel_id, method);
    }

    // Close SFTP subsystem (the underlying session stays open — shared with
    // the terminal and console). Always do this, even if the transfer was
    // cancelled — skipping it here leaks a channel on the server side, which
    // eventually exhausts the server's session/channel limit and makes
    // libssh2_sftp_init() start failing for new tunnels.
    if (sftp) {
        SftpOp op(sess);
        libssh2_sftp_shutdown(sftp);
    }

    closesocket(client_socket);
    SDL_Log("[Tunnel %d] Closed", tunnel_id);
}

// ============================================================================
// Web Server Thread
// ============================================================================

// Portable last-socket-error helpers (Winsock doesn't set errno)
#ifdef _WIN32
static int  ws_last_error()        { return WSAGetLastError(); }
static bool ws_err_addr_unusable(int e) { return e == WSAEADDRNOTAVAIL || e == WSAEAFNOSUPPORT; }
#else
static int  ws_last_error()        { return errno; }
static bool ws_err_addr_unusable(int e) { return e == EADDRNOTAVAIL || e == EAFNOSUPPORT; }
#endif

// Create a listening socket of the given family (AF_INET6 or AF_INET) bound
// to g_webserver_bind_addr, walking ports if auto-port is enabled.
// Returns the socket and sets *out_port on success, INVALID_SOCKET on failure.
// *out_family_unavailable is set when the family itself can't be used (e.g.
// IPv6 disabled or not compiled into the OS), so the caller can fall back.
static int webserver_open_listen_socket(int family, int *out_port, bool *out_family_unavailable) {
    *out_family_unavailable = false;

    const std::string &bind_addr = g_webserver_bind_addr;
    bool bind_is_v6 = bind_addr.find(':') != std::string::npos;

    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof(ss));
    socklen_t ss_len;
    struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&ss;
    struct sockaddr_in  *a4 = (struct sockaddr_in  *)&ss;

    if (family == AF_INET6) {
        // Accept plain IPv4 bind addresses (e.g. "0.0.0.0", "127.0.0.1") by
        // mapping them into IPv6 form so one dual-stack socket serves both.
        std::string bind_str = bind_addr;
        if (!bind_is_v6) {
            bind_str = (bind_str == "0.0.0.0") ? "::" : ("::ffff:" + bind_str);
        }
        a6->sin6_family = AF_INET6;
        if (inet_pton(AF_INET6, bind_str.c_str(), &a6->sin6_addr) <= 0) {
            SDL_Log("[WebServer] Invalid bind address: %s", bind_addr.c_str());
            return INVALID_SOCKET;
        }
        ss_len = sizeof(struct sockaddr_in6);
    } else {
        if (bind_is_v6) {
            SDL_Log("[WebServer] IPv6 bind address %s requested but IPv6 is unavailable",
                    bind_addr.c_str());
            return INVALID_SOCKET;
        }
        a4->sin_family = AF_INET;
        if (inet_pton(AF_INET, bind_addr.c_str(), &a4->sin_addr) <= 0) {
            SDL_Log("[WebServer] Invalid bind address: %s", bind_addr.c_str());
            return INVALID_SOCKET;
        }
        ss_len = sizeof(struct sockaddr_in);
    }

    int sock = socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET || sock < 0) {
        SDL_Log("[WebServer] socket(%s) failed: %s",
                family == AF_INET6 ? "IPv6" : "IPv4", strerror(ws_last_error()));
        *out_family_unavailable = true;
        return INVALID_SOCKET;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

    if (family == AF_INET6) {
        // Dual-stack: accept IPv4 connections on the same IPv6 socket (Linux/BSD
        // default to v6-only unless told otherwise; Windows defaults to dual-stack
        // already, but we set this explicitly either way).
        int v6only = 0;
        setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&v6only, sizeof(v6only));
    }

    // Try to bind to configured port, optionally incrementing if auto-port is enabled
    int port = g_webserver_configured_port;
    int bind_result = -1;
    int bind_errno = 0;
    int max_attempts = g_webserver_use_auto_port ? 100 : 1;

    for (int attempt = 0; attempt < max_attempts; attempt++) {
        if (family == AF_INET6) a6->sin6_port = htons(port);
        else                    a4->sin_port  = htons(port);
        bind_result = bind(sock, (struct sockaddr *)&ss, ss_len);
        if (bind_result == 0) break;
        bind_errno = ws_last_error();
        // An address the stack can't use at all won't get better on another port
        if (ws_err_addr_unusable(bind_errno)) break;
        if (!g_webserver_use_auto_port) break;
        port++;
    }

    if (bind_result < 0) {
        if (family == AF_INET6 && ws_err_addr_unusable(bind_errno)) {
            // IPv6 socket exists but the stack is disabled (e.g. disable_ipv6=1)
            *out_family_unavailable = true;
        }
        if (g_webserver_use_auto_port) {
            SDL_Log("[WebServer] bind() (%s) failed on ports %d-%d: %s",
                    family == AF_INET6 ? "IPv6" : "IPv4",
                    g_webserver_configured_port, g_webserver_configured_port + 99,
                    strerror(bind_errno));
        } else {
            SDL_Log("[WebServer] bind() (%s) failed on %s:%d: %s",
                    family == AF_INET6 ? "IPv6" : "IPv4",
                    bind_addr.c_str(), g_webserver_configured_port, strerror(bind_errno));
        }
        closesocket(sock);
        return INVALID_SOCKET;
    }

    *out_port = port;
    return sock;
}

static void webserver_thread_func() {
#ifdef _WIN32
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        SDL_Log("[WebServer] WSAStartup failed");
        return;
    }
#endif

    // Prefer a dual-stack IPv6 socket; fall back to plain IPv4 on systems
    // where IPv6 is disabled or unavailable (common on locked-down machines).
    int port = 0;
    bool v6_unavailable = false;
    g_listen_socket = webserver_open_listen_socket(AF_INET6, &port, &v6_unavailable);
    if (g_listen_socket == INVALID_SOCKET && v6_unavailable) {
        SDL_Log("[WebServer] IPv6 unavailable, falling back to IPv4");
        bool v4_unavailable = false;
        g_listen_socket = webserver_open_listen_socket(AF_INET, &port, &v4_unavailable);
    }
    if (g_listen_socket == INVALID_SOCKET) {
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }

    g_webserver_port = port;  // Store the port we're using
    SDL_Log("[WebServer] Starting on %s:%d", g_webserver_bind_addr.c_str(), port);

    if (listen(g_listen_socket, 5) < 0) {
        SDL_Log("[WebServer] listen() failed: %s", strerror(errno));
        closesocket(g_listen_socket);
        return;
    }

#ifndef _WIN32
    int flags = fcntl(g_listen_socket, F_GETFL, 0);
    fcntl(g_listen_socket, F_SETFL, flags | O_NONBLOCK);
#else
    u_long mode = 1;
    ioctlsocket(g_listen_socket, FIONBIO, &mode);
#endif

    g_webserver_running.store(true);
    SDL_Log("[WebServer] Listening on %s:%d", g_webserver_bind_addr.c_str(), g_webserver_port);

    while (!g_webserver_should_exit.load()) {
        struct sockaddr_in6 client_addr;
        memset(&client_addr, 0, sizeof(client_addr));
        socklen_t addr_len = sizeof(client_addr);

        int client_socket = accept(g_listen_socket, (struct sockaddr *)&client_addr, &addr_len);
        if (client_socket == INVALID_SOCKET) {
            SDL_Delay(10);
            continue;
        }

        int tunnel_id;
        SDL_LockMutex(g_tunnel_mutex);
        tunnel_id = ++g_tunnel_counter;
        SDL_UnlockMutex(g_tunnel_mutex);

        SDL_Log("[Tunnel %d] Opened", tunnel_id);
        
        // Spawn a thread to handle this request so we don't block the accept loop
        std::thread request_handler([client_socket, tunnel_id]() {
            handle_http_request(client_socket, tunnel_id);
        });
        request_handler.detach();  // Let it run independently
    }

    if (g_listen_socket != INVALID_SOCKET) {
        closesocket(g_listen_socket);
        g_listen_socket = INVALID_SOCKET;
    }

#ifdef _WIN32
    WSACleanup();
#endif

    g_webserver_running.store(false);
    SDL_Log("[WebServer] Stopped");
}

// ============================================================================
// Public API
// ============================================================================

bool sftp_webserver_start(const char *bind_addr, int port) {
    if (g_webserver_running.load()) {
        SDL_Log("[WebServer] Already running");
        return true;
    }

    // Clean up any lingering socket from previous run
    if (g_listen_socket != INVALID_SOCKET) {
        closesocket(g_listen_socket);
        g_listen_socket = INVALID_SOCKET;
    }

    // Validate and store configuration
    if (bind_addr) {
        g_webserver_bind_addr = bind_addr;
    } else {
        g_webserver_bind_addr = "127.0.0.1";
    }
    
    if (port <= 0 || port > 65535) {
        SDL_Log("[WebServer] Invalid port %d, using default 53716", port);
        port = 53716;
    }
    g_webserver_configured_port = port;
    g_webserver_use_auto_port = false;  // Don't auto-increment when explicitly configured

    g_local_mode = false;

    setup_signal_handlers();  // Install SIGPIPE handler
    
    if (!g_tunnel_mutex) {
        g_tunnel_mutex = SDL_CreateMutex();
    }

    g_webserver_should_exit.store(false);
    g_webserver_thread = new std::thread(webserver_thread_func);
    
    SDL_Delay(100);
    return g_webserver_running.load();
}

// Same server, but reads straight from the local filesystem under root_dir
// instead of going over SFTP — for when there's no SSH session to browse
// (e.g. F9 in a local shell). Read-only: browsing + download only.
bool sftp_webserver_start_local(const char *root_dir, const char *bind_addr, int port) {
    if (g_webserver_running.load()) {
        SDL_Log("[WebServer] Already running");
        return true;
    }

    // Clean up any lingering socket from previous run
    if (g_listen_socket != INVALID_SOCKET) {
        closesocket(g_listen_socket);
        g_listen_socket = INVALID_SOCKET;
    }

    // Validate and store configuration
    if (bind_addr) {
        g_webserver_bind_addr = bind_addr;
    } else {
        g_webserver_bind_addr = "127.0.0.1";
    }
    
    if (port <= 0 || port > 65535) {
        SDL_Log("[WebServer] Invalid port %d, using default 53716", port);
        port = 53716;
    }
    g_webserver_configured_port = port;
    g_webserver_use_auto_port = false;  // Don't auto-increment when explicitly configured

    g_local_mode = true;
    g_local_root = root_dir ? root_dir : "/";
    while (g_local_root.size() > 1 && g_local_root.back() == '/')
        g_local_root.pop_back();

    setup_signal_handlers();  // Install SIGPIPE handler

    if (!g_tunnel_mutex) {
        g_tunnel_mutex = SDL_CreateMutex();
    }

    g_webserver_should_exit.store(false);
    g_webserver_thread = new std::thread(webserver_thread_func);

    SDL_Delay(100);
    SDL_Log("[WebServer] Local mode serving: %s on %s:%d", g_local_root.c_str(), g_webserver_bind_addr.c_str(), port);
    return g_webserver_running.load();
}

void sftp_webserver_stop() {
    if (!g_webserver_running.load()) return;

    SDL_Log("[WebServer] Shutdown requested");
    g_webserver_should_exit.store(true);

    // Close the listening socket to interrupt accept()
    if (g_listen_socket != INVALID_SOCKET) {
        closesocket(g_listen_socket);
        g_listen_socket = INVALID_SOCKET;
    }

    if (g_webserver_thread) {
        g_webserver_thread->join();
        delete g_webserver_thread;
        g_webserver_thread = nullptr;
    }

    if (g_tunnel_mutex) {
        SDL_DestroyMutex(g_tunnel_mutex);
        g_tunnel_mutex = nullptr;
    }

    g_webserver_running.store(false);
    g_local_mode = false;
}

bool sftp_webserver_running() {
    return g_webserver_running.load();
}

int sftp_webserver_get_port() {
    return g_webserver_port;
}

#endif // USESSH
