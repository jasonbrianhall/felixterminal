// ssh_session.cpp — libssh2 SSH session for gl_terminal
// Compiled only when USESSH is defined.

#ifdef USESSH

#include "ssh_session.h"
#include "terminal.h"
#include "term_pty.h"   // term_feed, g_term_write_override

#include <libssh2.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ec.h>
#include <openssl/objects.h>

#include <SDL2/SDL.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

#ifndef _WIN32
#  include <sys/un.h>
#  include <pwd.h>
#else
#  include <direct.h>   // _mkdir
#endif

#ifndef _WIN32
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <netdb.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#else
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "Ws2_32.lib")
#endif

// ============================================================================
// STATE
// ============================================================================

static LIBSSH2_SESSION *s_session  = nullptr;
static LIBSSH2_CHANNEL *s_channel  = nullptr;
static int              s_sock     = -1;   // TCP socket fd
static bool             s_active   = false;
static bool             s_have_pty = false;
static std::vector<std::string> s_remote_forwards;  // Store remote forwards to register later
static Terminal        *s_term     = nullptr;  // for cell pixel size on resize

// All libssh2 calls on this session must hold this mutex.
// The SFTP transfer thread acquires it for the duration of a transfer;
// the main thread holds it during ssh_read / ssh_write.
// Must be recursive: ssh_read holds the lock, calls term_feed, which calls
// term_write (for DA responses etc.), which calls ssh_write — same thread,
// needs to re-enter. std::mutex would deadlock; recursive_mutex allows it.
static std::recursive_mutex s_session_mutex;

// ============================================================================
// HELPERS
// ============================================================================

static std::string last_ssh2_error(const char *context) {
    char *msg = nullptr;
    int   len = 0;
    if (s_session)
        libssh2_session_last_error(s_session, &msg, &len, 0);
    char buf[512];
    if (msg && len > 0)
        snprintf(buf, sizeof(buf), "[SSH] %s: %.*s", context, len, msg);
    else
        snprintf(buf, sizeof(buf), "[SSH] %s: unknown error", context);
    return buf;
}

// Resolve hostname and open a non-blocking TCP socket.
// Returns a valid socket fd or -1 on failure.
static int tcp_connect(const std::string &host, int port) {
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0) {
        SDL_Log("[SSH] getaddrinfo failed for host '%s'\n", host.c_str());
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *r = res; r; r = r->ai_next) {
        fd = (int)socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (fd < 0) continue;

        if (connect(fd, r->ai_addr, (int)r->ai_addrlen) == 0)
            break;

#ifndef _WIN32
        close(fd);
#else
        closesocket(fd);
#endif
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        SDL_Log("[SSH] connect to %s:%d failed\n", host.c_str(), port);
        return -1;
    }

    // Set non-blocking
#ifndef _WIN32
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#else
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
#endif

    return fd;
}

// ============================================================================
// HOME DIRECTORY + USER-FACING ERRORS
// ============================================================================

// The user's home directory ("" if it truly can't be determined).
// Windows: %USERPROFILE% (same place Windows' own OpenSSH client uses).
static std::string ssh_home_dir() {
#ifdef _WIN32
    const char *p = getenv("USERPROFILE");
    if (p && *p) return p;
    const char *d = getenv("HOMEDRIVE"), *h = getenv("HOMEPATH");
    if (d && *d && h && *h) return std::string(d) + h;
    return "";
#else
    const char *h = getenv("HOME");
    if (h && *h) return h;
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir && *pw->pw_dir) return pw->pw_dir;
    return "";
#endif
}

static std::string ssh_dir_path(const char *file) {
    std::string home = ssh_home_dir();
    if (home.empty()) return "";
#ifdef _WIN32
    return home + "\\.ssh\\" + file;
#else
    return home + "/.ssh/" + file;
#endif
}

// Reason for the last connection failure, worded for the user; the UI shows
// it above "Connection failed". Set from the connect thread, read by the UI.
static std::mutex  s_user_error_mtx;
static std::string s_user_error;
static void set_user_error(const std::string &msg) {
    std::lock_guard<std::mutex> lock(s_user_error_mtx);
    s_user_error = msg;
}
std::string ssh_last_user_error() {
    std::lock_guard<std::mutex> lock(s_user_error_mtx);
    return s_user_error;
}

// Build default SSH key paths to try (~/.ssh/id_rsa, id_ed25519, etc.)
static std::vector<std::string> get_default_key_paths() {
    std::vector<std::string> keys;
    
    std::string home_str = ssh_home_dir();
    if (home_str.empty()) return keys;
    const char *home = home_str.c_str();
    
    std::string ssh_dir = home;
    ssh_dir += "/.ssh";
    
    // Try common key names in order of preference (OpenSSH order)
    const char *key_names[] = { "id_ed25519_sk", "id_ed25519", "id_ecdsa_sk", "id_ecdsa", "id_dsa", "id_rsa" };
    for (size_t i = 0; i < sizeof(key_names) / sizeof(key_names[0]); i++) {
        std::string path = ssh_dir + "/" + key_names[i];
        
        // Check if file exists and is readable
        FILE *f = fopen(path.c_str(), "r");
        if (f) {
            fclose(f);
            keys.push_back(path);
        }
    }
    
    return keys;
}

// Parse ~/.ssh/config for a given host alias
// Supports: Host, HostName, User, Port, IdentityFile, ProxyCommand, etc.
// OpenSSH-style glob: '*' and '?', case-insensitive (host names are)
static bool ssh_glob_match(const char *pat, const char *str) {
    while (*pat) {
        if (*pat == '*') {
            while (*pat == '*') pat++;
            if (!*pat) return true;
            for (; *str; str++)
                if (ssh_glob_match(pat, str)) return true;
            return false;
        }
        if (!*str) return false;
        if (*pat != '?' && tolower((unsigned char)*pat) != tolower((unsigned char)*str))
            return false;
        pat++; str++;
    }
    return *str == '\0';
}

