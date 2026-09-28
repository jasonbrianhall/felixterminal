// Felix Terminal Main program (Last updated September 26, 2026)
// Written by Jason Brian Hall (jasonbrianhall@gmail.com)

#include "gl_terminal.h"
#include "gl_renderer.h"
#include "sdl_renderer.h"
#include "ft_font.h"
#include "term_color.h"
#include "terminal.h"
#include "term_pty.h"
#include "term_ui.h"
#include "term_clipboard.h"
#include "sticky_prompt.h"
#include "ssh_key_manager.h"
#include "gl_bouncingcircle.h"
#include "crt_audio.h"
#include "felix_settings.h"
#include "kitty_graphics.h"
#include "sixel_graphics.h"
#include "basic_graphics.h"
#include "font_manager.h"
#include "felixchirp/felixchirp.h"
#include "wopr/wopr.h"
#include "ssh_session.h"
#include "sftp_overlay.h"
#include "sftp_console.h"
#include "port_forward.h"
#include "pf_overlay.h"
#include "sftp_webserver.h"
#include "telnet_session.h"
#include "serial_session.h"

#include <SDL2/SDL.h>
#include "icon.h"
#include "crash_handler.h"
#ifndef _WIN32
#include <sys/wait.h>
#endif

#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>

// ============================================================================
// GLOBALS referenced across modules
// ============================================================================

int         g_font_size   = FONT_SIZE_DEFAULT;
float       g_opacity     = 1.0f;
bool        g_blink_text_on = true;
bool        g_autoscroll_enabled = true;  // true = autoscroll ON, false = autoscroll OFF
bool        g_line_numbers_enabled = false;  // Toggle for line numbering in scrollback
bool        g_menu_copy_pending = false;  // Flag to prevent MOUSEUP from overwriting menu copy
SDL_Window *g_sdl_window  = nullptr;
std::vector<FontEntry> g_font_list;
// Current window size exposed to terminal.cpp for basic_handle_osc coordinate mapping.
int         g_basic_win_w = 800;
int         g_basic_win_h = 480;
const char *g_term_type   = "xterm-256color";  // overridable via --term

// ============================================================================
// SSH / PTY / Telnet dispatch helpers — used throughout the main loop.
//
// TERM_WRITE always goes through term_write(), which internally checks
// g_term_write_override (set by ssh_connect / telnet_connect) to route to
// the active transport.  handle_key(), term_paste() etc. work transparently.
//
// TERM_READ must explicitly pick the right source since SSH, Telnet, and the
// local PTY all pull from different file descriptors / libraries.
// ============================================================================

