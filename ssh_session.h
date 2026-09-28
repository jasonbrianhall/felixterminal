#pragma once

#ifdef USESSH
#include <libssh2.h>
#endif

// ============================================================================
// SSH SESSION  — libssh2-backed remote shell, compiled only when USESSH is
// defined.  Mirrors the term_pty read/write interface so the main loop needs
// only minimal #ifdef guards.
//
// Build addition (when USESSH is defined):
//   g++ ... ssh_session.cpp ... -lssh2 -lcrypto -lssl -DUSESSH -o gl_terminal
//
// Typical usage:
//   SshConfig cfg;
//   cfg.host = "myserver.example.com";
//   cfg.port = 22;
//   cfg.user = "alice";
//   cfg.key_path = "/home/alice/.ssh/id_ed25519";  // "" = try agent then password
//   cfg.password = "";
//   cfg.command = "";  // "" = interactive shell, else specify command like "/bin/sh"
//   cfg.remote_forwards.push_back("8080:localhost:3000");  // optional -R forwarding
//
//   if (!ssh_connect(cfg, &term))  // blocks until authenticated
//       return 1;
//
//   // main loop — drop-in replacements for term_read / term_write:
//   if (ssh_read(&term))   needs_render = true;
//   ssh_write(&term, buf, n);
//
//   // resize after window resize:
//   ssh_pty_resize(term.cols, term.rows);
//
//   // shutdown:
//   ssh_disconnect();
// ============================================================================

#ifdef USESSH

#include "terminal.h"
#include <string>
#include <functional>

struct SshConfig {
    std::string host;           // real host to connect to (after ~/.ssh/config HostName)
    int         port        = 22;
    std::string user;
    std::string alias;          // name as typed when ~/.ssh/config HostName replaced it ("" otherwise)
    // Authentication: tried in order —
    //   1. Explicit key_path (command-line -i flag)
    //   2. config_key_paths (IdentityFile entries from ~/.ssh/config)
    //   3. Default key paths (id_ed25519, id_rsa, id_dsa, id_ecdsa)
    //   4. SSH agent
    //   5. password (if non-empty, or prompt_password is called)
    std::string key_path;       // "" = try config then defaults
    std::string key_path_pub;   // "" = derive from key_path
    std::vector<std::string> config_key_paths;  // IdentityFile entries from ~/.ssh/config
    std::string password;       // "" = skip password auth (use prompt_password)
    // Optional: known-hosts file for host-key verification.
    // Set to "" to skip verification (INSECURE — use only on trusted networks).
    std::string known_hosts_path;  // default: ~/.ssh/known_hosts
    // Called when no password was supplied and agent/key auth both failed.
    // Receives the prompt string (e.g. "user@host's password: ") and must
    // return the password entered by the user, or "" to abort.
    // If nullptr, password auth is skipped when no password is supplied.
    std::function<std::string(const char *prompt)> prompt_password;
    // Called when the server's host key is not found in known_hosts (i.e.
    // first time connecting to this host). Receives the full confirmation
    // prompt text (mirrors OpenSSH's "authenticity of host" prompt) and must
    // return the user's typed response. "yes" trusts the key and persists it
    // to known_hosts_path; anything else aborts the connection.
    // If nullptr, unknown host keys are rejected outright (fails closed).
    std::function<std::string(const char *prompt)> prompt_host_key;
    bool x11_forward = true;  // forward $DISPLAY to the remote session
    
    // Command to execute on the remote server.
    // If empty (default), an interactive shell is started.
    // Examples: "/bin/bash", "/bin/sh", "python3", "echo hello"
    std::string command;
    
    // Remote port forwarding (ssh -R syntax)
    // Format: "listen_port:bind_address:forward_port"
    // Example: "8080:localhost:3000"
    // The remote SSH server listens on listen_port and forwards
    // connections to your local machine's bind_address:forward_port.
    std::vector<std::string> remote_forwards;
    
    // Local port forwarding (ssh -L syntax)
    // Format: "listen_port:remote_host:remote_port"
    // Example: "3306:db.internal:3306"
    std::vector<std::string> local_forwards;
};

// Connect, authenticate, and open a PTY channel.
// Blocks until fully connected or failure.  On success the terminal's
// dimensions are sent to the remote side.
// Returns true on success; on failure prints a message via SDL_Log and returns false.
bool ssh_connect(const SshConfig &cfg, Terminal *t);

// Parse ~/.ssh/config and apply settings to cfg for the given alias/hostname.
// Merges config file settings with existing cfg values (command-line takes precedence).
// Returns true if config was found and parsed, false if file doesn't exist or parse failed.
bool ssh_config_load(const char *alias, SshConfig &cfg);

// Why the last ssh_connect() failed, worded for the user (e.g. a changed host
// key warning). Empty if there's nothing more specific than "failed".
std::string ssh_last_user_error();

// Non-blocking read from the SSH channel into the terminal parser.
// Returns true if any data was received (caller should set needs_render).
bool ssh_read(Terminal *t);

// Write n bytes to the SSH channel.
void ssh_write(Terminal *t, const char *buf, int n);

// Send a PTY resize request to the remote side.
void ssh_pty_resize(int cols, int rows);

// Returns true if the remote channel has been closed / EOF'd.
bool ssh_channel_closed();

// Close channel and free libssh2 resources.
void ssh_disconnect();

#include <libssh2.h>

// Returns true if an SSH session is currently active.
bool ssh_active();

// Returns true if the current channel has an allocated PTY.
// False for servers that refuse PTY allocation (e.g. git@github.com) —
// callers can use this to skip resize calls / hide PTY-only UI.
bool ssh_has_pty();

// Returns the raw libssh2 session pointer (used by sftp_overlay).
LIBSSH2_SESSION *ssh_get_session();
int              ssh_get_socket();

// Session mutex — must be held for the duration of any libssh2 call made
// outside ssh_session.cpp (e.g. SFTP transfers on a background thread).
void ssh_session_lock();
void ssh_session_unlock();

// Reset SSH session state after fork().
// Call in child process before any SSH/SFTP operations.
// Cleans up inherited libssh2 pointers and reinitializes for the child.
void ssh_reset_after_fork();

// Get remote forwards specs configured at connection time
// Format: "listen_port:bind_addr:forward_port"
const std::vector<std::string>& ssh_get_remote_forwards();

#endif // USESSH