// A "Host" line: whitespace-separated patterns; a match on any positive
// pattern selects the block, a match on any !negated pattern rejects it.
static bool ssh_host_line_matches(const std::string &patterns, const char *host) {
    bool matched = false;
    size_t i = 0, n = patterns.size();
    while (i < n) {
        while (i < n && isspace((unsigned char)patterns[i])) i++;
        size_t j = i;
        while (j < n && !isspace((unsigned char)patterns[j])) j++;
        if (j > i) {
            std::string pat = patterns.substr(i, j - i);
            bool neg = pat[0] == '!';
            if (neg) pat.erase(0, 1);
            if (ssh_glob_match(pat.c_str(), host)) {
                if (neg) return false;
                matched = true;
            }
        }
        i = j;
    }
    return matched;
}

// Parse ~/.ssh/config the way OpenSSH does: every block whose Host patterns
// match the name as typed is applied, top to bottom, and the FIRST value found
// for each setting wins (so put specific hosts above "Host *" defaults).
// Values given on the command line win over the file. Supported: HostName
// (with %h), User, Port, IdentityFile (accumulates, ~ expanded). Match and
// Include blocks are skipped.
bool ssh_config_load(const char *alias, SshConfig &cfg) {
    std::string home = ssh_home_dir();
    std::string config_path = ssh_dir_path("config");
    if (config_path.empty()) return false;

    FILE *f = fopen(config_path.c_str(), "r");
    if (!f) {
        SDL_Log("[SSH] no ~/.ssh/config found\n");
        return false;
    }

    const std::string name = alias ? alias : "";
    bool set_host = false;
    bool set_user = !cfg.user.empty();
    bool set_port = cfg.port != 22;     // a non-default port came from the command line
    bool in_block = true;               // lines before the first Host apply to everyone
    bool found_host = false;
    char line[1024];

    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') continue;
        size_t len = strlen(p);
        while (len > 0 && isspace((unsigned char)p[len - 1])) p[--len] = 0;

        // Key, then whitespace and/or '=', then value
        char *k_end = p;
        while (*k_end && !isspace((unsigned char)*k_end) && *k_end != '=') k_end++;
        std::string key(p, k_end - p);
        char *v = k_end;
        while (*v && isspace((unsigned char)*v)) v++;
        if (*v == '=') { v++; while (*v && isspace((unsigned char)*v)) v++; }
        std::string value = v;
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        for (char &c : key) c = (char)tolower((unsigned char)c);

        if (key == "host") {
            in_block = ssh_host_line_matches(value, name.c_str());
            if (in_block) found_host = true;
            continue;
        }
        if (key == "match") {
            SDL_Log("[SSH] ~/.ssh/config: 'Match' blocks are not supported, skipping\n");
            in_block = false;
            continue;
        }
        if (key == "include") {
            SDL_Log("[SSH] ~/.ssh/config: 'Include' is not supported, skipping '%s'\n", value.c_str());
            continue;
        }
        if (!in_block || value.empty()) continue;

        if (key == "hostname" && !set_host) {
            std::string h;
            for (size_t i = 0; i < value.size(); i++) {      // %h = the name as typed
                if (value[i] == '%' && i + 1 < value.size() && value[i + 1] == 'h') { h += name; i++; }
                else if (value[i] == '%' && i + 1 < value.size() && value[i + 1] == '%') { h += '%'; i++; }
                else h += value[i];
            }
            if (!h.empty() && h != cfg.host) {
                if (cfg.alias.empty()) cfg.alias = name;     // keep what the user typed
                cfg.host = h;
            }
            set_host = true;
        } else if (key == "user" && !set_user) {
            cfg.user = value;
            set_user = true;
        } else if (key == "port" && !set_port) {
            int port = atoi(value.c_str());
            if (port > 0 && port <= 65535) cfg.port = port;
            set_port = true;
        } else if (key == "identityfile") {
            std::string path = value;
            if (!path.empty() && path[0] == '~' && !home.empty())
                path = home + path.substr(1);
            bool dup = false;
            for (const std::string &existing : cfg.config_key_paths) dup |= (existing == path);
            if (!dup) cfg.config_key_paths.push_back(path);
        }
    }
    fclose(f);

    if (found_host) {
        SDL_Log("[SSH] loaded config for host '%s': hostname=%s user=%s port=%d\n",
                name.c_str(), cfg.host.c_str(), cfg.user.c_str(), cfg.port);
        return true;
    }
    SDL_Log("[SSH] host '%s' not found in ~/.ssh/config\n", name.c_str());
    return false;
}

// Attempt authentication via ssh-agent.
static bool auth_agent(const std::string &user) {
    LIBSSH2_AGENT *agent = libssh2_agent_init(s_session);
    if (!agent) return false;

    if (libssh2_agent_connect(agent) != 0) {
        libssh2_agent_free(agent);
        return false;
    }
    if (libssh2_agent_list_identities(agent) != 0) {
        libssh2_agent_disconnect(agent);
        libssh2_agent_free(agent);
        return false;
    }

    struct libssh2_agent_publickey *identity = nullptr, *prev = nullptr;
    bool ok = false;
    while (libssh2_agent_get_identity(agent, &identity, prev) == 0) {
        int rc;
        while ((rc = libssh2_agent_userauth(agent, user.c_str(), identity)) ==
               LIBSSH2_ERROR_EAGAIN)
            SDL_Delay(5);
        if (rc == 0) {
            SDL_Log("[SSH] authenticated via ssh-agent identity '%s'\n",
                    identity->comment ? identity->comment : "(unnamed)");
            ok = true;
            break;
        }
        prev = identity;
    }

    libssh2_agent_disconnect(agent);
    libssh2_agent_free(agent);
    return ok;
}

