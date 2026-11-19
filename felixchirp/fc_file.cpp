#ifndef _WIN32
#  include <dirent.h>
#  include <sys/stat.h>
#  include <unistd.h>
#  include <fcntl.h>
#else
#  include <windows.h>
#  include <shlobj.h>
#  include <io.h>
#  include <fcntl.h>
#endif

#include <string>
#include <vector>
#include <cstring>
#include <algorithm>
#include <sys/stat.h>
#include <stdlib.h>

#include "felixchirp.h"

#  include "../ssh_session.h"
#  include "../sftp_overlay.h"
#  include <libssh2_sftp.h>

#include "stb_image.h"

// ---- libwebp (for WebP support) -------------------------------------------
// WebP is decoded via libwebp if available
// Link with: -lwebp
#  include <webp/decode.h>


extern ImageViewer g_iv;
extern TextDocument s_text_doc;
extern bool s_viewing_text;
bool g_video_capable = false;  // Will be set during iv_open()

// ============================================================================
// CROSS-PLATFORM TEMP FILE
// ============================================================================

// Write data to a new temp file with the given extension. Returns the path on
// success or "" on failure. Caller must delete the file when done.
std::string iv_write_tempfile(const unsigned char *data, size_t len, const char *ext) {
    char path[512];
#ifndef _WIN32
    snprintf(path, sizeof(path), "/tmp/iv_tmp_XXXXXX%s", ext ? ext : "");
    int fd = mkstemps(path, ext ? (int)strlen(ext) : 0);
    if (fd < 0) return "";
    ssize_t written = write(fd, data, len);
    close(fd);
    if (written != (ssize_t)len) { unlink(path); return ""; }
#else
    char tmp_dir[MAX_PATH];
    if (!GetTempPathA(sizeof(tmp_dir), tmp_dir)) return "";
    char base[MAX_PATH];
    if (!GetTempFileNameA(tmp_dir, "iv_", 0, base)) return "";
    // GetTempFileName creates a .tmp file — rename to include the right extension
    snprintf(path, sizeof(path), "%s%s", base, ext ? ext : "");
    // Write to the path directly (overwrite the placeholder)
    FILE *f = fopen(path, "wb");
    if (!f) { DeleteFileA(base); return ""; }
    bool ok = (fwrite(data, 1, len, f) == len);
    fclose(f);
    DeleteFileA(base);  // remove the placeholder .tmp
    if (!ok) { DeleteFileA(path); return ""; }
#endif
    return std::string(path);
}

void iv_delete_tempfile(const char *path) {
    if (!path || !path[0]) return;
#ifndef _WIN32
    unlink(path);
#else
    DeleteFileA(path);
#endif
}

// Truncate a filename so it fits within max_chars, preserving the extension.
// Result is written into out (must be at least max_chars+1 bytes).
// Uses a UTF-8-safe approach by working in bytes (filenames are usually ASCII).
void iv_truncate_name(const char *name, int max_chars, char *out, int out_sz) {
    int len = (int)strlen(name);
    if (len <= max_chars) {
        strncpy(out, name, out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }
    // Find extension (last dot)
    const char *dot = strrchr(name, '.');
    int ext_len = dot ? (int)strlen(dot) : 0;  // includes the dot
    // We need: stem... + ellipsis(1 char '…' = 3 bytes) + ext
    // Keep at least "A…ext" = 1 stem char + ellipsis + ext
    int keep_stem = max_chars - 1 - ext_len;  // chars for stem before ellipsis
    if (keep_stem < 1) keep_stem = 1;
    // Build truncated string
    int pos = 0;
    for (int i = 0; i < keep_stem && i < len && pos < out_sz - 1; i++)
        out[pos++] = name[i];
    // UTF-8 ellipsis: 0xE2 0x80 0xA6
    if (pos + 3 < out_sz) { out[pos++]='\xe2'; out[pos++]='\x80'; out[pos++]='\xa6'; }
    if (dot && ext_len > 0 && pos + ext_len < out_sz) {
        strncpy(out + pos, dot, out_sz - pos - 1);
        pos += ext_len;
    }
    out[pos] = '\0';
}

std::string iv_home() {
#ifdef _WIN32
    char buf[MAX_PATH] = {};
    SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, buf);
    return buf[0] ? buf : "C:\\Users";
#else
    const char *h = getenv("HOME");
    return h ? h : "/home";
#endif
}