#ifdef USESSH
static inline bool _term_read_dispatch(bool ssh, bool tnet, bool ser, Terminal *t) {
    if (ssh)  return ssh_read(t);
    if (tnet) return telnet_read(t);
    if (ser)  return serial_read(t);
    return term_read(t);
}
#  define TERM_READ()  _term_read_dispatch(use_ssh, use_telnet, use_serial, &term)
#else
#  define TERM_READ()  (use_serial ? serial_read(&term) : use_telnet ? telnet_read(&term) : term_read(&term))
#endif
#define TERM_WRITE(buf, n)  term_write(&term, (buf), (n))

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char **argv) {
    install_crash_handler();  // writes crash_log.txt next to the exe on an unhandled SEH exception

    // ---- Command-line parsing ----
#ifdef WIN32
    const char *shell = "cmd.exe";
#else
    const char *shell = "/bin/bash";
#endif

#ifdef USESSH
    SshConfig ssh_cfg;
    struct PfPending { int lp; std::string rh; int rp; };
    std::vector<PfPending> pf_locals_pending, pf_remotes_pending;
    std::vector<int> pf_socks_pending;
#endif
    bool use_ssh    = false;
    bool use_telnet = false;
    bool use_serial = false;
//    bool force_sdl  = false;
    TelnetConfig telnet_cfg;
    SerialConfig serial_cfg;

    bool        start_webserver = false;
    int         webserver_port = 53716;
    std::string webserver_bind_addr = "127.0.0.1";
    std::string webserver_root = "/";  // Default to root filesystem

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        // --telnet [host[:port]]  — port defaults to 23 if omitted
        if (strcmp(arg, "--telnet") == 0 || strcmp(arg, "-telnet") == 0) {
            use_telnet = true;
            if (i + 1 < argc && argv[i+1][0] != '-') {
                const char *target = argv[++i];
                const char *colon  = strrchr(target, ':');
                if (colon) {
                    telnet_cfg.host = std::string(target, colon - target);
                    telnet_cfg.port = atoi(colon + 1);
                    if (telnet_cfg.port <= 0 || telnet_cfg.port > 65535)
                        telnet_cfg.port = 23;
                } else {
                    telnet_cfg.host = target;
                }
            }
            continue;
        }
        if ((strcmp(arg, "--telnet-port") == 0 || strcmp(arg, "-telnet-port") == 0) && i + 1 < argc) {
            telnet_cfg.port = atoi(argv[++i]);
            continue;
        }
        if ((strcmp(arg, "--telnet-ttype") == 0 || strcmp(arg, "-telnet-ttype") == 0) && i + 1 < argc) {
            telnet_cfg.ttype = argv[++i];
            continue;
        }
        if (strcmp(arg, "--raw") == 0 || strcmp(arg, "-raw") == 0) {
            telnet_cfg.raw_mode = true;
            continue;
        }
        if (strcmp(arg, "--ssl") == 0 || strcmp(arg, "-ssl") == 0) {
            telnet_cfg.use_ssl = true;
            continue;
        }

        // --term <type>  — TERM env var for the spawned shell (default: xterm-256color)
        if ((strcmp(arg, "--term") == 0 || strcmp(arg, "-term") == 0) && i + 1 < argc) {
            g_term_type = argv[++i];
            continue;
        }

        // --serial [port]  — port defaults to empty (prompted in-window)
        if (strcmp(arg, "--serial") == 0 || strcmp(arg, "-serial") == 0) {
            use_serial = true;
            if (i + 1 < argc && argv[i+1][0] != '-')
                serial_cfg.port = argv[++i];
            continue;
        }
        if ((strcmp(arg, "--serial-baud") == 0 || strcmp(arg, "-serial-baud") == 0) && i + 1 < argc) {
            serial_cfg.baud = atoi(argv[++i]);
            continue;
        }

#ifdef USESSH
        // --ssh [user@host[:port]]  — argument is optional; missing parts are
        // prompted inside the GL window.
        if ((strcmp(arg, "--ssh") == 0 || strcmp(arg, "-ssh") == 0)) {
            use_ssh = true;
            // NOTE: previously disabled kitty graphics unconditionally for
            // all SSH sessions here, to guard against tmux/multiplexers on
            // the remote end sending malformed APC. That threw out kitty
            // graphics for every SSH session, tmux or not. Left enabled by
            // default now; if remote tmux APC passthrough turns out to
            // actually crash, disable it specifically when the remote shell
            // is inside a multiplexer, not for SSH as a whole.
            if (i + 1 < argc && argv[i+1][0] != '-') {
                const char *target = argv[++i];
                // [user@]host[:port] — user optional (prompted if missing).
                // IPv6 literals: [addr] or [addr]:port; a bare address with
                // several colons (e.g. ::1) is taken as a host with no port.
                const char *at = strrchr(target, '@');
                const char *host_start = target;
                if (at) {
                    ssh_cfg.user = std::string(target, at - target);
                    host_start = at + 1;
                }
                std::string hostpart = host_start;
                std::string port_str;
                if (!hostpart.empty() && hostpart[0] == '[') {
                    size_t close = hostpart.find(']');
                    if (close != std::string::npos) {
                        if (close + 1 < hostpart.size() && hostpart[close + 1] == ':')
                            port_str = hostpart.substr(close + 2);
                        hostpart = hostpart.substr(1, close - 1);
                    }
                } else if (hostpart.find(':') != std::string::npos &&
                           hostpart.find(':') == hostpart.rfind(':')) {
                    size_t colon = hostpart.find(':');
                    port_str = hostpart.substr(colon + 1);
                    hostpart = hostpart.substr(0, colon);
                }
                ssh_cfg.host = hostpart;
                if (!port_str.empty()) {
                    ssh_cfg.port = atoi(port_str.c_str());
                    if (ssh_cfg.port <= 0 || ssh_cfg.port > 65535) ssh_cfg.port = 22;
                }
            }
            // Missing user/host will be prompted in the GL window
            continue;
        }
        if ((strcmp(arg, "--ssh-key") == 0 || strcmp(arg, "-ssh-key") == 0 || strcmp(arg, "-i") == 0) && i + 1 < argc) {
            ssh_cfg.key_path = argv[++i];
            continue;
        }
        if ((strcmp(arg, "--ssh-key-pub") == 0 || strcmp(arg, "-ssh-key-pub") == 0) && i + 1 < argc) {
            ssh_cfg.key_path_pub = argv[++i];
            continue;
        }
        if ((strcmp(arg, "--ssh-password") == 0 || strcmp(arg, "-ssh-password") == 0) && i + 1 < argc) {
            ssh_cfg.password = argv[++i];
            continue;
        }
        if ((strcmp(arg, "--ssh-known-hosts") == 0 || strcmp(arg, "-ssh-known-hosts") == 0) && i + 1 < argc) {
            ssh_cfg.known_hosts_path = argv[++i];
            continue;
        }
        if (strcmp(arg, "--no-x11") == 0 || strcmp(arg, "-no-x11") == 0) {
            ssh_cfg.x11_forward = false;
            continue;
        }
        if ((strcmp(arg, "-L") == 0 || strcmp(arg, "--forward-local") == 0) && i + 1 < argc) {
            int lp, rp; std::string rh;
            if (pf_parse_spec(argv[++i], &lp, &rh, &rp))
                pf_locals_pending.push_back({lp, rh, rp});
            else
                SDL_Log("Bad -L spec (expected local_port:remote_host:remote_port): %s\n", argv[i]);
            continue;
        }
        if ((strcmp(arg, "-R") == 0 || strcmp(arg, "--forward-remote") == 0) && i + 1 < argc) {
            int lp, rp; std::string rh;
            if (pf_parse_spec(argv[++i], &lp, &rh, &rp)) {
                SDL_Log("[DEBUG] -R spec parsed: lp=%d rh=%s rp=%d\n", lp, rh.c_str(), rp);
                pf_remotes_pending.push_back({lp, rh, rp});
            }
            else
                SDL_Log("Bad -R spec (expected remote_port:local_host:local_port): %s\n", argv[i]);
            continue;
        }
        if ((strcmp(arg, "-D") == 0 || strcmp(arg, "--dynamic") == 0) && i + 1 < argc) {
            int port = atoi(argv[++i]);
            if (port > 0 && port <= 65535)
                pf_socks_pending.push_back(port);
            else
                SDL_Log("Bad -D port (expected 1-65535): %s\n", argv[i]);
            continue;
        }
        if ((strcmp(arg, "-c") == 0 || strcmp(arg, "--ssh-command") == 0) && i + 1 < argc) {
            ssh_cfg.command = argv[++i];
            continue;
        }

        // --web-server [addr:port]  — starts web server at boot
        // Examples:
        //   --web-server                    (use defaults: 127.0.0.1:53716)
        //   --web-server 127.0.0.1:8080     (specific address and port)
        //   --web-server 0.0.0.0:8080       (listen on all interfaces)
        //   --web-server [::1]:8080         (IPv6 localhost)
        if ((strcmp(arg, "--web-server") == 0 || strcmp(arg, "--webserver") == 0)) {
            start_webserver = true;
            if (i + 1 < argc && argv[i+1][0] != '-') {
                const char *spec = argv[++i];
                const char *colon = strrchr(spec, ':');
                
                if (colon) {
                    // addr:port format
                    webserver_bind_addr = std::string(spec, colon - spec);
                    webserver_port = atoi(colon + 1);
                    if (webserver_port <= 0 || webserver_port > 65535) {
                        SDL_Log("Bad web server port (1-65535): %d\n", webserver_port);
                        webserver_port = 53716;
                    }
                } else {
                    // Just address or port? Try to parse as port, fall back to address
                    int port_attempt = atoi(spec);
                    if (port_attempt > 0 && port_attempt <= 65535) {
                        webserver_port = port_attempt;
                        webserver_bind_addr = "127.0.0.1";
                    } else {
                        // Treat as address
                        webserver_bind_addr = spec;
                        webserver_port = 53716;
                    }
                }
            }
            continue;
        }

        // --web-root <dir>  — root directory for local file server mode
        if ((strcmp(arg, "--web-root") == 0) && i + 1 < argc) {
            webserver_root = argv[++i];
            continue;
        }
#endif // USESSH

        // NOTE: SDL renderer path is disabled — OpenGL 3.3+ is required.
        // The --sdl flag is no longer supported.
/*        if (strcmp(arg, "--sdl") == 0 || strcmp(arg, "-sdl") == 0) {
            force_sdl = true;
            continue;
        }*/

        if (arg[0] != '-' && !use_ssh) {
            shell = arg;
            continue;
        }

        // --help / -h
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            SDL_Log("Usage: flt [shell]\n");
            SDL_Log("       flt [options]\n\n");
            SDL_Log("Options:\n");
            SDL_Log("  [shell]                     Command to run instead of default shell\n");
            SDL_Log("\nTelnet options:\n");
            SDL_Log("  --telnet [host[:port]]      Connect via Telnet (port defaults to 23)\n");
            SDL_Log("  --telnet-port <port>        Override Telnet port\n");
            SDL_Log("  --telnet-ttype <type>       Terminal type (default: xterm-256color)\n");
            SDL_Log("  --raw                       Enable raw mode\n");
            SDL_Log("  --ssl                       Use SSL/TLS connection\n");
            SDL_Log("\nSerial options:\n");
            SDL_Log("  --serial [port]             Connect via serial port (prompts if omitted)\n");
            SDL_Log("  --serial-baud <baud>        Serial baud rate\n");
            SDL_Log("\nGeneral options:\n");
            SDL_Log("  --term <type>               TERM env var for local shell (default: xterm-256color)\n");
#ifdef USESSH
            SDL_Log("\nSSH options:\n");
            SDL_Log("  --ssh [user@host[:port]]    Connect via SSH (prompts for missing fields)\n");
            SDL_Log("  -i <path>                   Private key file (alias: --ssh-key)\n");
            SDL_Log("  --ssh-key <path>            Private key file for public key auth\n");
            SDL_Log("  --ssh-key-pub <path>        Public key file (derived from key path if omitted)\n");
            SDL_Log("  --ssh-password <pass>       Password auth (prefer agent or key)\n");
            SDL_Log("  --ssh-known-hosts <path>    Known hosts file (default: ~/.ssh/known_hosts)\n");
            SDL_Log("  --no-x11                    Disable X11 forwarding\n");
            SDL_Log("  -c <command>                Execute remote command (alias: --ssh-command)\n");
            SDL_Log("  -L local_port:remote_host:remote_port   Local port forward\n");
            SDL_Log("  -R remote_port:local_host:local_port    Remote port forward\n");
            SDL_Log("  -D local_port                           SOCKS5 dynamic port forward\n");
            SDL_Log("\nWeb Server Options:\n");
            SDL_Log("  --webserver [listenaddress:port]        Listen address and port are optional but if listen address, port must be specified\n");
            SDL_Log("  --web-root <directory>                  Root directory to serve (local mode only, default is /)\n");
#endif
            SDL_Log("\nKeyboard shortcuts:\n");
            SDL_Log("  F2                          SFTP upload browser (SSH sessions only)\n");
            SDL_Log("  F3                          SFTP download browser (SSH sessions only)\n");
            SDL_Log("  F4                          SFTP interactive console (SSH sessions only)\n");
            SDL_Log("  F5                          Felix Chirp — image viewer / audio player\n");
            SDL_Log("  F6                          Port forward manager (SSH sessions only)\n");
            SDL_Log("  F7                          WOPR Terminal\n");
            SDL_Log("  F8                          SSH Key Manager (additional function when in SSH mode)\n");
            SDL_Log("  F9                          Web Server (serves on http://localhost:53716 by default)\n");
            SDL_Log("                                  Works locally and with SSH supporting SFTP\n");
            SDL_Log("  F11                         Toggle full screen\n");
            SDL_Log("  Ctrl+Scroll                 Resize font\n");
            SDL_Log("  Shift+PageUp/Down           Scroll scrollback buffer\n");
            SDL_Log("  Ctrl+Click                  Open URL in browser\n");
            SDL_Log("\nTroubleshooting:\n");
            SDL_Log("  OpenGL 3.3+ is required. If the terminal fails to start:\n");
            SDL_Log("  (common on older systems or Raspberry Pi with Mesa/VC4)\n");
            SDL_Log("  Try setting environment variables:\n");
            SDL_Log("    MESA_GL_VERSION_OVERRIDE=3.3 MESA_GLSL_VERSION_OVERRIDE=330 flt\n");
            SDL_Log("  Or add these to your ~/.profile to make it permanent:\n");
            SDL_Log("    export MESA_GL_VERSION_OVERRIDE=3.3\n");
            SDL_Log("    export MESA_GLSL_VERSION_OVERRIDE=330\n");
            SDL_Log("  If OpenGL 3.3 is still unavailable, your hardware/driver\n");
            SDL_Log("  does not support the required graphics capabilities.\n");
            
            
            return 0;
        }
    }

    // Disable X11 input method composition (IBus/fcitx intercept Ctrl+Shift+U
    // for Unicode entry before SDL sees it with correct mod state).
    // A terminal handles its own input — we don't want IME interference.
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "0");
    SDL_SetHint("SDL_IM_MODULE", "");       // empty = disable input method module

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
    SDL_LogSetAllPriority(SDL_LOG_PRIORITY_VERBOSE);
    debuglog_init();

    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    int win_w = 800, win_h = 480;
    SDL_Window *window = SDL_CreateWindow(
        WIN_TITLE,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    g_sdl_window = window;
    
    // Enforce minimum window size: 20 character width + 4px padding
    // Assuming ~8px average char width at default font size
    SDL_SetWindowMinimumSize(window, 160 + 4, 60);

    // Set embedded window icon
    SDL_Surface *icon_surf = SDL_CreateRGBSurfaceFrom(
        (void*)icon_pixels, icon_w, icon_h, 32, icon_w * 4,
        0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
    if (icon_surf) {
        SDL_SetWindowIcon(window, icon_surf);
        SDL_FreeSurface(icon_surf);
    }

    SDL_GLContext ctx = nullptr;
    // OpenGL 3.3+ is now required — SDL renderer fallback is disabled
    ctx = SDL_GL_CreateContext(window);
    if (!ctx) {
        SDL_Log("INFO: GL 3.3 core not available (%s), retrying with Compatibility profile\n",
                SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
        ctx = SDL_GL_CreateContext(window);
    }
    if (!ctx) {
        // OpenGL 3.3+ is required — show error dialog and exit
        char error_msg[512];
        snprintf(error_msg, sizeof(error_msg),
            "OpenGL 3.3 or higher is required to run Felix Terminal.\n\n"
            "Your graphics card or driver does not support OpenGL 3.3.\n"
            "Please ensure you have:\n"
            "- A compatible GPU (most modern GPUs support OpenGL 3.3+)\n"
            "- Updated graphics drivers\n\nSometimes this works in Linux --> MESA_GL_VERSION_OVERRIDE=3.3 MESA_GLSL_VERSION_OVERRIDE=330 flt\n\n"
            "Error: %s", SDL_GetError());
        SDL_ShowSimpleMessageBox(
            SDL_MESSAGEBOX_ERROR,
            "OpenGL 3.3+ Required",
            error_msg,
            window);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    
    SDL_GL_MakeCurrent(window, ctx);
    SDL_GL_SetSwapInterval(1);

    apply_theme(0);
    ft_init();
    crt_audio_init();
    wopr_audio_init();

    Terminal term;
    term_init(&term);

    settings_load();  // applies font size, theme, sound — after ft_init and term_init
    SDL_Log("[DEBUG] settings_load done\n");

    // Initialize available terminals for menu
    detect_available_terminals();
    SDL_Log("[DEBUG] Detected %d terminal options\n", (int)g_available_terminals.size());

    // Scan system fonts and restore saved font choice
    g_font_list = font_scan();
    {
        std::string saved = font_load_config();
        if (!saved.empty()) {
            for (const auto &fe : g_font_list) {
                if (fe.display_name == saved) {
                    font_apply(fe, g_font_list, nullptr, 0, 0);
                    break;
                }
            }
        }
    }

    // Update cell dimensions based on final font size before calculating window size
    term_update_cell_dims(&term);

    win_w = (int)(term.cell_w * term.cols) + 4;
    win_h = (int)(term.cell_h * term.rows) + 4;
    SDL_SetWindowSize(window, win_w, win_h);
    SDL_GetWindowSize(window, &win_w, &win_h);

    gl_init_renderer(win_w, win_h);
    kitty_init();
    sixel_init();
    g_basic_win_w = win_w;
    g_basic_win_h = win_h;
    basic_graphics_init(win_w, win_h);
    if (!g_use_sdl_renderer) glViewport(0, 0, win_w, win_h);

    term_resize(&term, win_w, win_h);

    // ---- SSH: async connection state ----------------------------------------
    // ssh_connect() blocks on network I/O (DNS, TCP, SSH handshake, auth).
    // We run it on a background thread so the GL window stays alive and
    // responsive.  The password prompt is handled via a shared request/response
    // struct: the background thread posts a prompt request, the main loop
    // renders it, collects the password, and posts the response back.
#ifdef USESSH
    enum class SshPhase { IDLE, SETUP, CONNECTING, PROMPTING, ACTIVE, FAILED };
    // Start in SETUP if any required fields are missing, otherwise go straight to CONNECTING
    SshPhase ssh_phase = use_ssh
        ? (ssh_cfg.host.empty() || ssh_cfg.user.empty()
               ? SshPhase::SETUP : SshPhase::CONNECTING)
        : SshPhase::IDLE;

    struct PromptReq {
        std::string prompt;
        std::string response;
        bool        pending  = false;
        bool        answered = false;
        bool        secret   = false;  // true = echo *, false = echo chars
        SDL_mutex  *mtx      = nullptr;
    } prompt_req;

    std::thread   ssh_thread;
    std::string   ssh_field_input;  // text being typed for any in-window prompt
    bool          ssh_conn_ok = false;
    std::atomic<bool> ssh_thread_done{false};
    std::atomic<bool> ssh_abort{false};  // set on quit to unblock prompt callback
    bool          ssh_setup_deferred_forwards = false;  // apply -R/-D after session stabilizes
    uint32_t      ssh_setup_forward_delay = 0;  // frames to wait before applying -R/-D

    // Helper: post a prompt request from the main thread to itself (SETUP phase).
    // SETUP prompts are driven directly by the main loop — no background thread yet.
    // We reuse prompt_req so the event handler can feed keystrokes into ssh_field_input.
    auto ssh_begin_prompt = [&](const char *text, bool secret) {
        term_feed(&term, text, (int)strlen(text));
        ssh_field_input.clear();
        prompt_req.secret   = secret;
        prompt_req.pending  = true;
        prompt_req.answered = false;
    };

    if (use_ssh) {
        prompt_req.mtx = SDL_CreateMutex();
        SDL_StartTextInput();

        if (ssh_phase == SshPhase::SETUP) {
            term_feed(&term, "SSH connection setup\r\n", 21);
            if (ssh_cfg.host.empty())
                ssh_begin_prompt("\r\nHost: ", false);
            else if (ssh_cfg.user.empty()) {
                char p[128];
                snprintf(p, sizeof(p), "User (%s): ", ssh_cfg.host.c_str());
                ssh_begin_prompt(p, false);
            }
        } else {
            char connecting_msg[256];
            snprintf(connecting_msg, sizeof(connecting_msg),
                     "Connecting to %s@%s:%d …\r\n",
                     ssh_cfg.user.c_str(), ssh_cfg.host.c_str(), ssh_cfg.port);
            term_feed(&term, connecting_msg, (int)strlen(connecting_msg));

            ssh_cfg.prompt_password = [&](const char *prompt_str) -> std::string {
                SDL_LockMutex(prompt_req.mtx);
                prompt_req.prompt   = prompt_str;
                prompt_req.response.clear();
                prompt_req.answered = false;
                prompt_req.pending  = true;
                prompt_req.secret   = true;
                SDL_UnlockMutex(prompt_req.mtx);
                while (true) {
                    if (ssh_abort.load()) return std::string{};
                    SDL_LockMutex(prompt_req.mtx);
                    bool done = prompt_req.answered;
                    std::string resp = prompt_req.response;
                    SDL_UnlockMutex(prompt_req.mtx);
                    if (done) return resp;
                    SDL_Delay(10);
                }
            };

            ssh_cfg.prompt_host_key = [&](const char *prompt_str) -> std::string {
                SDL_LockMutex(prompt_req.mtx);
                prompt_req.prompt   = prompt_str;
                prompt_req.response.clear();
                prompt_req.answered = false;
                prompt_req.pending  = true;
                prompt_req.secret   = false;  // echo "yes"/"no" as typed
                SDL_UnlockMutex(prompt_req.mtx);
                while (true) {
                    if (ssh_abort.load()) return std::string{};
                    SDL_LockMutex(prompt_req.mtx);
                    bool done = prompt_req.answered;
                    std::string resp = prompt_req.response;
                    SDL_UnlockMutex(prompt_req.mtx);
                    if (done) return resp;
                    SDL_Delay(10);
                }
            };

            ssh_thread = std::thread([&]() {
                ssh_conn_ok = ssh_connect(ssh_cfg, &term);
                ssh_thread_done.store(true);
            });
        }
    }
#endif
    // ---- Telnet: in-window host/port prompt then connect ----------------------
    // Mirrors the SSH SETUP phase: if host was not supplied on the command line,
    // we collect it interactively in the GL window before connecting.
    enum class TelnetPhase { IDLE, SETUP, CONNECTING, ACTIVE, FAILED };
    TelnetPhase telnet_phase = use_telnet
        ? (telnet_cfg.host.empty() ? TelnetPhase::SETUP : TelnetPhase::CONNECTING)
        : TelnetPhase::IDLE;
    uint32_t telnet_active_since = 0;  // SDL_GetTicks() when we entered ACTIVE

    std::string telnet_field_input;
    bool        telnet_collecting_port = false;  // false=host, true=port
    bool        telnet_collecting_ssl  = false;  // true=asking SSL y/n

    auto telnet_begin_prompt = [&](const char *text) {
        term_feed(&term, text, (int)strlen(text));
        telnet_field_input.clear();
    };

    if (use_telnet) {
        SDL_StartTextInput();
        if (telnet_phase == TelnetPhase::SETUP) {
            term_feed(&term, "Telnet connection setup\r\n", 24);
            telnet_begin_prompt("\r\nHost: ");
            telnet_collecting_port = false;
        } else {
            // Host given on CLI — connect immediately (blocking; fast for TCP)
            char msg[256];
            bool will_be_ssl = telnet_cfg.use_ssl || telnet_cfg.port == 443;
            bool will_be_raw = !will_be_ssl && (telnet_cfg.raw_mode || telnet_cfg.port != 23);
            const char *mode_str = will_be_ssl ? "SSL" : will_be_raw ? "raw TCP" : "telnet";
            snprintf(msg, sizeof(msg), "Connecting to %s:%d (%s) …\r\n",
                     telnet_cfg.host.c_str(), telnet_cfg.port, mode_str);
            term_feed(&term, msg, (int)strlen(msg));
            if (telnet_connect(telnet_cfg, &term)) {
                telnet_phase = TelnetPhase::ACTIVE;
                telnet_active_since = SDL_GetTicks();
            } else {
                telnet_phase = TelnetPhase::FAILED;
                const char *fail = "\r\nConnection failed. Press any key to close.\r\n";
                term_feed(&term, fail, (int)strlen(fail));
            }
        }
    }

    // ---- Serial: in-window port/baud/settings prompt then connect -------------
    // Prompts for port, baud rate, data bits, parity, stop bits, flow control.
    // If port was supplied on the command line, skips straight to baud prompt.
    enum class SerialPhase { IDLE, SETUP, ACTIVE, FAILED };
    SerialPhase serial_phase = use_serial
        ? SerialPhase::SETUP
        : SerialPhase::IDLE;

    // Which field are we currently collecting?
    enum class SerialField { PORT, BAUD, DATA_BITS, PARITY, STOP_BITS, FLOW };
    SerialField serial_field = serial_cfg.port.empty()
        ? SerialField::PORT : SerialField::BAUD;
    std::string serial_field_input;

    auto serial_begin_prompt = [&](const char *text) {
        term_feed(&term, text, (int)strlen(text));
        serial_field_input.clear();
    };

    if (use_serial) {
        SDL_StartTextInput();
        term_feed(&term, "Serial connection setup\r\n", 24);
        if (serial_field == SerialField::PORT) {
#ifdef _WIN32
            serial_begin_prompt("\r\nPort (e.g. COM1, COM3): ");
#else
            serial_begin_prompt("\r\nPort (e.g. /dev/ttyUSB0, /dev/ttyS0): ");
#endif
        } else {
            char p[64];
            snprintf(p, sizeof(p), "\r\nBaud rate [%d]: ", serial_cfg.baud);
            serial_begin_prompt(p);
        }
    }

    // ---- local shell (non-SSH, non-Telnet, non-Serial) -----------------------
#ifdef USESSH
    if (!use_ssh)
#endif
    if (!use_telnet && !use_serial)
    {
        if (!term_spawn(&term, shell)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Failed to spawn shell: %s\r\n"
                     "Press any key to quit.\r\n", shell);
            term_feed(&term, msg, (int)strlen(msg));
        }

        // Start web server if requested (local mode)
        if (start_webserver) {
            if (!sftp_webserver_start_local(webserver_root.c_str(), webserver_bind_addr.c_str(), webserver_port)) {
                SDL_Log("[WEBSERVER] Failed to start local web server on %s:%d\n",
                        webserver_bind_addr.c_str(), webserver_port);
            } else {
                SDL_Log("[WEBSERVER] Started local web server on %s:%d\n",
                        webserver_bind_addr.c_str(), webserver_port);
            }
        }
    }

#ifdef _WIN32
    SDL_Delay(300);
    TERM_READ();
#else
#  ifdef USESSH
    if (!use_ssh)
#  endif
    if (!use_telnet && !use_serial)
    {
        SDL_Delay(200);
        term_read(&term);
        for (int i = 0; i < term.rows * term.cols; i++)
            term.cells[i] = {' ', TCOLOR_PALETTE(7), TCOLOR_PALETTE(0), 0, {0,0,0}};
        term.cur_row = term.cur_col = 0;
        term.scroll_top = 0; term.scroll_bot = term.rows - 1;
        term.state = PS_NORMAL;
        term_write(&term, "\n", 1);
        SDL_Delay(100);
        term_read(&term);
    }
#endif

    SDL_Cursor *cursor_ibeam = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_IBEAM);
    SDL_Cursor *cursor_hand  = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
    SDL_SetCursor(cursor_ibeam);

#ifdef USESSH
    // Get the remote cwd by running 'pwd' on a temporary exec channel.
    // Falls back to "/" on any failure.
    auto guess_remote_cwd = [&]() -> std::string {
        LIBSSH2_SESSION *sess = ssh_get_session();
        if (!sess) return "/";

        libssh2_session_set_blocking(sess, 1);

        LIBSSH2_CHANNEL *ch = nullptr;
        while (!(ch = libssh2_channel_open_session(sess))) {
            if (libssh2_session_last_errno(sess) != LIBSSH2_ERROR_EAGAIN) break;
            SDL_Delay(5);
        }
        if (!ch) {
            libssh2_session_set_blocking(sess, 0);
            SDL_Log("[SFTP] guess_remote_cwd: could not open channel, using /\n");
            return "/";
        }

        int rc;
        while ((rc = libssh2_channel_exec(ch, "pwd")) == LIBSSH2_ERROR_EAGAIN)
            SDL_Delay(5);

        std::string result;
        if (rc == 0) {
            char buf[4096] = {};
            ssize_t n = libssh2_channel_read(ch, buf, sizeof(buf) - 1);
            if (n > 0) {
                result = buf;
                // Strip trailing newline/whitespace
                while (!result.empty() &&
                       (result.back() == '\n' || result.back() == '\r' || result.back() == ' '))
                    result.pop_back();
            }
        }

        libssh2_channel_send_eof(ch);
        libssh2_channel_wait_eof(ch);
        libssh2_channel_close(ch);
        libssh2_channel_wait_closed(ch);
        libssh2_channel_free(ch);
        libssh2_session_set_blocking(sess, 0);

        if (result.empty() || result[0] != '/') {
            SDL_Log("[SFTP] guess_remote_cwd: got '%s', falling back to /\n", result.c_str());
            return "/";
        }
        SDL_Log("[SFTP] remote cwd: %s\n", result.c_str());
        return result;
    };
#endif

    uint32_t last_ticks = SDL_GetTicks();
    bool running = true;
    bool local_dead = false;  // set when the local child process has exited
    // Tracks whether the term FBO needs a full clear before the next draw.
    // Only set on layout changes (resize, theme, font) — NOT on PTY scroll.
    bool fbo_needs_clear = true;

    // Auto-scroll state for selection drag
    int  autoscroll_mouse_x = 0, autoscroll_mouse_y = 0;
    double autoscroll_accum = 0.0;

    while (running) {
        uint32_t now = SDL_GetTicks();
        double dt = (now - last_ticks) / 1000.0;
        last_ticks = now;
        bool needs_render = false;

        // Text blink (500ms) — mark all rows with ATTR_BLINK cells dirty.
        // Simplest safe approach: mark all dirty; blink is rare anyway.
        term.blink += dt;
        if (term.blink >= 0.5) {
            term.blink = 0;
            g_blink_text_on = !g_blink_text_on;
            term_dirty_all(&term);
            needs_render = true;
        }

        // Cursor blink (600ms) — only the cursor row needs to change
        if (term.cursor_blink_enabled) {
            term.cursor_blink += dt;
            if (term.cursor_blink >= 0.6) {
                term.cursor_blink = 0;
                term.cursor_blink_phase = !term.cursor_blink_phase;
                term_dirty_row(&term, term.cur_row);
                needs_render = true;
            }
        }

        // Kitty animation frames
        if (kitty_tick(dt))
            needs_render = true;

        // Auto-scroll during selection drag
        if (term.sel_active) {
            int margin = (int)term.cell_h;  // one cell height from edge triggers scroll
            int scroll_dir = 0;
            if (autoscroll_mouse_y < margin)                scroll_dir =  1; // toward top → more scrollback
            else if (autoscroll_mouse_y > win_h - margin)  scroll_dir = -1; // toward bottom → less scrollback
            if (scroll_dir != 0) {
                float past = (scroll_dir > 0)
                    ? (float)(margin - autoscroll_mouse_y) / margin        // above top edge
                    : (float)(autoscroll_mouse_y - (win_h - margin)) / margin; // below bottom edge
                if (past < 0.f) past = 0.f;
                double rate = 4.0 + past * 12.0;  // rows per second
                autoscroll_accum += dt * rate;
                int steps = (int)autoscroll_accum;
                autoscroll_accum -= steps;
                if (steps > 0) {
                    int new_off = SDL_clamp(term.sb_offset + scroll_dir * steps, 0, term.sb_count);
                    if (new_off != term.sb_offset) {
                        term.sb_offset = new_off;
                        // Update selection end to current mouse position
                        pixel_to_cell(&term, autoscroll_mouse_x, autoscroll_mouse_y, 2, 2,
                                      &term.sel_end_row, &term.sel_end_col);
                        term.sel_exists = true;
                        term_dirty_all(&term);
                        needs_render = true;
                    }
                }
            } else {
                autoscroll_accum = 0.0;
            }
        }

        // SSH connection state machine
#ifdef USESSH
        // Overlay: only redraw when progress actually changed or transfer just finished.
        // When idle (no transfer running), a 30ms sleep makes keyboard nav responsive
        // without spinning the CPU.
        if (g_sftp.visible) {
            static float s_last_progress = -1.f;
            static bool  s_last_transferring = false;
            float  cur_prog   = g_sftp.transferring ? sftp_progress() : g_sftp.progress;
            bool   cur_xfer   = g_sftp.transferring;
            bool   xfer_done  = s_last_transferring && !cur_xfer;  // transition: was running, now done
            if (cur_prog != s_last_progress || xfer_done || cur_xfer) {
                needs_render       = true;
                s_last_progress    = cur_prog;
            }
            s_last_transferring = cur_xfer;
            // Throttle overlay to ~33fps: responsive for keyboard nav, smooth for progress bar
            if (!needs_render)
                SDL_Delay(30);
        }
        if (use_ssh && ssh_phase == SshPhase::SETUP) {
            needs_render = true;
        } else if (use_ssh && ssh_phase == SshPhase::CONNECTING) {
            needs_render = true;

            // Check if background thread posted a password prompt request
            SDL_LockMutex(prompt_req.mtx);
            bool has_prompt = prompt_req.pending && !prompt_req.answered;
            std::string prompt_text = has_prompt ? prompt_req.prompt : "";
            SDL_UnlockMutex(prompt_req.mtx);

            if (has_prompt) {
                SDL_Log("[Main] detected pending prompt, switching to PROMPTING\n");
                term_feed(&term, prompt_text.c_str(), (int)prompt_text.size());
                ssh_field_input.clear();
                ssh_phase = SshPhase::PROMPTING;
            } else if (ssh_thread_done.load()) {
                ssh_thread.join();
                if (ssh_conn_ok) {
                    // sftp_init must run on the main thread — it temporarily
                    // sets the session to blocking mode, which races with
                    // ssh_read/ssh_write if done on the background thread.
                    sftp_init();
                    ssh_phase = SshPhase::ACTIVE;
                    SDL_StartTextInput();
                    // Defer port forward setup to next frame to ensure session is fully initialized
                    // (sftp_init may have temporarily set blocking mode)
                    for (auto &p : pf_locals_pending)
                        pf_add_local(p.lp, p.rh, p.rp);
                    // Schedule remote/socks for later with a delay to let libssh2 stabilize
                    ssh_setup_deferred_forwards = true;
                    ssh_setup_forward_delay = 100;  // ~100ms at 60fps

                    // Start web server if requested (SFTP mode)
                    if (start_webserver) {
                        if (!sftp_webserver_start(webserver_bind_addr.c_str(), webserver_port)) {
                            SDL_Log("[WEBSERVER] Failed to start SFTP web server on %s:%d\n",
                                    webserver_bind_addr.c_str(), webserver_port);
                        } else {
                            SDL_Log("[WEBSERVER] Started SFTP web server on %s:%d\n",
                                    webserver_bind_addr.c_str(), webserver_port);
                        }
                    }

                    needs_render = true;
                } else {
                    ssh_phase = SshPhase::FAILED;
                    const char *msg = "\r\nConnection failed. Press any key to close.\r\n";
                    term_feed(&term, msg, (int)strlen(msg));
                    SDL_StopTextInput();
                }
                needs_render = true;
            }
        } else if (use_ssh && ssh_phase == SshPhase::PROMPTING) {
            needs_render = true;
        }
#endif

        // Telnet connection state machine
        if (use_telnet && (telnet_phase == TelnetPhase::SETUP ||
                           telnet_phase == TelnetPhase::FAILED))
            needs_render = true;

        // Serial connection state machine
        if (use_serial && (serial_phase == SerialPhase::SETUP ||
                           serial_phase == SerialPhase::FAILED))
            needs_render = true;

        // PTY / SSH / Telnet read.
        // For local PTY: loop up to 8ms to drain burst output without falling behind.
        // For SSH/Telnet: one call per frame — both loop internally until EAGAIN.
#ifdef USESSH
        bool ssh_ready = !use_ssh || ssh_phase == SshPhase::ACTIVE || ssh_phase == SshPhase::FAILED;
#else
        constexpr bool ssh_ready = true;
#endif
        bool telnet_ready = !use_telnet || telnet_phase == TelnetPhase::ACTIVE || telnet_phase == TelnetPhase::FAILED;
        bool serial_ready = !use_serial || serial_phase == SerialPhase::ACTIVE || serial_phase == SerialPhase::FAILED;
        {
            bool had_sel = term.sel_exists || term.sel_active;
            int old_sb_count = term.sb_count;
            bool got_data = false;
#ifdef USESSH
            if (ssh_ready && telnet_ready && serial_ready) {
                got_data = TERM_READ();
            }
#else
            // Local PTY — drain in a tight loop up to 8ms budget
            uint32_t read_start = SDL_GetTicks();
            while (serial_ready && TERM_READ()) {
                got_data = true;
                if (SDL_GetTicks() - read_start >= 8) break;
            }
            (void)ssh_ready;
#endif
            
            // Apply deferred -R/-D port forwards after SSH session has stabilized
#ifdef USESSH
            if (ssh_setup_deferred_forwards && ssh_phase == SshPhase::ACTIVE) {
                if (ssh_setup_forward_delay > 0) {
                    ssh_setup_forward_delay--;
                } else {
                    for (auto &p : pf_remotes_pending) {
                        SDL_Log("[DEBUG] pf_add_remote (deferred): %d -> %s:%d\n", p.lp, p.rh.c_str(), p.rp);
                        pf_add_remote(p.lp, p.rh, p.rp);
                    }
                    for (int port : pf_socks_pending) {
                        SDL_Log("[DEBUG] pf_add_socks (deferred): %d\n", port);
                        pf_add_socks(port);
                    }
                    pf_remotes_pending.clear();
                    pf_socks_pending.clear();
                    ssh_setup_deferred_forwards = false;
                }
            }
#endif
            
            if (got_data) {
                needs_render = true;
                bool new_lines = (term.sb_count != old_sb_count);
                if (new_lines) {
                    if (g_autoscroll_enabled) {
                        // Autoscroll ON: jump to bottom to show new data
                        term.sb_offset = 0;
                    } else {
                        // Autoscroll OFF: keep viewport in same position by shifting offset
                        int new_lines_added = term.sb_count - old_sb_count;
                        term.sb_offset += new_lines_added;
                        if (term.sb_offset > term.sb_count) term.sb_offset = term.sb_count;
                    }
                    if (had_sel) { term.sel_exists = false; term.sel_active = false; }
                }
            }
        }

        // Child / channel exit check
#ifdef USESSH
        if (use_ssh && ssh_phase == SshPhase::FAILED) {
            // Connection failed during setup — already showing error
        } else if (use_ssh) {
            if (ssh_phase == SshPhase::ACTIVE && ssh_channel_closed()) {
                const char *msg = "\r\n[Connection closed — press spacebar to exit or close window]\r\n";
                term_feed(&term, msg, (int)strlen(msg));
                ssh_phase = SshPhase::FAILED;
            }
        } else
#endif
        if (use_telnet) {
            if (telnet_phase == TelnetPhase::ACTIVE && telnet_channel_closed() &&
                       (SDL_GetTicks() - telnet_active_since) >= 500) {
                const char *msg = "\r\n[Connection closed — press spacebar to exit or close window]\r\n";
                term_feed(&term, msg, (int)strlen(msg));
                telnet_phase = TelnetPhase::FAILED;
            }
        } else
        if (use_serial) {
            if (serial_phase == SerialPhase::ACTIVE && serial_channel_closed()) {
                const char *msg = "\r\n[Serial port closed — press spacebar to exit or close window]\r\n";
                term_feed(&term, msg, (int)strlen(msg));
                serial_phase = SerialPhase::FAILED;
            }
        } else
        {
#ifdef _WIN32
            if (!local_dead && term_child_exited()) {
                const char *msg = "\r\n[Process exited — press spacebar to exit or close window]\r\n";
                term_feed(&term, msg, (int)strlen(msg));
                local_dead = true;
            }
#else
            if (!local_dead) {
                int status;
                pid_t wp = (term.child > 0) ? waitpid(term.child, &status, WNOHANG) : 0;
                if (wp > 0) {
                    const char *msg = "\r\n[Process exited — press spacebar to exit or close window]\r\n";
                    term_feed(&term, msg, (int)strlen(msg));
                    local_dead = true;
                }
            }
#endif
        }

        // Event loop
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            needs_render = true;
            switch (ev.type) {
            case SDL_QUIT: settings_save();
#ifdef USESSH
                ssh_abort.store(true);
#endif
                running = false; break;

            case SDL_KEYDOWN: {
#ifdef USESSH
                // SETUP: collecting host, user (with visible echo)
                // PROMPTING: collecting password (with * echo)
                if (use_ssh && (ssh_phase == SshPhase::SETUP ||
                                ssh_phase == SshPhase::PROMPTING)) {
                    SDL_Keycode sym = ev.key.keysym.sym;
                    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
                        term_feed(&term, "\r\n", 2);

                        if (ssh_phase == SshPhase::SETUP) {
                            // Store the typed value into the right field
                            if (ssh_cfg.host.empty()) {
                                // User entered host — check if it contains username@host format
                                const char *at = strchr(ssh_field_input.c_str(), '@');
                                if (at) {
                                    // Parse "username@host[:port]" format
                                    ssh_cfg.user = std::string(ssh_field_input.c_str(), at - ssh_field_input.c_str());
                                    const char *host_start = at + 1;
                                    const char *colon = strrchr(host_start, ':');
                                    if (colon) {
                                        ssh_cfg.host = std::string(host_start, colon - host_start);
                                        ssh_cfg.port = atoi(colon + 1);
                                        if (ssh_cfg.port <= 0 || ssh_cfg.port > 65535) ssh_cfg.port = 22;
                                    } else {
                                        ssh_cfg.host = host_start;
                                    }
                                } else {
                                    // Just a hostname, will prompt for user later
                                    ssh_cfg.host = ssh_field_input;
                                }
                                
                                // Try to load SSH config for this host alias
                                ssh_config_load(ssh_cfg.host.c_str(), ssh_cfg);
                            } else if (ssh_cfg.user.empty())
                                ssh_cfg.user = ssh_field_input;
                            ssh_field_input.clear();

                            // Advance to next missing field, or start connecting
                            if (ssh_cfg.host.empty()) {
                                ssh_begin_prompt("Host: ", false);
                            } else if (ssh_cfg.user.empty()) {
                                char p[128];
                                snprintf(p, sizeof(p), "User (%s): ", ssh_cfg.host.c_str());
                                ssh_begin_prompt(p, false);
                            } else {
                                // All fields collected — start connection thread
                                ssh_phase = SshPhase::CONNECTING;

                                // Clear any SETUP prompt state so the CONNECTING
                                // handler doesn't mistake it for a password request
                                SDL_LockMutex(prompt_req.mtx);
                                prompt_req.pending  = false;
                                prompt_req.answered = false;
                                prompt_req.response.clear();
                                SDL_UnlockMutex(prompt_req.mtx);

                                char connecting_msg[256];
                                snprintf(connecting_msg, sizeof(connecting_msg),
                                         "Connecting to %s@%s:%d …\r\n",
                                         ssh_cfg.user.c_str(), ssh_cfg.host.c_str(), ssh_cfg.port);
                                term_feed(&term, connecting_msg, (int)strlen(connecting_msg));

                                ssh_cfg.prompt_password = [&](const char *prompt_str) -> std::string {
                                    SDL_LockMutex(prompt_req.mtx);
                                    prompt_req.prompt   = prompt_str;
                                    prompt_req.response.clear();
                                    prompt_req.answered = false;
                                    prompt_req.pending  = true;
                                    prompt_req.secret   = true;
                                    SDL_UnlockMutex(prompt_req.mtx);
                                    while (true) {
                                        if (ssh_abort.load()) return std::string{};
                                        SDL_LockMutex(prompt_req.mtx);
                                        bool done = prompt_req.answered;
                                        std::string resp = prompt_req.response;
                                        SDL_UnlockMutex(prompt_req.mtx);
                                        if (done) return resp;
                                        SDL_Delay(10);
                                    }
                                };
                                ssh_cfg.prompt_host_key = [&](const char *prompt_str) -> std::string {
                                    SDL_LockMutex(prompt_req.mtx);
                                    prompt_req.prompt   = prompt_str;
                                    prompt_req.response.clear();
                                    prompt_req.answered = false;
                                    prompt_req.pending  = true;
                                    prompt_req.secret   = false;
                                    SDL_UnlockMutex(prompt_req.mtx);
                                    while (true) {
                                        if (ssh_abort.load()) return std::string{};
                                        SDL_LockMutex(prompt_req.mtx);
                                        bool done = prompt_req.answered;
                                        std::string resp = prompt_req.response;
                                        SDL_UnlockMutex(prompt_req.mtx);
                                        if (done) return resp;
                                        SDL_Delay(10);
                                    }
                                };
                                ssh_thread = std::thread([&]() {
                                    ssh_conn_ok = ssh_connect(ssh_cfg, &term);
                                    ssh_thread_done.store(true);
                                });
                            }
                        } else {
                            // PROMPTING: post password response to waiting thread
                            SDL_LockMutex(prompt_req.mtx);
                            prompt_req.response = ssh_field_input;
                            prompt_req.pending  = false;
                            prompt_req.answered = true;
                            SDL_UnlockMutex(prompt_req.mtx);
                            ssh_field_input.clear();
                            ssh_phase = SshPhase::CONNECTING;
                        }
                    } else if (sym == SDLK_ESCAPE) {
                        if (ssh_phase == SshPhase::PROMPTING) {
                            term_feed(&term, "\r\n", 2);
                            SDL_LockMutex(prompt_req.mtx);
                            prompt_req.response.clear();
                            prompt_req.pending  = false;
                            prompt_req.answered = true;
                            SDL_UnlockMutex(prompt_req.mtx);
                            ssh_field_input.clear();
                            ssh_phase = SshPhase::CONNECTING;
                        }
                        // In SETUP, ignore Escape (keep prompting)
                    } else if (sym == SDLK_BACKSPACE && !ssh_field_input.empty()) {
                        ssh_field_input.pop_back();
                        if (ssh_phase != SshPhase::PROMPTING || !prompt_req.secret)
                            term_feed(&term, "\b \b", 3);
                    }
                    break;
                }
                // Spacebar exits after connection closed
                if (ev.key.keysym.sym == SDLK_SPACE) {
#ifdef USESSH
                    if (use_ssh && ssh_phase == SshPhase::FAILED) {
                        settings_save(); running = false; break;
                    }
#endif
                    if (use_telnet && telnet_phase == TelnetPhase::FAILED) {
                        settings_save(); running = false; break;
                    }
                    if (use_serial && serial_phase == SerialPhase::FAILED) {
                        settings_save(); running = false; break;
                    }
                    if (local_dead) {
                        settings_save(); running = false; break;
                    }
                }
                // F7/WOPR checked before ssh_ready so it works in local sessions
                if (ev.key.keysym.sym == SDLK_F7) {
                    if (g_wopr.visible) {
                        wopr_close();
                    } else {
                        // Close all other overlays first
                        g_help_visible         = false;
                        g_iv.visible           = false;
#ifdef USESSH
                        g_sftp.visible         = false;
                        g_sftp_console_visible = false;
                        g_pf_overlay.visible   = false;
                        g_ssh_key_mgr.visible  = false;
#endif
                        g_menu.visible         = false;
                        wopr_open();
                    }
                    needs_render = true;
                    break;
                }
                if (g_wopr.visible) {
                    wopr_keydown(ev.key.keysym.sym, nullptr);
                    needs_render = true;
                    break;
                }
                // Still connecting — ignore all normal key input
                if (!ssh_ready) break;
#endif
                // Telnet SETUP: collecting host, port, SSL interactively
                if (use_telnet && telnet_phase == TelnetPhase::SETUP) {
                    SDL_Keycode sym = ev.key.keysym.sym;
                    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
                        term_feed(&term, "\r\n", 2);
                        if (!telnet_collecting_port) {
                            // Host entered — ask for port
                            telnet_cfg.host = telnet_field_input;
                            telnet_collecting_port = true;
                            char p[64];
                            snprintf(p, sizeof(p), "Port [%d]: ", telnet_cfg.port);
                            telnet_begin_prompt(p);
                        } else if (!telnet_collecting_ssl) {
                            // Port entered (empty = keep default)
                            if (!telnet_field_input.empty()) {
                                int p = atoi(telnet_field_input.c_str());
                                if (p > 0 && p <= 65535) telnet_cfg.port = p;
                            }
                            // Ask SSL — default yes if port is 443
                            telnet_collecting_ssl = true;
                            char p[32];
                            snprintf(p, sizeof(p), "SSL? [%s]: ", telnet_cfg.port == 443 ? "Y/n" : "y/N");
                            telnet_begin_prompt(p);
                        } else {
                            // SSL entered — y/Y/yes = true, empty keeps port-based default
                            if (!telnet_field_input.empty()) {
                                char c = telnet_field_input[0];
                                telnet_cfg.use_ssl = (c == 'y' || c == 'Y');
                            } else {
                                telnet_cfg.use_ssl = (telnet_cfg.port == 443);
                            }
                            // All fields collected — connect now
                            char msg[256];
                            bool will_be_ssl = telnet_cfg.use_ssl;
                            bool will_be_raw = !will_be_ssl && (telnet_cfg.raw_mode || telnet_cfg.port != 23);
                            const char *mode_str = will_be_ssl ? "SSL" : will_be_raw ? "raw TCP" : "telnet";
                            snprintf(msg, sizeof(msg), "Connecting to %s:%d (%s) …\r\n",
                                     telnet_cfg.host.c_str(), telnet_cfg.port, mode_str);
                            term_feed(&term, msg, (int)strlen(msg));
                            if (telnet_connect(telnet_cfg, &term)) {
                                telnet_phase = TelnetPhase::ACTIVE;
                                telnet_active_since = SDL_GetTicks();

                                // Start web server if requested (local mode for telnet)
                                if (start_webserver) {
                                    if (!sftp_webserver_start_local(webserver_root.c_str(), webserver_bind_addr.c_str(), webserver_port)) {
                                        SDL_Log("[WEBSERVER] Failed to start local web server on %s:%d\n",
                                                webserver_bind_addr.c_str(), webserver_port);
                                    } else {
                                        SDL_Log("[WEBSERVER] Started local web server on %s:%d\n",
                                                webserver_bind_addr.c_str(), webserver_port);
                                    }
                                }
                            } else {
                                telnet_phase = TelnetPhase::FAILED;
                                const char *fail = "\r\nConnection failed. Press any key to close.\r\n";
                                term_feed(&term, fail, (int)strlen(fail));
                            }
                        }
                    } else if (sym == SDLK_BACKSPACE && !telnet_field_input.empty()) {
                        telnet_field_input.pop_back();
                        term_feed(&term, "\b \b", 3);
                    }
                    break;
                }
                // Still in Telnet setup — ignore normal key input
                if (use_telnet && !telnet_ready) break;
                // Serial SETUP: collecting port, baud, data bits, parity, stop bits, flow
                if (use_serial && serial_phase == SerialPhase::SETUP) {
                    SDL_Keycode sym = ev.key.keysym.sym;
                    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
                        term_feed(&term, "\r\n", 2);
                        switch (serial_field) {
                        case SerialField::PORT:
                            if (!serial_field_input.empty())
                                serial_cfg.port = serial_field_input;
                            if (serial_cfg.port.empty()) {
                                serial_begin_prompt("Port cannot be empty.\r\nPort: ");
                            } else {
                                char p[64];
                                snprintf(p, sizeof(p), "Baud rate [%d]: ", serial_cfg.baud);
                                serial_field = SerialField::BAUD;
                                serial_begin_prompt(p);
                            }
                            break;
                        case SerialField::BAUD:
                            if (!serial_field_input.empty()) {
                                int b = atoi(serial_field_input.c_str());
                                if (b > 0) serial_cfg.baud = b;
                            }
                            serial_field = SerialField::DATA_BITS;
                            {
                                char p[64];
                                snprintf(p, sizeof(p), "Data bits [%d]: ", serial_cfg.data_bits);
                                serial_begin_prompt(p);
                            }
                            break;
                        case SerialField::DATA_BITS:
                            if (!serial_field_input.empty()) {
                                int d = atoi(serial_field_input.c_str());
                                if (d >= 5 && d <= 8) serial_cfg.data_bits = d;
                            }
                            serial_field = SerialField::PARITY;
                            serial_begin_prompt("Parity [N/o/e]: ");
                            break;
                        case SerialField::PARITY:
                            if (!serial_field_input.empty()) {
                                char c = serial_field_input[0];
                                if (c == 'o' || c == 'O')      serial_cfg.parity = SerialParity::ODD;
                                else if (c == 'e' || c == 'E') serial_cfg.parity = SerialParity::EVEN;
                                else                           serial_cfg.parity = SerialParity::NONE;
                            }
                            serial_field = SerialField::STOP_BITS;
                            serial_begin_prompt("Stop bits [1/2]: ");
                            break;
                        case SerialField::STOP_BITS:
                            if (!serial_field_input.empty() && serial_field_input[0] == '2')
                                serial_cfg.stop_bits = SerialStopBits::TWO;
                            else
                                serial_cfg.stop_bits = SerialStopBits::ONE;
                            serial_field = SerialField::FLOW;
                            serial_begin_prompt("Flow control [N/h/s] (none/hardware/software): ");
                            break;
                        case SerialField::FLOW:
                            if (!serial_field_input.empty()) {
                                char c = serial_field_input[0];
                                if (c == 'h' || c == 'H')      serial_cfg.flow = SerialFlow::HARDWARE;
                                else if (c == 's' || c == 'S') serial_cfg.flow = SerialFlow::SOFTWARE;
                                else                           serial_cfg.flow = SerialFlow::NONE;
                            }
                            // All fields collected — connect now
                            {
                                char msg[256];
                                const char *parity_str[] = {"N","O","E","M","S"};
                                const char *stop_str[]   = {"1","1.5","2"};
                                const char *flow_str[]   = {"none","hw","sw"};
                                snprintf(msg, sizeof(msg),
                                         "Opening %s @ %d %d%s%s flow=%s …\r\n",
                                         serial_cfg.port.c_str(),
                                         serial_cfg.baud,
                                         serial_cfg.data_bits,
                                         parity_str[(int)serial_cfg.parity],
                                         stop_str[(int)serial_cfg.stop_bits],
                                         flow_str[(int)serial_cfg.flow]);
                                term_feed(&term, msg, (int)strlen(msg));
                            }
                            if (serial_connect(serial_cfg, &term)) {
                                serial_phase = SerialPhase::ACTIVE;

                                // Start web server if requested (local mode for serial)
                                if (start_webserver) {
                                    if (!sftp_webserver_start_local(webserver_root.c_str(), webserver_bind_addr.c_str(), webserver_port)) {
                                        SDL_Log("[WEBSERVER] Failed to start local web server on %s:%d\n",
                                                webserver_bind_addr.c_str(), webserver_port);
                                    } else {
                                        SDL_Log("[WEBSERVER] Started local web server on %s:%d\n",
                                                webserver_bind_addr.c_str(), webserver_port);
                                    }
                                }
                            } else {
                                serial_phase = SerialPhase::FAILED;
                                const char *fail = "\r\nFailed to open port. Press any key to close.\r\n";
                                term_feed(&term, fail, (int)strlen(fail));
                            }
                            break;
                        }
                    } else if (sym == SDLK_BACKSPACE && !serial_field_input.empty()) {
                        serial_field_input.pop_back();
                        term_feed(&term, "\b \b", 3);
                    }
                    break;
                }
                // Still in Serial setup — ignore normal key input
                if (use_serial && !serial_ready) break;
                // F11 fullscreen — checked before any overlay so it always works
                if (ev.key.keysym.sym == SDLK_F11) {
                    bool is_full = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
                    SDL_SetWindowFullscreen(window, is_full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                    break;
                }

                // SFTP console (F4) — forward all keys when visible
                if (g_ssh_key_mgr.visible) {
                    if (ssh_key_mgr_keydown(ev.key.keysym, nullptr))
                        needs_render = true;
                    break;
                }
#ifdef USESSH
                if (g_sftp_console_visible) {
                    if (sftp_console_keydown(ev.key.keysym, nullptr))
                        needs_render = true;
                    else break;  // not consumed (e.g. F11) — fall through
                    break;
                }
#endif
                // SFTP overlay — forward all keys when visible
#ifdef USESSH
                if (g_sftp.visible) {
                    if (sftp_overlay_keydown(ev.key.keysym.sym))
                        needs_render = true;
                    else break;  // not consumed (e.g. F11) — fall through
                    break;
                }
                // F2 = upload, F3 = download, F4 = SFTP console
                if (use_ssh && ssh_active()) {
                    if (ev.key.keysym.sym == SDLK_F2) {
                        SDL_Log("[SFTP] F2 upload triggered\n");
                        g_help_visible = false;
                        g_iv.visible   = false;
                        if (g_wopr.visible) wopr_close();
                        g_pf_overlay.visible   = false;
                        g_ssh_key_mgr.visible  = false;
                        g_sftp_console_visible = false;
                        g_menu.visible = false;
                        SDL_GetWindowSize(window, &win_w, &win_h);
                        sftp_overlay_open(SftpOverlayMode::UPLOAD,
                                          guess_remote_cwd().c_str(), win_w, win_h);
                        needs_render = true;
                        break;
                    }
                    if (ev.key.keysym.sym == SDLK_F3) {
                        SDL_Log("[SFTP] F3 download triggered\n");
                        g_help_visible = false;
                        g_iv.visible   = false;
                        if (g_wopr.visible) wopr_close();
                        g_pf_overlay.visible   = false;
                        g_ssh_key_mgr.visible  = false;
                        g_sftp_console_visible = false;
                        g_menu.visible = false;
                        SDL_GetWindowSize(window, &win_w, &win_h);
                        sftp_overlay_open(SftpOverlayMode::DOWNLOAD,
                                          guess_remote_cwd().c_str(), win_w, win_h);
                        needs_render = true;
                        break;
                    }
                    if (ev.key.keysym.sym == SDLK_F4) {
                        SDL_Log("[SFTP] F4 console triggered\n");
                        g_help_visible = false;
                        g_iv.visible   = false;
                        if (g_wopr.visible) wopr_close();
                        g_pf_overlay.visible = false;
                        g_ssh_key_mgr.visible  = false;
                        g_sftp.visible       = false;
                        g_menu.visible       = false;
                        SDL_GetWindowSize(window, &win_w, &win_h);
                        sftp_console_open(win_w, win_h);
                        needs_render = true;
                        break;
                    }
                }
#endif
                if (ev.key.repeat) {
                    SDL_Keycode sym = ev.key.keysym.sym;
                    if (sym >= SDLK_SPACE && sym < SDLK_DELETE)
                        break;
                }
                if (ev.key.keysym.sym == SDLK_F1) {
                    // Close all other overlays before toggling help
                    if (!g_help_visible) {
                        g_iv.visible = false;
                        if (g_wopr.visible) wopr_close();
#ifdef USESSH
                        g_sftp.visible         = false;
                        g_sftp_console_visible = false;
                        g_pf_overlay.visible   = false;
                        g_ssh_key_mgr.visible  = false;
#endif
                        g_menu.visible = false;
                    }
                    g_help_visible = !g_help_visible;
                    needs_render = true;
                    break;
                }
                if (ev.key.keysym.sym == SDLK_F12) {
                    // Close all other overlays before toggling the debug log
                    if (!g_debuglog_visible) {
                        g_help_visible = false;
                        g_iv.visible   = false;
                        if (g_wopr.visible) wopr_close();
#ifdef USESSH
                        g_sftp.visible         = false;
                        g_sftp_console_visible = false;
                        g_pf_overlay.visible   = false;
                        g_ssh_key_mgr.visible  = false;
#endif
                        g_menu.visible = false;
                    }
                    g_debuglog_visible = !g_debuglog_visible;
                    needs_render = true;
                    break;
                }
                if (ev.key.keysym.sym == SDLK_F5) {
                    // Close all other overlays before opening image viewer
                    g_help_visible = false;
                    if (g_wopr.visible) wopr_close();
#ifdef USESSH
                    g_sftp.visible         = false;
                    g_sftp_console_visible = false;
                    g_pf_overlay.visible   = false;
                    g_ssh_key_mgr.visible  = false;
#endif
                    g_menu.visible = false;
                    SDL_GetWindowSize(window, &win_w, &win_h);
#ifdef USESSH
                    iv_open(use_ssh && ssh_active(), win_w, win_h);
#else
                    iv_open(false, win_w, win_h);
#endif
                    needs_render = true;
                    break;
                }
#ifdef USESSH
                if (ev.key.keysym.sym == SDLK_F6 && use_ssh && ssh_active()) {
                    if (g_pf_overlay.visible) {
                        g_pf_overlay.visible = false;
                        g_ssh_key_mgr.visible  = false;
                    } else {
                        // Close all other overlays first
                        g_help_visible         = false;
                        g_iv.visible           = false;
                        if (g_wopr.visible) wopr_close();
                        g_sftp.visible         = false;
                        g_sftp_console_visible = false;
                        g_menu.visible         = false;
                        pf_overlay_open();
                    }
                    needs_render = true;
                    break;
                }
                if (g_pf_overlay.visible) {
                    if (pf_overlay_keydown(ev.key.keysym.sym))
                        needs_render = true;
                    else break;  // not consumed (e.g. F11) — fall through
                    break;
                }
                // F9 — toggle web file browser. Remote (SFTP-backed) when an
                // SSH session is active, local filesystem browser otherwise.
                if (ev.key.keysym.sym == SDLK_F9) {
                    char url[64];
                    if (sftp_webserver_running()) {
                        sftp_webserver_stop();
                        SDL_Log("[WebServer] File browser stopped\n");
                        toast_show("Web file browser OFF", nullptr, 0.62f, 0.62f, 0.68f);
                    } else if (use_ssh && ssh_active()) {
                        if (sftp_webserver_start()) {
                            SDL_Log("[WebServer] Remote file browser: http://localhost:%d\n",
                                    sftp_webserver_get_port());
                            snprintf(url, sizeof(url), "http://localhost:%d  (remote, via SFTP)",
                                     sftp_webserver_get_port());
                            toast_show("Web file browser ON", url, 0.30f, 0.85f, 0.45f);
                        } else {
                            toast_show("Web file browser failed to start", "See the debug log (F12)",
                                       0.95f, 0.35f, 0.35f);
                        }
                    } else {
                        if (sftp_webserver_start_local("/")) {
                            SDL_Log("[WebServer] Local file browser: http://localhost:%d\n",
                                    sftp_webserver_get_port());
                            snprintf(url, sizeof(url), "http://localhost:%d  (local files)",
                                     sftp_webserver_get_port());
                            toast_show("Web file browser ON", url, 0.30f, 0.85f, 0.45f);
                        } else {
                            toast_show("Web file browser failed to start", "See the debug log (F12)",
                                       0.95f, 0.35f, 0.35f);
                        }
                    }
                    needs_render = true;
                    break;
                }
#endif
                if (g_iv.visible) {
                    iv_keydown(ev.key.keysym.sym);
                    needs_render = true;
                    break;
                }
                if (g_custom_shell_dialog_open) {
                    custom_shell_dialog_keydown(ev.key.keysym.sym, nullptr);
                    needs_render = true;
                    break;
                }
                if (g_help_visible) {
                    help_keydown(ev.key.keysym.sym);
                    needs_render = true;
                    break;
                }
                if (g_debuglog_visible) {
                    debuglog_keydown(ev.key.keysym.sym);
                    needs_render = true;
                    break;
                }
                if (g_menu.visible) { g_menu.visible = false; break; }
                SDL_Keymod mod = SDL_GetModState();
                int page = term.rows - 1;
                if (ev.key.keysym.sym == SDLK_PAGEUP && (mod & KMOD_SHIFT)) {
                    term.sb_offset = SDL_min(term.sb_offset + page, term.sb_count);
                    term_dirty_all(&term);
                    break;
                }
                if (ev.key.keysym.sym == SDLK_PAGEDOWN && (mod & KMOD_SHIFT)) {
                    term.sb_offset = SDL_max(term.sb_offset - page, 0);
                    term_dirty_all(&term);
                    break;
                }
                if (mod & KMOD_CTRL) {
                    if (ev.key.keysym.sym == SDLK_c && (mod & KMOD_SHIFT)) {
                        term_copy_selection_html(&term); break;
                    }
                    if (ev.key.keysym.sym == SDLK_c && term.sel_exists) {
                        term_copy_selection_rich(&term); break;
                    }
                    if (ev.key.keysym.sym == SDLK_v) { term_paste(&term); break; }
                    if (ev.key.keysym.sym == SDLK_LSHIFT ||
                        ev.key.keysym.sym == SDLK_RSHIFT) break;
                }
                handle_key(&term, ev.key.keysym, NULL);
                if (TERM_READ()) needs_render = true;
                break;
            }

            case SDL_TEXTINPUT: {
                // Block text input when image viewer is visible
                if (g_iv.visible) {
                    break;
                }
                // Handle custom shell dialog input first
                if (g_custom_shell_dialog_open) {
                    custom_shell_dialog_keydown(SDLK_UNKNOWN, ev.text.text);
                    needs_render = true;
                    break;
                }
#ifdef USESSH
                // Forward text input to SSH key manager when visible
                if (g_ssh_key_mgr.visible) {
                    SDL_Keysym ks{}; ks.sym = SDLK_UNKNOWN;
                    ssh_key_mgr_keydown(ks, ev.text.text);
                    needs_render = true;
                    break;
                }
                // Forward text to SFTP console when visible
                if (g_sftp_console_visible) {
                    SDL_Keysym ks{}; ks.sym = SDLK_UNKNOWN;
                    sftp_console_keydown(ks, ev.text.text);
                    needs_render = true;
                    break;
                }
                // Feed text into port-forward overlay input field when active
                if (g_pf_overlay.visible) {
                    pf_overlay_textinput(ev.text.text);
                    needs_render = true;
                    break;
                }
                // Drop text input entirely when the SFTP overlay is up
                if (g_sftp.visible) break;
                // Forward text to WOPR login fields
                if (g_wopr.visible) {
                    wopr_keydown(SDLK_UNKNOWN, ev.text.text);
                    needs_render = true;
                    break;
                }
                if (use_ssh && ssh_phase == SshPhase::SETUP) {
                    // Visible echo for host/user fields
                    ssh_field_input += ev.text.text;
                    term_feed(&term, ev.text.text, (int)strlen(ev.text.text));
                    break;
                }
                if (use_ssh && ssh_phase == SshPhase::PROMPTING) {
                    ssh_field_input += ev.text.text;
                    if (!prompt_req.secret)  // don't echo passwords, but do echo yes/no etc.
                        term_feed(&term, ev.text.text, (int)strlen(ev.text.text));
                    break;
                }
                if (!ssh_ready) break;
#endif
                if (use_telnet && telnet_phase == TelnetPhase::SETUP) {
                    // Visible echo for host/port fields
                    telnet_field_input += ev.text.text;
                    term_feed(&term, ev.text.text, (int)strlen(ev.text.text));
                    break;
                }
                if (use_telnet && !telnet_ready) break;
                if (use_serial && serial_phase == SerialPhase::SETUP) {
                    // Visible echo for all serial fields
                    serial_field_input += ev.text.text;
                    term_feed(&term, ev.text.text, (int)strlen(ev.text.text));
                    break;
                }
                if (use_serial && !serial_ready) break;
                SDL_Keymod mod = SDL_GetModState();
                if (!(mod & KMOD_CTRL) && !(mod & KMOD_ALT)) {
                    TERM_WRITE(ev.text.text, (int)strlen(ev.text.text));
                    if (TERM_READ()) needs_render = true;
                }
                break;
            }

            case SDL_MOUSEBUTTONDOWN: {
                if (g_ssh_key_mgr.visible) {
                    ssh_key_mgr_mousedown(ev.button.x, ev.button.y, ev.button.button);
                    needs_render = true;
                    break;
                }
#ifdef USESSH
                if (g_sftp_console_visible) {
                    sftp_console_mousedown(ev.button.x, ev.button.y, ev.button.button);
                    needs_render = true;
                    break;
                }
#endif
                // Clicking anywhere dismisses the help overlay (links open their URL)
                if (g_help_visible) {
                    help_mousedown(ev.button.x, ev.button.y);
                    needs_render = true;
                    break;
                }
                if (g_debuglog_visible) {
                    g_debuglog_visible = false;
                    needs_render = true;
                    break;
                }
#ifdef USESSH
                // SFTP overlay captures all clicks
                if (g_sftp.visible) {
                    SDL_GetWindowSize(window, &win_w, &win_h);
                    sftp_overlay_mousedown(ev.button.x, ev.button.y, ev.button.button, win_w, win_h);
                    needs_render = true;
                    break;
                }
#endif
                // Image viewer captures all clicks
                if (g_iv.visible) {
                    SDL_GetWindowSize(window, &win_w, &win_h);
                    iv_mousedown(ev.button.x, ev.button.y, ev.button.button, win_w, win_h);
                    needs_render = true;
                    break;
                }
                // WOPR overlay captures all clicks
                if (g_wopr.visible) {
                    wopr_mousedown(ev.button.x, ev.button.y, ev.button.button);
                    needs_render = true;
                    break;
                }
#ifdef USESSH
                // Port-forward overlay captures all clicks
                if (g_pf_overlay.visible) {
                    needs_render = true;
                    break;
                }
#endif
                int r, c;
                pixel_to_cell(&term, ev.button.x, ev.button.y, 2, 2, &r, &c);
                if (term.mouse_report && !g_menu.visible) {
                    int btn = (ev.button.button == SDL_BUTTON_LEFT)   ? 0 :
                              (ev.button.button == SDL_BUTTON_MIDDLE) ? 1 :
                              (ev.button.button == SDL_BUTTON_RIGHT)  ? 2 : -1;
                    if (btn >= 0) {
                        char seq[32]; int slen;
                        if (term.mouse_sgr)
                            slen = snprintf(seq,sizeof(seq),"\x1b[<%d;%d;%dM",btn,c+1,r+1);
                        else
                            slen = snprintf(seq,sizeof(seq),"\x1b[M%c%c%c",
                                            (char)(32+btn),(char)(32+c+1),(char)(32+r+1));
                        TERM_WRITE(seq, slen);
                    }
                    break;
                }
                if (ev.button.button == SDL_BUTTON_RIGHT) {
                    SDL_GetWindowSize(window, &win_w, &win_h);
                    menu_open(&g_menu, ev.button.x, ev.button.y, win_w, win_h);
                    break;
                } else if (ev.button.button == SDL_BUTTON_LEFT) {
                    // Ctrl+click opens URL
                    if (!g_menu.visible) {
                        SDL_Keymod mod = SDL_GetModState();
                        if (mod & KMOD_CTRL) {
                            std::string url = url_at_pixel(&term, ev.button.x, ev.button.y, 2, 2);
                            if (!url.empty()) {
                                open_url(url);
                                break;
                            }
                        }
                    }
                    if (g_menu.visible) {
                        int sub_hit = submenu_hit(&g_menu, ev.button.x, ev.button.y);
                        if (sub_hit >= 0) {
                            if (g_menu.sub_open == MENU_ID_THEMES) {
                                apply_theme(sub_hit);
                                settings_save();
                                fbo_needs_clear = true;
                                term_dirty_all(&term);
                            } else if (g_menu.sub_open == MENU_ID_OPACITY) {
                                static const float opacities[] = {1.0f,0.85f,0.7f,0.5f,0.3f,0.1f};
                                g_opacity = opacities[sub_hit];
                                SDL_SetWindowOpacity(window, g_opacity);
                                term_dirty_all(&term);
                            } else if (g_menu.sub_open == MENU_ID_RENDER_MODE) {
                                if (sub_hit == RENDER_MODE_NORMAL) {
                                    g_render_mode = 0;
                                } else {
                                    g_render_mode ^= (1u << sub_hit);
                                }
                                term_dirty_all(&term);
                            } else if (g_menu.sub_open == MENU_ID_NEW_TERMINAL) {
                                action_new_terminal_custom(sub_hit);
                            } else if (g_menu.sub_open == MENU_ID_ENTERTAINMENT) {
                                if      (sub_hit == ENT_IDX_FIGHT)    fight_set_enabled(!fight_get_enabled());
                                else if (sub_hit == ENT_IDX_BOUNCING) bc_set_enabled(!bc_get_enabled());
                                else if (sub_hit == ENT_IDX_SOUND)  { term_audio_set_enabled(!term_audio_get_enabled()); settings_save(); }
                                // keep submenu open so user can toggle multiple items
                                break;
                            } else if (g_menu.sub_open == MENU_ID_FONTS) {
                                SDL_GetWindowSize(window, &win_w, &win_h);
                                font_apply(g_font_list[sub_hit], g_font_list, &term, win_w, win_h);
                                font_save_config(g_font_list[sub_hit].display_name);
                                ft_invalidate_glyph_cache();
                                fbo_needs_clear = true;
                                term_dirty_all(&term);
                            } else if (g_menu.sub_open == MENU_ID_ADV_OPTIONS) {
                                if (sub_hit == 0) {
                                    // Sticky Prompt
                                    sticky_prompt_toggle();
                                } else if (sub_hit == 1) {
                                    // Disable Autoscroll
                                    g_autoscroll_enabled = !g_autoscroll_enabled;
                                } else if (sub_hit == 2) {
                                    // Line Numbers
                                    g_line_numbers_enabled = !g_line_numbers_enabled;
                                    term_dirty_all(&term);  // Redraw all rows when toggling line numbers
                                }
                                needs_render = true;
                                // Keep submenu open so user can toggle multiple items
                                break;
                            }
                            g_menu.visible = false;
                        } else {
                            int hit = menu_hit(&g_menu, ev.button.x, ev.button.y);
                            bool is_sub_parent = (hit==MENU_ID_THEMES || hit==MENU_ID_OPACITY ||
                                                  hit==MENU_ID_RENDER_MODE || hit==MENU_ID_ENTERTAINMENT ||
                                                  hit==MENU_ID_NEW_TERMINAL || hit==MENU_ID_FONTS ||
                                                  hit==MENU_ID_ADV_OPTIONS);
                            if (!is_sub_parent) g_menu.visible = false;
                            switch (hit) {
                            case MENU_ID_COPY:      g_menu_copy_pending = true; term_copy_selection_rich(&term); break;
                            case MENU_ID_COPY_HTML: g_menu_copy_pending = true; term_copy_selection_html(&term); break;
                            case MENU_ID_COPY_ANSI: g_menu_copy_pending = true; term_copy_selection_ansi(&term); break;
                            case MENU_ID_PASTE:     term_paste(&term); break;
                            case MENU_ID_RESET:
                                term_soft_reset(&term);
                                needs_render = true;
#ifdef WIN32
                                TERM_WRITE("cls\r\n",5);
#else
                                TERM_WRITE("reset\n",6);
#endif
                                break;
                            case MENU_ID_SELECT_ALL: term_select_all(&term); break;
                            case MENU_ID_HELP:
                                g_iv.visible = false;
                                if (g_wopr.visible) wopr_close();
#ifdef USESSH
                                g_sftp.visible         = false;
                                g_sftp_console_visible = false;
                                g_pf_overlay.visible   = false;
                                g_ssh_key_mgr.visible  = false;
#endif
                                g_help_visible = true;
                                needs_render = true;
                                break;
                            case MENU_ID_QUIT: settings_save(); running = false; break;
                            default:
                                if (hit < 0) g_menu.visible = false;
                                break;
                            }
                        }
                    } else {
                        autoscroll_mouse_x = ev.button.x;
                        autoscroll_mouse_y = ev.button.y;
                        
                        // Double-click word selection
                        static uint32_t last_click_time = 0;
                        static int last_click_row = -1, last_click_col = -1;
                        uint32_t now = SDL_GetTicks();
                        bool is_double_click = (now - last_click_time < 500 &&
                                                last_click_row == r && last_click_col == c);
                        
                        if (is_double_click) {
                            // Select entire word on double-click
                            Cell *cell = vcell(&term, r, c);
                            
                            // Check if we're on a word character
                            auto is_separator = [](uint32_t cp) {
                                if (cp <= 32 || cp == 127) return true;
                                switch (cp) {
                                    case '!':  case '"': case '#': case '$': case '%': case '&':
                                    case '(':  case ')': case '*': case '+': case ',': case '|':
                                    case ';':  case '<': case '=': case '`': case '^': case '{':
                                    case '>':  case '?': case '[': case ']': case '}':       
                                        return true;
                                    default: return false;
                                }
                            };
                            
                            if (cell && !is_separator(cell->cp)) {
                                // Expand left
                                int start_col = c;
                                for (int col = c; col >= 0; col--) {
                                    Cell *ch = vcell(&term, r, col);
                                    if (!ch || is_separator(ch->cp)) {
                                        start_col = col + 1;
                                        break;
                                    }
                                    if (col == 0) start_col = 0;
                                }
                                
                                // Expand right
                                int end_col = c;
                                for (int col = c; col < term.cols; col++) {
                                    Cell *ch = vcell(&term, r, col);
                                    if (!ch || is_separator(ch->cp)) {
                                        end_col = col - 1;
                                        break;
                                    }
                                    if (col == term.cols - 1) end_col = term.cols - 1;
                                }
                                
                                term.sel_start_row = term.sel_end_row = r;
                                term.sel_start_col = start_col;
                                term.sel_end_col = end_col;
                                term.sel_active = false;
                                term.sel_exists = true;
                            } else {
                                // Single character selection if on separator
                                term.sel_start_row = term.sel_end_row = r;
                                term.sel_start_col = term.sel_end_col = c;
                                term.sel_active = false;
                                term.sel_exists = true;
                            }
                        } else {
                            // Single click: start selection
                            term.sel_start_row = term.sel_end_row = r;
                            term.sel_start_col = term.sel_end_col = c;
                            term.sel_active = true;
                            term.sel_exists = false;
                        }
                        
                        last_click_time = now;
                        last_click_row = r;
                        last_click_col = c;
                        term_dirty_all(&term);
                    }
                } else if (ev.button.button == SDL_BUTTON_MIDDLE) {
                    if (g_menu.visible) g_menu.visible = false;
                    else term_paste(&term);
                }
                break;
            }

            case SDL_MOUSEMOTION:
                if (g_ssh_key_mgr.visible) {
                    ssh_key_mgr_mousemotion(ev.motion.x, ev.motion.y,
                                            (ev.motion.state & SDL_BUTTON_LMASK) != 0);
                    needs_render = true;
                    break;
                }
#ifdef USESSH
                if (g_sftp_console_visible) {
                    sftp_console_mousemotion(ev.motion.x, ev.motion.y,
                                             (ev.motion.state & SDL_BUTTON_LMASK) != 0);
                    needs_render = true;
                    break;
                }
#endif
                if (g_wopr.visible) {
                    if (wopr_mousemove(ev.motion.x, ev.motion.y))
                        needs_render = true;
                    break;
                }
                if (g_iv.visible) {
                    SDL_GetWindowSize(window, &win_w, &win_h);
                    if (iv_mousemotion(ev.motion.x, ev.motion.y, win_w, win_h))
                        needs_render = true;
                    break;
                }
                if (g_help_visible) {
                    if (help_mousemotion(ev.motion.x, ev.motion.y))
                        needs_render = true;
                    break;
                }
#ifdef USESSH
                if (g_sftp.visible) { break; }
                if (g_pf_overlay.visible) { break; }
#endif
                if (g_menu.visible) {
                    int hit = menu_hit(&g_menu, ev.motion.x, ev.motion.y);
                    if (hit >= 0) g_menu.hovered = hit;
                    if (hit == MENU_ID_THEMES || hit == MENU_ID_OPACITY ||
                        hit == MENU_ID_RENDER_MODE || hit == MENU_ID_ENTERTAINMENT ||
                        hit == MENU_ID_NEW_TERMINAL || hit == MENU_ID_FONTS ||
                        hit == MENU_ID_ADV_OPTIONS) {
                        if (g_menu.sub_open != hit) {
                            g_menu.sub_open = hit; g_menu.sub_hovered = -1;
                            g_menu.sub_x = g_menu.x + g_menu.width + 2;
                            int item_y = g_menu.y + 4;
                            for (int i=0;i<hit;i++)
                                item_y += MENU_ITEMS[i].separator ? g_menu.sep_h : g_menu.item_h;
                            g_menu.sub_y = item_y;
                            int count = (hit==MENU_ID_THEMES)        ? THEME_COUNT :
                                        (hit==MENU_ID_RENDER_MODE)    ? RENDER_MODE_COUNT :
                                        (hit==MENU_ID_ENTERTAINMENT)  ? ENT_COUNT :
                                        (hit==MENU_ID_NEW_TERMINAL)   ? (int)g_available_terminals.size() :
                                        (hit==MENU_ID_FONTS)          ? (int)g_font_list.size() : 6;
                            int sw = g_menu.width + g_font_size*2;
                            int sh = count * g_menu.item_h + 8;
                            SDL_GetWindowSize(window, &win_w, &win_h);
                            if (g_menu.sub_x + sw > win_w) g_menu.sub_x = g_menu.x - sw - 2;
                            if (g_menu.sub_y + sh > win_h) g_menu.sub_y = win_h - sh - 2;
                        }
                    } else if (hit >= 0) {
                        g_menu.sub_open = -1;
                    }
                    g_menu.sub_hovered = submenu_hit(&g_menu, ev.motion.x, ev.motion.y);
                } else if (term.sel_active && (ev.motion.state & SDL_BUTTON_LMASK)) {
                    autoscroll_mouse_x = ev.motion.x;
                    autoscroll_mouse_y = ev.motion.y;
                    pixel_to_cell(&term, ev.motion.x, ev.motion.y, 2, 2,
                                  &term.sel_end_row, &term.sel_end_col);
                    term.sel_exists = true;
                    term_dirty_all(&term);
                } else {
                    // Update URL hover highlight — only redraw the affected row
                    if (url_update_hover(&term, ev.motion.x, ev.motion.y, 2, 2)) {
                        // Redraw everything: the link being left may be on
                        // another row, and an OSC 8 link can span several
                        term_dirty_all(&term);
                        needs_render = true;
                    }
                    // Show pointer cursor when over a URL (Ctrl = clickable)
                    SDL_Keymod mod = SDL_GetModState();
                    std::string hurl = url_at_pixel(&term, ev.motion.x, ev.motion.y, 2, 2);
                    if (!hurl.empty() && (mod & KMOD_CTRL))
                        SDL_SetCursor(cursor_hand);
                    else
                        SDL_SetCursor(cursor_ibeam);
                }
                break;

            case SDL_MOUSEBUTTONUP: {
#ifdef USESSH
                if (g_sftp_console_visible) {
                    sftp_console_mouseup(ev.button.x, ev.button.y);
                    needs_render = true;
                    break;
                }
#endif
                if (g_wopr.visible) {
                    wopr_mouseup(ev.button.x, ev.button.y, ev.button.button);
                    needs_render = true;
                    break;
                }
                if (g_iv.visible) {
                    iv_mouseup(ev.button.x, ev.button.y, ev.button.button);
                    needs_render = true;
                    break;
                }
#ifdef USESSH
                if (g_sftp.visible) { needs_render = true; break; }
                if (g_pf_overlay.visible) { needs_render = true; break; }
#endif
                int r, c;
                pixel_to_cell(&term, ev.button.x, ev.button.y, 2, 2, &r, &c);
                if (term.mouse_report && !g_menu.visible) {
                    int btn = (ev.button.button == SDL_BUTTON_LEFT)   ? 0 :
                              (ev.button.button == SDL_BUTTON_MIDDLE) ? 1 :
                              (ev.button.button == SDL_BUTTON_RIGHT)  ? 2 : -1;
                    if (btn >= 0) {
                        char seq[32]; int slen;
                        if (term.mouse_sgr)
                            slen = snprintf(seq,sizeof(seq),"\x1b[<%d;%d;%dm",btn,c+1,r+1);
                        else
                            slen = snprintf(seq,sizeof(seq),"\x1b[M%c%c%c",
                                            (char)(32+3),(char)(32+c+1),(char)(32+r+1));
                        TERM_WRITE(seq, slen);
                    }
                } else if (ev.button.button == SDL_BUTTON_LEFT && term.sel_active) {
                    if (g_menu_copy_pending) {
                        g_menu_copy_pending = false;
                    } else {
                        pixel_to_cell(&term, ev.button.x, ev.button.y, 2, 2,
                                      &term.sel_end_row, &term.sel_end_col);
                        term.sel_active = false;
                        bool same = (term.sel_start_row == term.sel_end_row &&
                                     term.sel_start_col == term.sel_end_col);
                        term.sel_exists = !same;
                        if (term.sel_exists) term_copy_selection(&term);
                        term_dirty_all(&term);
                    }
                } else if (ev.button.button == SDL_BUTTON_LEFT && term.sel_exists) {
                    // Double-click word selection already set - just copy and keep selection
                    if (g_menu_copy_pending) {
                        g_menu_copy_pending = false;
                    } else {
                        term_copy_selection(&term);
                        term_dirty_all(&term);
                    }
                }
                break;
            }

            case SDL_MOUSEWHEEL: {
                if (g_iv.visible) {
                    SDL_GetWindowSize(window, &win_w, &win_h);
                    iv_mousewheel(ev.wheel.mouseX, ev.wheel.mouseY,
                                  ev.wheel.y, win_w, win_h);
                    needs_render = true;
                    break;
                }
                if (g_ssh_key_mgr.visible) {
                    ssh_key_mgr_scroll(ev.wheel.y);
                    needs_render = true;
                    break;
                }
#ifdef USESSH
                if (g_sftp_console_visible) {
                    sftp_console_scroll(ev.wheel.y);
                    needs_render = true;
                    break;
                }
                if (g_sftp.visible) {
                    SDL_GetWindowSize(window, &win_w, &win_h);
                    sftp_overlay_mousewheel(ev.wheel.mouseX, ev.wheel.mouseY,
                                            ev.wheel.y, win_w);
                    needs_render = true;
                    break;
                }
                if (g_pf_overlay.visible) { needs_render = true; break; }
#endif
                if (g_wopr.visible) {
                    if (wopr_mousewheel(ev.wheel.y))
                        needs_render = true;
                    break;
                }
                if (g_debuglog_visible) {
                    debuglog_scroll(ev.wheel.y);
                    needs_render = true;
                    break;
                }
                SDL_Keymod mod = SDL_GetModState();
                if (mod & KMOD_CTRL) {
                    int delta = (ev.wheel.y > 0) ? 1 : -1;
                    if (mod & KMOD_SHIFT) delta *= 4;
                    SDL_GetWindowSize(window, &win_w, &win_h);
                    term_set_font_size(&term, g_font_size + delta, win_w, win_h);
                    ft_invalidate_glyph_cache();
                    fbo_needs_clear = true;
                    G.proj = mat4_ortho(0, (float)win_w, (float)win_h, 0, -1, 1);
                    settings_save();
                } else if (term.mouse_report) {
                    int r2, c2;
                    pixel_to_cell(&term, ev.motion.x, ev.motion.y, 2, 2, &r2, &c2);
                    int btn = (ev.wheel.y > 0) ? 64 : 65;
                    char seq[32]; int slen;
                    if (term.mouse_sgr)
                        slen = snprintf(seq,sizeof(seq),"\x1b[<%d;%d;%dM",btn,c2+1,r2+1);
                    else
                        slen = snprintf(seq,sizeof(seq),"\x1b[M%c%c%c",
                                        (char)(32+btn),(char)(32+c2+1),(char)(32+r2+1));
                    TERM_WRITE(seq, slen);
                } else {
                    int delta = (ev.wheel.y > 0) ? 3 : -3;
                    int new_off = SDL_clamp(term.sb_offset + delta, 0, term.sb_count);
                    if (new_off != term.sb_offset) {
                        term.sb_offset = new_off;
                        term_dirty_all(&term);
                    }
                }
                break;
            }

            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_RESIZED ||
                    ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    SDL_GetWindowSize(window, &win_w, &win_h);
                    if (!g_use_sdl_renderer) glViewport(0, 0, win_w, win_h);
                    G.proj = mat4_ortho(0, (float)win_w, (float)win_h, 0, -1, 1);
                    gl_resize_fbo(win_w, win_h);
                    term_resize(&term, win_w, win_h);
                    g_basic_win_w = win_w;
                    g_basic_win_h = win_h;
                    basic_graphics_init(win_w, win_h);  // update canvas mapping
                    fbo_needs_clear = true;
#ifdef USESSH
                    if (use_ssh) ssh_pty_resize(term.cols, term.rows);
#endif
                    if (use_telnet && telnet_phase == TelnetPhase::ACTIVE)
                        telnet_pty_resize(term.cols, term.rows);
                }
                break;
            }
        }

        // Animated render modes (CRT flicker, VHS noise) need continuous redraw
        if (g_render_mode & (RENDER_BIT_CRT | RENDER_BIT_VHS | RENDER_BIT_C64 | RENDER_BIT_COMPOSITE | RENDER_BIT_GHOSTING))
            needs_render = true;

        // Notify audio of mode state
        static uint32_t s_prev_render_mode = ~0u;
        if (g_render_mode != s_prev_render_mode) {
            term_audio_set_mode(g_render_mode);
            s_prev_render_mode = g_render_mode;
        }

        // Fight simulation tick every frame
        fight_tick((float)win_w, (float)win_h);
        if (fight_get_enabled()) needs_render = true;

        // Bouncing circle tick every frame
        static uint32_t s_last_bc_ticks = 0;
        uint32_t cur_ticks = SDL_GetTicks();
        float bc_dt = s_last_bc_ticks ? (float)(cur_ticks - s_last_bc_ticks) / 1000.f : 0.016f;
        s_last_bc_ticks = cur_ticks;
        bc_tick((float)win_w, (float)win_h, bc_dt);
        if (bc_get_enabled()) needs_render = true;

        // WOPR overlay tick — cap screen updates at 20 fps to simulate an old monitor
        if (g_wopr.visible) {
            wopr_update(dt);
            static uint32_t s_wopr_last_render = 0;
            if (now - s_wopr_last_render >= 50) {
                s_wopr_last_render = now;
                needs_render = true;
            }
        }

        // Felix Chirp tick — always runs when visible, outside the needs_render gate
        if (g_iv.visible) {
            iv_tick((double)bc_dt);
            if (g_iv.audio_playing || g_iv.audio_paused || g_iv.cdg_display)
                needs_render = true;
        }

        // Hard cap at ~60 fps for animated/fight modes that don't vsync themselves.
        // Sleep is placed AFTER rendering so input→render has no artificial delay.
        // (vsync via SDL_GL_SetSwapInterval(1) already throttles normal frames.)

        // Wake up the render loop if BASIC has queued commands or has persistent content.
        // The extra needs_render when s_basic_has_content is true ensures SDL's double
        // buffer always gets the composited frame presented on both back and front buffers.
        if (s_dl_dirty || s_basic_has_content) needs_render = true;
        // Synchronized output: keep polling while it's held so the frame
        // appears as soon as the app releases it (or the 200 ms timeout hits).
        if (term.sync_output) needs_render = true;
        // Keep redrawing while a toast is fading in/out
        if (toast_active()) needs_render = true;

        if (needs_render) {
            // Re-render the terminal FBO if rows are dirty, BASIC has commands,
            // or BASIC is active (s_basic_has_content forces needs_render every
            // frame for double-buffer correctness — term must also redraw every
            // frame so both buffers get the composited result, preventing flash).
            bool any_dirty = term.all_dirty || s_dl_dirty || s_basic_has_content;
            if (!any_dirty) {
                for (int r = 0; r < term.rows && !any_dirty; r++)
                    any_dirty = term.dirty_rows[r] != 0;
            }
            // App is mid-frame (?2026h): keep showing the previous frame; the
            // dirty rows stay dirty and get drawn once the frame is complete.
            if (any_dirty && !s_basic_has_content && term_sync_active(&term))
                any_dirty = false;

            if (any_dirty) {
                float bg_r = THEMES[g_theme_idx].bg_r;
                float bg_g = THEMES[g_theme_idx].bg_g;
                float bg_b = THEMES[g_theme_idx].bg_b;
                basic_render(win_w, win_h); // updates s_basic_has_content for this frame
                // When BASIC is active we clear the term FBO transparent every frame
                // and must redraw ALL rows — not just dirty ones — so the cleared
                // FBO gets fully repopulated before compositing.
                if (s_basic_has_content) term_dirty_all(&term);
                if (fbo_needs_clear || s_basic_has_content) {
                    gl_clear_term_frame(win_w, win_h, bg_r, bg_g, bg_b);
                    fbo_needs_clear = false;
                }
                gl_begin_term_frame(win_w, win_h, bg_r, bg_g, bg_b);
                term_render(&term, 2, 2);   // terminal text on top
                gl_end_term_frame();
            }

            gl_update_ghost(win_w, win_h);

            // Composite: blit cached terminal into post-process FBO, overlay fight figures
            gl_begin_frame();
            if (!g_use_sdl_renderer) glViewport(0, 0, win_w, win_h);
            if (fight_get_enabled()) {
                fight_render((float)win_w, (float)win_h);
            }
            if (bc_get_enabled()) {
                bc_render((float)win_w, (float)win_h);
            }
            float t_sec = (float)(SDL_GetTicks()) / 1000.0f;
            gl_end_frame(t_sec, win_w, win_h);

            // Menu renders after post-process so it's never distorted
            if (!g_use_sdl_renderer) glViewport(0, 0, win_w, win_h);
            menu_render(&g_menu);
#ifdef USESSH
            sftp_overlay_render(win_w, win_h);
            sftp_console_render(win_w, win_h);
            pf_overlay_render(win_w, win_h);
            ssh_key_mgr_render(win_w, win_h);
#endif
            // Image viewer (F5) — full-screen overlay
            iv_render(win_w, win_h);
            // Custom shell dialog
            custom_shell_dialog_render(win_w, win_h);
            // Help overlay renders last (topmost layer)
            help_render(win_w, win_h);
            debuglog_render(win_w, win_h);
            toast_render(win_w, win_h);
            // WOPR overlay — above everything including help
            if (g_wopr.visible) wopr_render(win_w, win_h);

            // Flush any glyph/vert geometry queued by overlay renderers
            // (menus, help, WOPR, etc.) before presenting the frame.
            // Must be called with framebuffer 0 bound (gl_end_frame leaves it there).
            if (!g_use_sdl_renderer) {
                glUseProgram(G.prog);
                gl_flush_glyphs();
                gl_flush_verts();
                glUseProgram(0);
            }

            if (g_use_sdl_renderer)
                SDL_RenderPresent(g_sdl_renderer);
            else
                SDL_GL_SwapWindow(window);

            // Cap idle/animated frames at ~60 fps after swap to avoid busy-looping.
            // Input frames are already throttled by vsync above; this only bites
            // when needs_render was forced by fight/CRT/bouncing modes.
            uint32_t elapsed = SDL_GetTicks() - now;
            if (elapsed < 16) SDL_Delay(16 - elapsed);
        } else {
            // Nothing to render this iteration — sleep briefly to yield the CPU
            // rather than spinning at full speed waiting for PTY data.
            SDL_Delay(4);
        }
    }

    SDL_FreeCursor(cursor_hand);
    SDL_FreeCursor(cursor_ibeam);
    if (!g_use_sdl_renderer) {
        SDL_GL_DeleteContext(ctx);
    } else {
        if (g_sdl_renderer) SDL_DestroyRenderer(g_sdl_renderer);
    }
    SDL_DestroyWindow(window);
    wopr_close();          // join/detach any running sub-game thread before audio shutdown
    wopr_audio_shutdown();
    crt_audio_shutdown();
    kitty_shutdown();
    sixel_shutdown();
    basic_graphics_shutdown();
    menu_font_shutdown();
    term_free(&term);      // Clean up terminal buffers (fixes memory leaks)
#ifdef USESSH
    if (use_ssh) {
        ssh_abort.store(true);
        if (ssh_thread.joinable()) ssh_thread.join();
        if (prompt_req.mtx) SDL_DestroyMutex(prompt_req.mtx);
        sftp_console_join();
        sftp_transfer_join();
        sftp_webserver_stop();  // Stop web server before shutting down SFTP
        sftp_shutdown();
        pf_shutdown_all();
        ssh_disconnect();
    }
#endif
    if (use_telnet) telnet_disconnect();
    if (use_serial) serial_disconnect();
    SDL_Quit();
    ft_shutdown();

    return 0;
}