// Decrypts priv_path with passphrase via OpenSSL and writes an OpenSSH-format
// .pub next to it. Only succeeds if the passphrase is actually correct.
static bool generate_pubkey_file(const std::string &priv_path, const std::string &passphrase,
                                  std::string &out_pub_path) {
    FILE *fp = fopen(priv_path.c_str(), "r");
    if (!fp) return false;
    EVP_PKEY *pkey = PEM_read_PrivateKey(fp, nullptr, nullptr,
                                          (void *)(passphrase.empty() ? nullptr : passphrase.c_str()));
    fclose(fp);
    if (!pkey) return false;

    std::string blob;
    auto put_u32 = [&](uint32_t v) {
        char b[4] = { (char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v };
        blob.append(b, 4);
    };
    auto put_str = [&](const std::string &s) { put_u32((uint32_t)s.size()); blob += s; };
    auto put_mpint = [&](const BIGNUM *bn) {
        int len = BN_num_bytes(bn);
        std::vector<unsigned char> buf(len);
        BN_bn2bin(bn, buf.data());
        bool pad = len > 0 && (buf[0] & 0x80);
        put_u32((uint32_t)(len + (pad ? 1 : 0)));
        if (pad) blob += '\0';
        blob.append((char *)buf.data(), len);
    };

    std::string type_name;
    int keytype = EVP_PKEY_id(pkey);
    if (keytype == EVP_PKEY_RSA) {
        type_name = "ssh-rsa";
        const RSA *rsa = EVP_PKEY_get0_RSA(pkey);
        const BIGNUM *n = nullptr, *e = nullptr;
        RSA_get0_key(rsa, &n, &e, nullptr);
        put_str(type_name);
        put_mpint(e);
        put_mpint(n);
    } else if (keytype == EVP_PKEY_ED25519) {
        type_name = "ssh-ed25519";
        unsigned char raw[32];
        size_t raw_len = sizeof(raw);
        if (EVP_PKEY_get_raw_public_key(pkey, raw, &raw_len) != 1) {
            EVP_PKEY_free(pkey);
            return false;
        }
        put_str(type_name);
        put_str(std::string((char *)raw, raw_len));
    } else if (keytype == EVP_PKEY_EC) {
        const EC_KEY *ec = EVP_PKEY_get0_EC_KEY(pkey);
        const EC_GROUP *grp = EC_KEY_get0_group(ec);
        int nid = EC_GROUP_get_curve_name(grp);
        const char *curve = nid == NID_X9_62_prime256v1 ? "nistp256"
                           : nid == NID_secp384r1        ? "nistp384"
                           : nid == NID_secp521r1         ? "nistp521" : nullptr;
        if (!curve) { EVP_PKEY_free(pkey); return false; }
        type_name = std::string("ecdsa-sha2-") + curve;
        const EC_POINT *pt = EC_KEY_get0_public_key(ec);
        unsigned char pub[256];
        size_t pub_len = EC_POINT_point2oct(grp, pt, POINT_CONVERSION_UNCOMPRESSED,
                                             pub, sizeof(pub), nullptr);
        put_str(type_name);
        put_str(curve);
        put_str(std::string((char *)pub, pub_len));
    } else {
        EVP_PKEY_free(pkey);
        return false;
    }
    EVP_PKEY_free(pkey);

    std::vector<unsigned char> b64(4 * ((blob.size() + 2) / 3) + 1);
    int outlen = EVP_EncodeBlock(b64.data(), (const unsigned char *)blob.data(), (int)blob.size());

    std::string pub_path = priv_path + ".pub";
    FILE *out = fopen(pub_path.c_str(), "w");
    if (!out) return false;
    fprintf(out, "%s %.*s\n", type_name.c_str(), outlen, (char *)b64.data());
    fclose(out);
    chmod(pub_path.c_str(), 0644);

    out_pub_path = pub_path;
    return true;
}

// Attempt public-key file authentication.
static bool auth_key(const std::string &user,
                     const std::string &priv_path,
                     const std::string &pub_path_in,
                     const std::string &passphrase) {
    std::string pub_path = pub_path_in.empty() ? priv_path + ".pub" : pub_path_in;
    bool have_pub = access(pub_path.c_str(), R_OK) == 0;

    if (access(priv_path.c_str(), R_OK) != 0)
        SDL_Log("[SSH] key auth: cannot read private key '%s': %s\n",
                priv_path.c_str(), strerror(errno));

    if (!have_pub) {
        std::string generated;
        if (generate_pubkey_file(priv_path, passphrase, generated)) {
            SDL_Log("[SSH] key auth: generated public key '%s'\n", generated.c_str());
            pub_path = generated;
            have_pub = true;
        } else {
            SDL_Log("[SSH] key auth: no public key at '%s', deriving from private key\n",
                    pub_path.c_str());
        }
    }

    SDL_Log("[SSH] attempting key auth: user='%s' pub='%s' priv='%s'\n",
            user.c_str(), have_pub ? pub_path.c_str() : "(derived)", priv_path.c_str());

    int rc;
    while ((rc = libssh2_userauth_publickey_fromfile(
                s_session,
                user.c_str(),
                have_pub ? pub_path.c_str() : nullptr,
                priv_path.c_str(),
                passphrase.empty() ? nullptr : passphrase.c_str())) ==
           LIBSSH2_ERROR_EAGAIN)
        SDL_Delay(5);

    if (rc == 0) {
        SDL_Log("[SSH] authenticated via key '%s'\n", priv_path.c_str());
        return true;
    }
    SDL_Log("[SSH] key auth failed (rc=%d): %s\n", rc, last_ssh2_error("key auth").c_str());
    return false;
}

// Attempt password authentication.
static bool auth_password(const std::string &user, const std::string &password) {
    int rc;
    while ((rc = libssh2_userauth_password(
                s_session, user.c_str(), password.c_str())) ==
           LIBSSH2_ERROR_EAGAIN)
        SDL_Delay(5);

    if (rc == 0) {
        SDL_Log("[SSH] authenticated via password\n");
        return true;
    }
    SDL_Log("%s\n", last_ssh2_error("password auth failed").c_str());
    return false;
}

// Map libssh2 hostkey type integer to the corresponding LIBSSH2_KNOWNHOST_KEY_*
// flag.  The old code used a ternary that fell back to SSHDSS for anything
// non-RSA, which breaks Ed25519 and ECDSA — the key types preferred by all
// modern OpenSSH servers.
static int hostkey_type_to_knownhost_flag(int key_type) {
    switch (key_type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA:       return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
#ifdef LIBSSH2_HOSTKEY_TYPE_ECDSA_256
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ECDSA_384
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ECDSA_521
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ED25519
    case LIBSSH2_HOSTKEY_TYPE_ED25519:   return LIBSSH2_KNOWNHOST_KEY_ED25519;
#endif
    default:
        SDL_Log("[SSH] WARNING: unknown host key type %d — falling back to DSS flag\n",
                key_type);
        return LIBSSH2_KNOWNHOST_KEY_SSHDSS;
    }
}

// Numeric IP of the connected peer (for the "'host (IP)'" prompt line).
// Returns "" on failure — caller falls back to just the hostname.
static std::string get_peer_ip() {
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (getpeername(s_sock, (struct sockaddr *)&ss, &len) != 0)
        return "";
    char ipstr[INET6_ADDRSTRLEN] = {};
    if (getnameinfo((struct sockaddr *)&ss, len, ipstr, sizeof(ipstr),
                     nullptr, 0, NI_NUMERICHOST) != 0)
        return "";
    return ipstr;
}

// Human-readable key type name for the trust prompt ("RSA", "ED25519", ...).
static const char *hostkey_type_name(int key_type) {
    switch (key_type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA: return "RSA";
#ifdef LIBSSH2_HOSTKEY_TYPE_ECDSA_256
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return "ECDSA";
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ECDSA_384
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return "ECDSA";
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ECDSA_521
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return "ECDSA";
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ED25519
    case LIBSSH2_HOSTKEY_TYPE_ED25519: return "ED25519";
#endif
    default: return "DSA";
    }
}

// OpenSSH-style "SHA256:base64" fingerprint of the server's host key.
static std::string hostkey_fingerprint() {
    const char *hash = libssh2_hostkey_hash(s_session, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (!hash) return "";
    unsigned char b64[64] = {};
    int outlen = EVP_EncodeBlock(b64, (const unsigned char *)hash, 32);
    std::string s((char *)b64, outlen);
    while (!s.empty() && s.back() == '=')  // OpenSSH fingerprints omit padding
        s.pop_back();
    return "SHA256:" + s;
}

// Verify the server's host key against the known_hosts file.
// Returns true if the key is trusted, false if it should be rejected.
// Fails closed: if known_hosts can't be located, the connection is refused.
// An unknown key is shown to the user (OpenSSH-style prompt) and, if accepted
// with "yes" or by pasting the fingerprint, APPENDED to known_hosts — the
// file is never rewritten, so entries libssh2 can't parse (@revoked,
// @cert-authority, newer key types, comments) are left untouched.
static bool verify_host_key(const SshConfig &cfg) {
    const std::string &host = cfg.host;
    int port = cfg.port;
    // OpenSSH records non-default ports as "[host]:port"
    const std::string kh_name = (port == 22) ? host : "[" + host + "]:" + std::to_string(port);

    std::string kh_path = cfg.known_hosts_path.empty() ? ssh_dir_path("known_hosts")
                                                       : cfg.known_hosts_path;
    if (kh_path.empty()) {
        SDL_Log("[SSH] cannot locate known_hosts (no home directory) — refusing to connect\n");
        set_user_error("Can't verify the server's identity: no home directory was found for "
                       "known_hosts.\r\nSet HOME (Linux) or USERPROFILE (Windows), or pass "
                       "--ssh-known-hosts <file>.\r\n");
        return false;
    }

    LIBSSH2_KNOWNHOSTS *kh = libssh2_knownhost_init(s_session);
    if (!kh) {
        SDL_Log("[SSH] knownhost init failed\n");
        return false;
    }

    int readrc = libssh2_knownhost_readfile(kh, kh_path.c_str(), LIBSSH2_KNOWNHOST_FILE_OPENSSH);
    if (readrc < 0)
        SDL_Log("[SSH] no existing known_hosts at '%s' (or unreadable) — treating as empty\n", kh_path.c_str());

    size_t hkey_len = 0;
    int hkey_type = 0;
    const char *hkey = libssh2_session_hostkey(s_session, &hkey_len, &hkey_type);
    if (!hkey) {
        SDL_Log("[SSH] cannot retrieve host key from session\n");
        libssh2_knownhost_free(kh);
        return false;
    }

    int check_flags = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW;
    check_flags |= hostkey_type_to_knownhost_flag(hkey_type);

    struct libssh2_knownhost *store = nullptr;
    int checkrc = libssh2_knownhost_checkp(kh, host.c_str(), port, hkey, hkey_len,
                                            check_flags, &store);

    const std::string fp      = hostkey_fingerprint();
    const char       *keyname = hostkey_type_name(hkey_type);
    char where[64];
    if (port == 22) where[0] = '\0';
    else snprintf(where, sizeof(where), " (port %d)", port);

    bool trusted = false;
    switch (checkrc) {
    case LIBSSH2_KNOWNHOST_CHECK_MATCH:
        SDL_Log("[SSH] host key verified: %s:%d\n", host.c_str(), port);
        trusted = true;
        break;

    case LIBSSH2_KNOWNHOST_CHECK_NOTFOUND: {
        SDL_Log("[SSH] WARNING: host key not in known_hosts: %s:%d\n", host.c_str(), port);
        if (!cfg.prompt_host_key) {
            set_user_error("The server's host key isn't in known_hosts and can't be confirmed "
                           "here.\r\nConnect once interactively to review and accept it.\r\n");
            break;
        }
        std::string ip         = get_peer_ip();
        std::string host_field = ip.empty() ? host : (host + " (" + ip + ")");
        char prompt[1024];
        snprintf(prompt, sizeof(prompt),
                 "The authenticity of host '%s'%s can't be established.\r\n"
                 "%s key fingerprint is: %s\r\n"
                 "Are you sure you want to continue connecting (yes/no/[fingerprint])? ",
                 host_field.c_str(), where, keyname, fp.c_str());

        std::string answer = cfg.prompt_host_key(prompt);
        while (!answer.empty() && isspace((unsigned char)answer.back())) answer.pop_back();
        while (!answer.empty() && isspace((unsigned char)answer.front())) answer.erase(0, 1);
        std::string lower = answer;
        for (char &c : lower) c = (char)tolower((unsigned char)c);

        // "yes", or the exact fingerprint (base64 is case-sensitive, so no lowercasing)
        bool accepted = (lower == "yes") || (!fp.empty() && answer == fp);
        if (!accepted) {
            SDL_Log("[SSH] host key not trusted by user, aborting connection\n");
            set_user_error(lower.rfind("sha256:", 0) == 0
                ? "That fingerprint doesn't match the server's key — connection cancelled.\r\n"
                : "Host key not accepted — connection cancelled.\r\n");
            break;
        }
        trusted = true;

        // Append just this host's line (never rewrite the whole file)
        struct libssh2_knownhost *added = nullptr;
        char kline[8192];
        size_t klen = 0;
        if (libssh2_knownhost_addc(kh, kh_name.c_str(), nullptr, hkey, hkey_len,
                                   nullptr, 0, check_flags, &added) == 0 && added &&
            libssh2_knownhost_writeline(kh, added, kline, sizeof(kline), &klen,
                                        LIBSSH2_KNOWNHOST_FILE_OPENSSH) == 0) {
            std::string entry(kline, klen);
            while (!entry.empty() && (entry.back() == '\n' || entry.back() == '\r')) entry.pop_back();

            size_t slash = kh_path.find_last_of("/\\");
            if (slash != std::string::npos) {                // ~/.ssh may not exist yet
                std::string dir = kh_path.substr(0, slash);
#ifdef _WIN32
                _mkdir(dir.c_str());
#else
                mkdir(dir.c_str(), 0700);
#endif
            }
            bool needs_newline = false;                      // don't glue onto a last line
            if (FILE *rf = fopen(kh_path.c_str(), "rb")) {
                if (fseek(rf, -1, SEEK_END) == 0) needs_newline = (fgetc(rf) != '\n');
                fclose(rf);
            }
            FILE *af = fopen(kh_path.c_str(), "ab");
            if (af) {
                if (needs_newline) fputc('\n', af);
                fprintf(af, "%s\n", entry.c_str());
                fclose(af);
                SDL_Log("[SSH] host key for '%s' added to %s\n", kh_name.c_str(), kh_path.c_str());
            } else {
                SDL_Log("[SSH] WARNING: could not write host key to '%s'\n", kh_path.c_str());
            }
        } else {
            SDL_Log("[SSH] WARNING: could not format host key entry for '%s'\n", kh_name.c_str());
        }
        break;
    }

    case LIBSSH2_KNOWNHOST_CHECK_MISMATCH: {
        SDL_Log("[SSH] ERROR: host key mismatch (possible MITM): %s:%d\n", host.c_str(), port);
        char msg[2048];
        snprintf(msg, sizeof(msg),
                 "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\r\n"
                 "@    WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!     @\r\n"
                 "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\r\n"
                 "IT IS POSSIBLE THAT SOMEONE IS DOING SOMETHING NASTY!\r\n"
                 "The %s key for '%s'%s does not match the one saved in\r\n"
                 "%s\r\n"
                 "Someone could be intercepting this connection, or the server's\r\n"
                 "key was changed. The server now presents:\r\n"
                 "  %s\r\n"
                 "If you're sure the change is legitimate, remove the old entry for\r\n"
                 "'%s' from that file and connect again.\r\n",
                 keyname, host.c_str(), where, kh_path.c_str(), fp.c_str(), kh_name.c_str());
        set_user_error(msg);
        break;
    }

    case LIBSSH2_KNOWNHOST_CHECK_FAILURE:
    default:
        SDL_Log("[SSH] host key check failure (rc=%d)\n", checkrc);
        set_user_error("The server's host key couldn't be checked — connection refused.\r\n");
        break;
    }

    libssh2_knownhost_free(kh);
    return trusted;
}

#ifndef _WIN32
// ============================================================================
// X11 FORWARDING — always requested; forwards the remote DISPLAY back to the
// local X server, substituting libssh2's fake auth cookie with the real
// local Xauthority cookie so remote GUI apps authenticate correctly.
// ============================================================================

static std::string x11_local_display() {
    const char *d = getenv("DISPLAY");
    return d ? d : ":0";
}

// Connects to the local X server for `display` (unix socket or TCP).
static int x11_local_connect(const std::string &display) {
    std::string d = display;
    size_t dot = d.find_last_of('.');
    if (dot != std::string::npos) d = d.substr(0, dot);
    size_t colon = d.find_last_of(':');
    std::string host = (colon == std::string::npos) ? "" : d.substr(0, colon);
    int num = (colon == std::string::npos) ? 0 : atoi(d.c_str() + colon + 1);

    if (host.empty() || host == "unix") {
        struct sockaddr_un addr = {};
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, sizeof(addr.sun_path), "/tmp/.X11-unix/X%d", num);
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            close(fd);
            return -1;
        }
        return fd;
    }
    return tcp_connect(host, 6000 + num);
}