void iv_load_local(const char *filepath, const char *label) {
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        snprintf(g_iv.error, sizeof(g_iv.error), "Cannot open: %s", filepath);
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || sz > 64 * 1024 * 1024) {
        snprintf(g_iv.error, sizeof(g_iv.error), "File too large or empty");
        fclose(f);
        return;
    }
    unsigned char *buf = (unsigned char *)malloc(sz);
    if (!buf || fread(buf, 1, sz, f) != (size_t)sz) {
        snprintf(g_iv.error, sizeof(g_iv.error), buf ? "Cannot read: %s" : "Out of memory: %s", label);
        free(buf);
        fclose(f);
        return;
    }
    fclose(f);
    iv_load_image_from_mem(buf, (size_t)sz, label);
    free(buf);
}

// ============================================================================
// OPEN / CLOSE
// ============================================================================

void iv_open(bool remote, int /*win_w*/, int /*win_h*/) {
#ifdef HAVE_GSTREAMER
    // Initialize GStreamer and detect video capability
    static bool gst_initialized = false;
    if (!gst_initialized) {
        gst_init(nullptr, nullptr);
        gst_initialized = true;
        
        // Test if basic GStreamer elements are available
        GstElement *test = gst_element_factory_make("filesrc", nullptr);
        g_video_capable = (test != nullptr);
        if (test) gst_object_unref(test);
    }
#else
    g_video_capable = false;
#endif

    g_iv = ImageViewer{};
    g_iv.visible = true;
    g_iv.remote  = remote;

    if (!remote) {
        strncpy(g_iv.path, iv_home().c_str(), sizeof(g_iv.path)-1);
        iv_list_local(g_iv.path, g_iv.entries);
    } else {
        // Start at remote filesystem root — getenv("HOME") is the *local*
        // home directory and is wrong when connecting from Windows.
        // The user can navigate to their home via the listing.
        strncpy(g_iv.path, "/", sizeof(g_iv.path)-1);
        iv_list_remote(g_iv.path, g_iv.entries);
    }
}

// Start in a folder, or open a file at once (its folder listed, it
// selected): for a path given on the command line. Relative paths are from
// the current folder. Returns false (and says why in g_iv.error) when there's
// no such file or folder; the listing is then the home folder's.
bool iv_open_path(const char *path) {
    if (!path || !*path) return false;
    std::string full = path;
#ifdef _WIN32
    char buf[MAX_PATH];
    if (_fullpath(buf, path, sizeof buf)) full = buf;
    const char *seps = "\\/";
#else
    char *rp = realpath(path, nullptr);
    if (rp) { full = rp; free(rp); }
    const char *seps = "/";
#endif
    struct stat st;
    if (stat(full.c_str(), &st) != 0) {
        snprintf(g_iv.error, sizeof(g_iv.error), "No such file or folder: %s", path);
        return false;
    }
    std::string dir = full, file;
    if ((st.st_mode & S_IFMT) != S_IFDIR) {          // (MSVC has no S_ISDIR)
        size_t sl = full.find_last_of(seps);
        if (sl == std::string::npos) { dir = "."; file = full; }
        else { dir = full.substr(0, sl == 0 ? 1 : sl); file = full.substr(sl + 1); }
#ifdef _WIN32
        if (dir.size() == 2 && dir[1] == ':') dir += '\\';     // ("C:" is C:'s current folder)
#endif
    }
    strncpy(g_iv.path, dir.c_str(), sizeof(g_iv.path) - 1);
    g_iv.path[sizeof(g_iv.path) - 1] = '\0';
    g_iv.in_zip = false;
    iv_list_local(g_iv.path, g_iv.entries);
    g_iv.selected = 0;
    g_iv.scroll_top = 0;
    if (!file.empty()) {
        for (size_t i = 0; i < g_iv.entries.size(); i++)
            if (strcmp(g_iv.entries[i].name, file.c_str()) == 0) {
                g_iv.selected = (int)i;
                iv_enter_selected();
                break;
            }
    }
    return true;
}

void iv_close() {
    iv_stop_audio();
    iv_kfn_stop();
    iv_video_stop();
    iv_cdg_free();
    iv_free_tex();
    
    // Clean up text document - use swap to safely clear without double-free
    TextDocument empty_doc;
    s_text_doc = empty_doc;
    s_viewing_text = false;
    
    g_iv.visible = false;
}