static bool hex_to_bytes(const std::string &hex, std::vector<unsigned char> &out) {
    out.clear();
    auto hv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        int hi = hv(hex[i]), lo = hv(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back((unsigned char)((hi << 4) | lo));
    }
    return !out.empty();
}

// Real local Xauthority cookie for `display`, via `xauth list`.
static bool x11_local_cookie(const std::string &display, std::vector<unsigned char> &cookie) {
    std::string cmd = "xauth list " + display + " 2>/dev/null";
    FILE *fp = popen(cmd.c_str(), "r");
    if (!fp) return false;
    char line[512] = {};
    bool ok = false;
    if (fgets(line, sizeof(line), fp)) {
        std::string last;
        char *tok = strtok(line, " \t\n");
        while (tok) { last = tok; tok = strtok(nullptr, " \t\n"); }
        ok = hex_to_bytes(last, cookie);
    }
    pclose(fp);
    return ok;
}

// Pumps one forwarded X11 channel to/from the local X server, patching the
// connection-setup packet's auth cookie on the way in.
static void x11_forward_worker(LIBSSH2_CHANNEL *xchan) {
    std::string display = x11_local_display();
    int xfd = x11_local_connect(display);
    if (xfd < 0) {
        SDL_Log("[SSH][X11] could not reach local X server (DISPLAY=%s)\n", display.c_str());
        std::lock_guard<std::recursive_mutex> lock(s_session_mutex);
        libssh2_channel_free(xchan);
        return;
    }

    std::vector<unsigned char> cookie;
    bool have_cookie = x11_local_cookie(display, cookie);

    // Read the connection-setup request (12-byte header + padded proto name
    // + padded auth data) so the fake cookie can be swapped for the real one.
    unsigned char buf[4096];
    int got = 0, total = 12;
    bool header_parsed = false;
    for (;;) {
        ssize_t n;
        {
            std::lock_guard<std::recursive_mutex> lock(s_session_mutex);
            n = libssh2_channel_read(xchan, (char *)buf + got, sizeof(buf) - got);
        }
        if (n > 0) {
            got += (int)n;
            if (!header_parsed && got >= 12) {
                bool msb = (buf[0] == 'B');
                auto rd16 = [&](int off) { return msb ? (buf[off] << 8 | buf[off + 1])
                                                       : (buf[off] | buf[off + 1] << 8); };
                int proto_len = rd16(6), data_len = rd16(8);
                total = 12 + ((proto_len + 3) & ~3) + ((data_len + 3) & ~3);
                header_parsed = true;
            }
        } else if (n != LIBSSH2_ERROR_EAGAIN) {
            close(xfd);
            std::lock_guard<std::recursive_mutex> lock(s_session_mutex);
            libssh2_channel_free(xchan);
            return;
        }
        if ((header_parsed && got >= total) || got >= (int)sizeof(buf)) break;
        SDL_Delay(5);
    }

    if (have_cookie && header_parsed) {
        bool msb = (buf[0] == 'B');
        auto rd16 = [&](int off) { return msb ? (buf[off] << 8 | buf[off + 1])
                                               : (buf[off] | buf[off + 1] << 8); };
        int proto_len = rd16(6), data_len = rd16(8);
        int data_off = 12 + ((proto_len + 3) & ~3);
        if (data_len == (int)cookie.size() && data_off + data_len <= got)
            memcpy(buf + data_off, cookie.data(), cookie.size());
    }

    ssize_t wn = write(xfd, buf, got);
    (void)wn;

    std::thread pump_out([xchan, xfd]() {
        char b[4096];
        for (;;) {
            ssize_t n;
            {
                std::lock_guard<std::recursive_mutex> lock(s_session_mutex);
                n = libssh2_channel_read(xchan, b, sizeof(b));
            }
            if (n > 0) {
                if (write(xfd, b, n) != n) break;
            } else if (n == LIBSSH2_ERROR_EAGAIN) {
                bool eof;
                {
                    std::lock_guard<std::recursive_mutex> lock(s_session_mutex);
                    eof = libssh2_channel_eof(xchan) != 0;
                }
                if (eof) break;
                SDL_Delay(5);
            } else break;
        }
        shutdown(xfd, SHUT_RD);
    });

    char b[4096];
    for (;;) {
        ssize_t n = read(xfd, b, sizeof(b));
        if (n <= 0) break;
        int sent = 0;
        bool err = false;
        while (sent < n) {
            ssize_t rc;
            {
                std::lock_guard<std::recursive_mutex> lock(s_session_mutex);
                rc = libssh2_channel_write(xchan, b + sent, n - sent);
            }
            if (rc > 0) sent += (int)rc;
            else if (rc == LIBSSH2_ERROR_EAGAIN) SDL_Delay(1);
            else { err = true; break; }
        }
        if (err) break;
    }

    pump_out.join();
    close(xfd);
    std::lock_guard<std::recursive_mutex> lock(s_session_mutex);
    libssh2_channel_send_eof(xchan);
    libssh2_channel_close(xchan);
    libssh2_channel_free(xchan);
}

static void x11_open_callback(LIBSSH2_SESSION *session, LIBSSH2_CHANNEL *channel,
                               char *shost, int sport, void **abstract) {
    (void)session; (void)shost; (void)sport; (void)abstract;
    std::thread(x11_forward_worker, channel).detach();
}
#endif // !_WIN32

// Bridge called by term_write() for all output (handle_key, term_paste, etc.)
static void ssh_write_bridge(Terminal *t, const char *s, int n) {
    ssh_write(t, s, n);
}

// ============================================================================
// PUBLIC API
// ============================================================================

bool ssh_connect(const SshConfig &cfg, Terminal *t) {
    set_user_error("");
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        SDL_Log("[SSH] WSAStartup failed: %d\n", WSAGetLastError());
        return false;
    }
#endif

    // libssh2 global init (idempotent across calls)
    if (libssh2_init(0) != 0) {
        SDL_Log("[SSH] libssh2_init failed\n");
        return false;
    }

    // TCP connection
    s_sock = tcp_connect(cfg.host, cfg.port);
    if (s_sock < 0) return false;

    // Create session (non-blocking)
    s_session = libssh2_session_init();
    if (!s_session) {
        SDL_Log("[SSH] libssh2_session_init failed\n");
        return false;
    }

    // Use non-blocking I/O; we spin with SDL_Delay to avoid busy-wait
    libssh2_session_set_blocking(s_session, 0);

    // Send keepalives every 60 seconds to prevent server-side idle disconnect
    libssh2_keepalive_config(s_session, 1, 60);

    // Perform SSH handshake
    int rc;
    while ((rc = libssh2_session_handshake(s_session, s_sock)) ==
           LIBSSH2_ERROR_EAGAIN)
        SDL_Delay(5);
    if (rc != 0) {
        SDL_Log("%s\n", last_ssh2_error("handshake failed").c_str());
        return false;
    }

    // Host key verification
    if (!verify_host_key(cfg)) {
        SDL_Log("[SSH] host key verification failed — aborting\n");
        return false;
    }

    // Authentication priority (OpenSSH-like):
    //   1. Explicit key (-i flag)
    //   2. IdentityFile from ~/.ssh/config (in order listed)
    //   3. Default keys (~/.ssh/id_ed25519, id_rsa, id_dsa, id_ecdsa)
    //   4. SSH agent
    //   5. Password (supplied or prompted)
    bool authed = false;

    // Query what the server will accept — must retry on EAGAIN for non-blocking session
    char *auth_methods = nullptr;
    do {
        auth_methods = libssh2_userauth_list(s_session, cfg.user.c_str(),
                                             (unsigned int)cfg.user.size());
        if (!auth_methods && libssh2_session_last_errno(s_session) == LIBSSH2_ERROR_EAGAIN)
            SDL_Delay(5);
        else
            break;
    } while (true);
    SDL_Log("[SSH] server auth methods: %s\n", auth_methods ? auth_methods : "(none/error)");

    // Collect all keys to try, in order of preference
    std::vector<std::string> keys_to_try;
    
    // 1. Explicit -i key (highest priority)
    if (!cfg.key_path.empty()) {
        keys_to_try.push_back(cfg.key_path);
    }
    
    // 2. Config file IdentityFile entries
    for (const auto &key : cfg.config_key_paths) {
        keys_to_try.push_back(key);
    }
    
    // 3. Default key paths (only if no explicit key or config keys)
    if (cfg.key_path.empty() && cfg.config_key_paths.empty()) {
        std::vector<std::string> default_keys = get_default_key_paths();
        for (const auto &key : default_keys) {
            keys_to_try.push_back(key);
        }
    }

    // Try all collected keys
    for (const auto &key_path : keys_to_try) {
        if (authed) break;
        SDL_Log("[SSH] trying key '%s'\n", key_path.c_str());
        authed = auth_key(cfg.user, key_path, "", cfg.password);

        if (!authed && cfg.prompt_password) {
            const int MAX_ATTEMPTS = 3;
            for (int attempt = 1; attempt <= MAX_ATTEMPTS && !authed; attempt++) {
                char prompt[256];
                snprintf(prompt, sizeof(prompt), "Enter passphrase for key '%s': ",
                         key_path.c_str());
                std::string passphrase = cfg.prompt_password(prompt);
                if (passphrase.empty()) break;
                authed = auth_key(cfg.user, key_path, "", passphrase);
            }
            if (authed) break;
        }
    }

    // Try agent if no key worked
    if (!authed) {
        SDL_Log("[SSH] trying agent auth for user '%s'\n", cfg.user.c_str());
        authed = auth_agent(cfg.user);
        if (!authed) SDL_Log("[SSH] agent auth failed or no agent\n");
    }

    if (!authed) {
        bool server_allows_password = !auth_methods ||
                                      strstr(auth_methods, "password") != nullptr;
        SDL_Log("[SSH] password auth stage: password_set=%s prompt_cb=%s server_allows=%s\n",
                cfg.password.empty() ? "no" : "yes",
                cfg.prompt_password ? "yes" : "no",
                server_allows_password ? "yes" : "no");
        
        if (server_allows_password) {
            // Try supplied password first (if any)
            if (!cfg.password.empty()) {
                SDL_Log("[SSH] attempting with supplied password\n");
                authed = auth_password(cfg.user, cfg.password);
            }
            
            // If that failed and we have a prompt callback, allow up to 3 attempts
            if (!authed && cfg.prompt_password) {
                const int MAX_ATTEMPTS = 3;
                for (int attempt = 1; attempt <= MAX_ATTEMPTS && !authed; attempt++) {
                    char prompt[256];
                    if (attempt == 1) {
                        snprintf(prompt, sizeof(prompt), "%s@%s's password: ",
                                 cfg.user.c_str(), cfg.host.c_str());
                    } else {
                        snprintf(prompt, sizeof(prompt), "Permission denied, try again. %s@%s's password: ",
                                 cfg.user.c_str(), cfg.host.c_str());
                    }
                    
                    SDL_Log("[SSH] password prompt attempt %d/%d\n", attempt, MAX_ATTEMPTS);
                    std::string password = cfg.prompt_password(prompt);
                    
                    if (password.empty()) {
                        SDL_Log("[SSH] user aborted password entry\n");
                        break;
                    }
                    
                    authed = auth_password(cfg.user, password);
                    if (authed) {
                        SDL_Log("[SSH] authenticated via password on attempt %d\n", attempt);
                    } else {
                        SDL_Log("[SSH] password attempt %d failed\n", attempt);
                    }
                }
            }
        }
    }

    if (!authed) {
        SDL_Log("[SSH] authentication failed for user '%s'\n", cfg.user.c_str());
        return false;
    }

    // Open channel