// The most a picture is kept at, on its long side, in pixels: one larger is
// made smaller as soon as it's decoded (the texture is 4 bytes a pixel, and
// on DOS the memory a picture took is kept by Chirp until it exits; a
// 12-megapixel photo took over 150 MB). 2048 still zooms to 3x on a
// 640x480 screen with every pixel there. Elsewhere, the picture as it is.
#ifdef __DJGPP__
static const int IV_MAX_PICTURE_SIDE = 2048;
#else
static const int IV_MAX_PICTURE_SIDE = 0;
#endif

// Average `src` (sw x sh, `comp` bytes a pixel) down to dw x dh RGBA.
static unsigned char *iv_shrink_to_rgba(const unsigned char *src, int sw, int sh, int comp, int dw, int dh) {
    unsigned char *dst = (unsigned char *)malloc((size_t)dw * dh * 4);
    if (!dst) return nullptr;
    std::vector<uint32_t> acc((size_t)dw * 4);
    std::vector<int> x0(dw + 1);
    for (int x = 0; x <= dw; x++) x0[x] = (int)((long long)x * sw / dw);
    for (int y = 0; y < dh; y++) {
        int ya = (int)((long long)y * sh / dh), yb = (int)((long long)(y + 1) * sh / dh);
        if (yb <= ya) yb = ya + 1;
        std::fill(acc.begin(), acc.end(), 0);
        for (int sy = ya; sy < yb; sy++) {
            const unsigned char *row = src + (size_t)sy * sw * comp;
            for (int x = 0; x < dw; x++) {
                uint32_t *a = &acc[(size_t)x * 4];
                int xb = x0[x + 1] > x0[x] ? x0[x + 1] : x0[x] + 1;
                for (int sx = x0[x]; sx < xb; sx++) {
                    const unsigned char *p = row + (size_t)sx * comp;
                    if (comp >= 3) { a[0] += p[0]; a[1] += p[1]; a[2] += p[2]; a[3] += comp == 4 ? p[3] : 255; }
                    else           { a[0] += p[0]; a[1] += p[0]; a[2] += p[0]; a[3] += comp == 2 ? p[1] : 255; }
                }
            }
        }
        unsigned char *out = dst + (size_t)y * dw * 4;
        for (int x = 0; x < dw; x++) {
            int xb = x0[x + 1] > x0[x] ? x0[x + 1] : x0[x] + 1;
            uint32_t n = (uint32_t)(xb - x0[x]) * (uint32_t)(yb - ya);
            for (int k = 0; k < 4; k++) out[x * 4 + k] = (unsigned char)((acc[(size_t)x * 4 + k] + n / 2) / n);
        }
    }
    return dst;
}

void iv_load_image_from_mem(const unsigned char *data, size_t len, const char *label) {
    int w = 0, h = 0, ch = 0;
    iv_free_tex();                     // (the last picture's memory before this one's is taken)
    // As few bytes a pixel as the picture has (a JPEG: 3, not 4), when it
    // will be made smaller anyway: less to hold while it's decoded.
    int want = 4;
    if (IV_MAX_PICTURE_SIDE && stbi_info_from_memory(data, (int)len, &w, &h, &ch) &&
        (w > IV_MAX_PICTURE_SIDE || h > IV_MAX_PICTURE_SIDE))
        want = ch == 2 || ch == 4 ? 4 : 3;
    unsigned char *px = stbi_load_from_memory(data, (int)len, &w, &h, &ch, want);
    bool webp = false;

    // If stbi fails, try WebP decoder
    if (!px) {
        px = WebPDecodeRGBA(data, len, &w, &h);
        webp = px != nullptr;
        want = 4;
    }

    if (!px) {
        snprintf(g_iv.error, sizeof(g_iv.error), "Decode failed: %s", stbi_failure_reason());
        iv_free_tex();
        return;
    }
    auto free_px = [&](unsigned char *p) { if (webp) WebPFree(p); else stbi_image_free(p); };

    // Too big: made smaller (and RGBA) before anything else is allocated.
    if (IV_MAX_PICTURE_SIDE && (w > IV_MAX_PICTURE_SIDE || h > IV_MAX_PICTURE_SIDE)) {
        double sc = (double)IV_MAX_PICTURE_SIDE / (w > h ? w : h);
        int dw = (int)(w * sc + 0.5), dh = (int)(h * sc + 0.5);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        unsigned char *small = iv_shrink_to_rgba(px, w, h, want, dw, dh);
        free_px(px);
        if (!small) {
            snprintf(g_iv.error, sizeof(g_iv.error), "Out of memory: %s", label);
            iv_free_tex();
            return;
        }
        g_iv.error[0] = '\0';
        iv_upload_texture(small, dw, dh);
        free(small);
    } else {
        g_iv.error[0] = '\0';
        iv_upload_texture(px, w, h);
        // Freed by the decoder that made it. (This used to decode the picture a
        // second time to find out which that was, and never freed the second
        // one: every picture opened leaked a whole RGBA copy of itself.)
        free_px(px);
    }

    strncpy(g_iv.img_label, label, sizeof(g_iv.img_label)-1);
    // Reset view for every new image
    g_iv.zoom    = 1.0f;
    g_iv.pan_x   = 0.0f;
    g_iv.pan_y   = 0.0f;
    g_iv.img_rot = 0;
}