#ifndef _WIN32
    if (cfg.x11_forward)
        libssh2_session_callback_set(s_session, LIBSSH2_CALLBACK_X11, (void *)x11_open_callback);
#endif
    while (!(s_channel = libssh2_channel_open_session(s_session)))
        if (libssh2_session_last_errno(s_session) != LIBSSH2_ERROR_EAGAIN)
            break;
    if (!s_channel) {
        SDL_Log("%s\n", last_ssh2_error("channel open failed").c_str());
        return false;
    }

    // Request PTY. Servers like git@github.com refuse PTY allocation but
    // still open the channel and send a message before closing — non-fatal.
    // Report one fewer column/row than we actually render, matching
    // term_spawn()'s convention (term_pty.cpp) — so a full-width line from
    // the remote shell never reaches our real autowrap boundary.
    int report_cols = t->cols > 1 ? t->cols - 1 : t->cols;
    int report_rows = t->rows > 1 ? t->rows - 1 : t->rows;
    while ((rc = libssh2_channel_request_pty_ex(
                s_channel,
                g_term_type, (unsigned int)strlen(g_term_type),
                nullptr, 0,
                report_cols, report_rows,
                (int)(report_cols * t->cell_w), (int)(report_rows * t->cell_h))) == LIBSSH2_ERROR_EAGAIN)
        SDL_Delay(5);
    s_have_pty = (rc == 0);
    if (!s_have_pty)
        SDL_Log("%s (continuing without PTY)\n", last_ssh2_error("pty request failed").c_str());

    // X11 forwarding. Non-fatal if the server refuses it.