// ============================================================================
// DIRECTORY LISTING
// ============================================================================

#ifdef __DJGPP__
#include <dos.h>
#include <ctype.h>
// DOS: a drive's root ("C:\") lists the other drives, as folders named "D:",
// so the browser can change drives (iv_enter_selected goes to "D:\").
bool iv_is_drive_name(const char *n) {
    return n && isalpha((unsigned char)n[0]) && n[1] == ':' && !n[2];
}
static bool iv_is_drive_root(const char *p) {
    return isalpha((unsigned char)p[0]) && p[1] == ':' && (!p[2] || ((p[2] == '\\' || p[2] == '/') && !p[3]));
}
// The drives DOS has (by setting the current drive and reading it back,
// which touches no disk, so an empty floppy drive doesn't hold things up).
static void iv_list_drives(const char *path, std::vector<IVEntry> &out) {
    unsigned cur = 0, n = 0, got = 0;
    _dos_getdrive(&cur);
    for (unsigned d = 1; d <= 26; d++) {
        _dos_setdrive(d, &n);
        _dos_getdrive(&got);
        if (got != d || (int)toupper((unsigned char)path[0]) - 'A' + 1 == (int)d) continue;
        IVEntry e{};
        e.name[0] = (char)('A' + d - 1); e.name[1] = ':';
        e.is_dir = true;
        out.push_back(e);
    }
    _dos_setdrive(cur, &n);
}
#endif

void iv_list_local(const char *path, std::vector<IVEntry> &out) {
    out.clear();
#ifdef __DJGPP__
    if (iv_is_drive_root(path)) iv_list_drives(path, out);
    else
#endif
#ifndef _WIN32
    if (strcmp(path, "/") != 0) {
        IVEntry up{}; strncpy(up.name, "..", sizeof(up.name)-1); up.is_dir = true;
        out.push_back(up);
    }
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        IVEntry e{};
        strncpy(e.name, de->d_name, sizeof(e.name)-1);
        char full[4096]; snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
        struct stat st{};
        if (stat(full, &st) == 0) {
            e.is_dir = S_ISDIR(st.st_mode);
            e.size   = (uint64_t)st.st_size;
        }
        bool img   = is_image_ext(e.name);
        bool txt   = is_text_ext(e.name);
        bool md    = is_markdown_ext(e.name);
        bool zip   = is_zip_ext(e.name);
        bool audio = is_audio_ext(e.name);
        bool video = is_video_ext(e.name) && g_video_capable;
        bool cdg   = is_cdg_ext(e.name);
        bool kfn   = is_kfn_ext(e.name);
        if (e.is_dir || img || txt || md || zip || audio || video || cdg || kfn) {
            if (zip)   { e.is_zip = true; e.has_cdg_pair = zip_contains_cdg_pair(full); }
            if (audio) { e.is_audio = true; e.has_cdg_pair = has_paired_cdg(path, e.name); }
            if (video) { e.is_video = true; }
            if (cdg)   e.is_cdg = true;
            if (kfn)   e.is_kfn = true;
            out.push_back(e);
        }
    }
    closedir(d);
#else
    if (strcmp(path, "/") != 0 && strcmp(path, "\\") != 0) {
        IVEntry up{}; strncpy(up.name, "..", sizeof(up.name)-1); up.is_dir = true;
        out.push_back(up);
    }
    char pattern[4096]; snprintf(pattern, sizeof(pattern), "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        IVEntry e{};
        strncpy(e.name, fd.cFileName, sizeof(e.name)-1);
        e.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.size   = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        if (e.is_dir || is_image_ext(e.name) || is_zip_ext(e.name) ||
            is_audio_ext(e.name) || (is_video_ext(e.name) && g_video_capable) ||
            is_cdg_ext(e.name) || is_kfn_ext(e.name)) {
            if (is_zip_ext(e.name)) {
                e.is_zip = true;
                char full[4096]; snprintf(full, sizeof(full), "%s\\%s", path, e.name);
                e.has_cdg_pair = zip_contains_cdg_pair(full);
            }
            if (is_audio_ext(e.name)) { e.is_audio = true;
                e.has_cdg_pair = has_paired_cdg(path, e.name); }
            if (is_video_ext(e.name) && g_video_capable) { e.is_video = true; }
            if (is_cdg_ext(e.name))   e.is_cdg = true;
            if (is_kfn_ext(e.name))   e.is_kfn = true;
            out.push_back(e);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#endif
    std::stable_sort(out.begin(), out.end(), [](const IVEntry &a, const IVEntry &b){
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
#ifdef __DJGPP__
        // DOS: the drives (A: B: ...) first, after ".."
        bool ad = iv_is_drive_name(a.name), bd = iv_is_drive_name(b.name);
        if (ad != bd) return strcmp(a.name, "..") != 0 && (ad || strcmp(b.name, "..") == 0);
#endif
        return strcmp(a.name, b.name) < 0;
    });
}

// Reuse the SFTP subsystem from sftp_overlay (s_sftp is extern there).
// We call the public sftp_panel helpers indirectly by duplicating the
// list logic here so we don't need to expose s_sftp directly.
void iv_list_remote(const char *path, std::vector<IVEntry> &out) {
    out.clear();
    // Access the SFTP handle via the public SSH session API
    LIBSSH2_SESSION *sess = ssh_get_session();
    if (!sess) return;

    // We need the sftp handle. sftp_overlay.cpp owns it but doesn't expose it.
    // Use SftpPanel as a vehicle: create a temporary panel, call sftp_panel_refresh.
    // Instead, we open our own SFTP channel for listing (read-only, quick).
    // Note: sftp_init() in sftp_overlay already initialised the subsystem;
    // we open a second handle here for the directory listing to avoid
    // touching sftp_overlay's internal state.
    ssh_session_lock();
    libssh2_session_set_blocking(sess, 1);
    LIBSSH2_SFTP *sftp = libssh2_sftp_init(sess);
    if (!sftp) { libssh2_session_set_blocking(sess, 0); ssh_session_unlock(); return; }

    if (strcmp(path, "/") != 0) {
        IVEntry up{}; strncpy(up.name, "..", sizeof(up.name)-1); up.is_dir = true;
        out.push_back(up);
    }

    LIBSSH2_SFTP_HANDLE *dh = libssh2_sftp_opendir(sftp, path);
    if (dh) {
        char name[512], longentry[1024];
        LIBSSH2_SFTP_ATTRIBUTES attrs{};
        while (libssh2_sftp_readdir_ex(dh, name, sizeof(name), longentry, sizeof(longentry), &attrs) > 0) {
            if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
            bool is_dir = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) &&
                          LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
            bool img   = is_image_ext(name);
            bool txt   = is_text_ext(name);
            bool md    = is_markdown_ext(name);
            bool audio = is_audio_ext(name);
            bool zip   = is_zip_ext(name);
            bool video = is_video_ext(name) && g_video_capable;
            bool cdg   = is_cdg_ext(name);
            bool kfn   = is_kfn_ext(name);
            if (!is_dir && !img && !txt && !md && !audio && !zip && !video && !cdg && !kfn) continue;
            IVEntry e{};
            strncpy(e.name, name, sizeof(e.name)-1);
            e.is_dir = is_dir;
            e.size   = (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) ? attrs.filesize : 0;
            if (audio) e.is_audio = true;
            if (zip)   e.is_zip   = true;
            if (video) e.is_video = true;
            if (cdg)   e.is_cdg   = true;
            if (kfn)   e.is_kfn   = true;
            // Note: has_cdg_pair and zip CDG peek not done for remote listings
            // (would require extra SFTP round-trips per file)
            out.push_back(e);
        }
        libssh2_sftp_closedir(dh);
    }
    libssh2_sftp_shutdown(sftp);
    libssh2_session_set_blocking(sess, 0);
    ssh_session_unlock();

    std::stable_sort(out.begin(), out.end(), [](const IVEntry &a, const IVEntry &b){
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
        return strcmp(a.name, b.name) < 0;
    });
}