#ifndef _WIN32
    if (cfg.x11_forward) {
        while ((rc = libssh2_channel_x11_req_ex(s_channel, 0, nullptr, nullptr, 0)) ==
               LIBSSH2_ERROR_EAGAIN)
            SDL_Delay(5);
        if (rc == 0)
            SDL_Log("[SSH] X11 forwarding enabled\n");
        else
            SDL_Log("%s (continuing without X11 forwarding)\n", last_ssh2_error("x11 forwarding request failed").c_str());
    }
#endif

    // Setup remote port forwarding (-R)
    // Store for later retrieval via ssh_get_remote_forwards()
    s_remote_forwards = cfg.remote_forwards;

    // Environment hints — must be sent BEFORE the shell/exec request, or the
    // shell has already started without them. Best-effort: sshd only
    // accepts variables listed in its AcceptEnv (COLORTERM often is).
    while (libssh2_channel_setenv(s_channel, "COLORTERM", "truecolor") == LIBSSH2_ERROR_EAGAIN)
        SDL_Delay(5);

    // Start shell or execute command
    if (!cfg.command.empty()) {
        // Execute a specific command on the remote server
        SDL_Log("[SSH] executing command: %s\n", cfg.command.c_str());
        while ((rc = libssh2_channel_exec(s_channel, cfg.command.c_str())) 
               == LIBSSH2_ERROR_EAGAIN)
            SDL_Delay(5);
    } else {
        // Start interactive shell (default behavior)
        SDL_Log("[SSH] starting interactive shell\n");
        while ((rc = libssh2_channel_shell(s_channel)) == LIBSSH2_ERROR_EAGAIN)
            SDL_Delay(5);
    }
    if (rc != 0) {
        SDL_Log("%s\n", last_ssh2_error("shell/command request failed").c_str());
        return false;
    }

    // Route all term_write() calls (handle_key, term_paste, etc.) through SSH
    g_term_write_override = ssh_write_bridge;

    s_term   = t;
    s_active = true;
    SDL_Log("[SSH] connected to %s@%s:%d\n", cfg.user.c_str(), cfg.host.c_str(), cfg.port);
    return true;
}

bool ssh_read(Terminal *t) {
    if (!s_active || !s_channel) return false;

    char buf[4096];
    bool got_data = false;

    std::unique_lock<std::recursive_mutex> lock(s_session_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;

    for (;;) {
        ssize_t n = libssh2_channel_read(s_channel, buf, sizeof(buf));
        if (n > 0) {
            term_feed(t, buf, (int)n);
            got_data = true;
            continue;
        } else if (n != LIBSSH2_ERROR_EAGAIN && n < 0) {
            SDL_Log("%s\n", last_ssh2_error("channel read error").c_str());
            s_active = false;
            break;
        }

        // Also drain stderr — servers that reject a shell (e.g. git@github.com)
        // send their message here before closing the channel.
        ssize_t ne = libssh2_channel_read_stderr(s_channel, buf, sizeof(buf));
        if (ne > 0) {
            term_feed(t, buf, (int)ne);
            got_data = true;
            continue;
        } else if (ne != LIBSSH2_ERROR_EAGAIN && ne < 0) {
            SDL_Log("%s\n", last_ssh2_error("stderr read error").c_str());
            s_active = false;
            break;
        }

        if (n == LIBSSH2_ERROR_EAGAIN && ne == LIBSSH2_ERROR_EAGAIN) {
            static uint32_t s_last_keepalive = 0;
            uint32_t now = SDL_GetTicks();
            if (now - s_last_keepalive >= 1000) {
                int next_keepalive = 0;
                libssh2_keepalive_send(s_session, &next_keepalive);
                s_last_keepalive = now;
            }
            break;
        }

        if (libssh2_channel_eof(s_channel)) {
            s_active = false;
            break;
        }
    }
    return got_data;
}

void ssh_write(Terminal *t, const char *buf, int n) {
    (void)t;
    if (!s_active || !s_channel || n <= 0) return;

    std::lock_guard<std::recursive_mutex> lock(s_session_mutex);
    int sent = 0;
    while (sent < n) {
        ssize_t rc = libssh2_channel_write(s_channel, buf + sent, (size_t)(n - sent));
        if (rc > 0) {
            sent += (int)rc;
        } else if (rc == LIBSSH2_ERROR_EAGAIN) {
            SDL_Delay(1);
        } else {
            SDL_Log("%s\n", last_ssh2_error("channel write error").c_str());
            break;
        }
    }
}

void ssh_pty_resize(int cols, int rows) {
    if (!s_active || !s_channel || !s_have_pty) return;
    int report_cols = cols > 1 ? cols - 1 : cols;
    int report_rows = rows > 1 ? rows - 1 : rows;
    // Include the pixel size (like the initial pty request does). The plain
    // libssh2_channel_request_pty_size() macro sends 0x0 pixels, after which
    // timg & co. can't see the cell size and fall back from kitty/sixel
    // graphics to block characters.
    int px_w = s_term ? (int)(report_cols * s_term->cell_w) : 0;
    int px_h = s_term ? (int)(report_rows * s_term->cell_h) : 0;
    std::lock_guard<std::recursive_mutex> lock(s_session_mutex);
    int rc, tries = 0;
    while ((rc = libssh2_channel_request_pty_size_ex(s_channel, report_cols, report_rows,
                                                      px_w, px_h)) == LIBSSH2_ERROR_EAGAIN &&
           ++tries < 200)
        SDL_Delay(1);
}

bool ssh_channel_closed() {
    if (!s_active || !s_channel) return true;
    return libssh2_channel_eof(s_channel) != 0;
}

void ssh_disconnect() {
    // Restore normal PTY write path before tearing down the channel
    g_term_write_override = nullptr;

    if (s_channel) {
        libssh2_channel_send_eof(s_channel);
        libssh2_channel_wait_eof(s_channel);
        libssh2_channel_close(s_channel);
        libssh2_channel_wait_closed(s_channel);
        libssh2_channel_free(s_channel);
        s_channel = nullptr;
    }
    if (s_session) {
        libssh2_session_disconnect(s_session, "Normal shutdown");
        libssh2_session_free(s_session);
        s_session = nullptr;
    }
    if (s_sock >= 0) {
#ifndef _WIN32
        close(s_sock);
#else
        closesocket(s_sock);
#endif
        s_sock = -1;
    }
    s_active = false;
    s_have_pty = false;
    s_term = nullptr;
    libssh2_exit();
#ifdef _WIN32
    WSACleanup();
#endif
}

bool ssh_active()  { return s_active; }
bool ssh_has_pty() { return s_have_pty; }

LIBSSH2_SESSION *ssh_get_session() { return s_session; }
int              ssh_get_socket()  { return s_sock; }
void             ssh_session_lock()   { s_session_mutex.lock(); }
void             ssh_session_unlock() { s_session_mutex.unlock(); }

void ssh_reset_after_fork() {
    // Clean up inherited libssh2 state from parent process.
    // After fork(), child inherits parent's static variables which point to
    // freed/invalid memory. This resets them so the child can initialize
    // its own libssh2 session cleanly.
    libssh2_exit();
    
    // Reset static state
    s_session = nullptr;
    s_channel = nullptr;
    s_sock    = -1;
    s_active  = false;
    s_have_pty = false;
    s_remote_forwards.clear();
    
    // Re-initialize libssh2 for the child process
    if (libssh2_init(0) != 0) {
        SDL_Log("[SSH] libssh2_init (post-fork) failed\n");
    }
}

// Get remote forwards specs that were configured at connection time
const std::vector<std::string>& ssh_get_remote_forwards() {
    return s_remote_forwards;
}

#endif // USESSH
