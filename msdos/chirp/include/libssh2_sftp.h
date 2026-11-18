/* libssh2_sftp.h for Felix Chirp on MS-DOS: see libssh2.h. Every call fails. */
#pragma once
#include "libssh2.h"
typedef struct _LIBSSH2_SFTP LIBSSH2_SFTP;
typedef struct _LIBSSH2_SFTP_HANDLE LIBSSH2_SFTP_HANDLE;
typedef struct _LIBSSH2_SFTP_ATTRIBUTES {
    unsigned long flags;
    unsigned long long filesize;
    unsigned long uid, gid, permissions, atime, mtime;
} LIBSSH2_SFTP_ATTRIBUTES;
#define LIBSSH2_SFTP_ATTR_SIZE        0x00000001
#define LIBSSH2_SFTP_ATTR_PERMISSIONS 0x00000004
#define LIBSSH2_SFTP_S_IFMT   0170000
#define LIBSSH2_SFTP_S_IFDIR  0040000
#define LIBSSH2_SFTP_S_ISDIR(m) (((m) & LIBSSH2_SFTP_S_IFMT) == LIBSSH2_SFTP_S_IFDIR)
#define LIBSSH2_FXF_READ 0x00000001
static inline LIBSSH2_SFTP *libssh2_sftp_init(LIBSSH2_SESSION *s) { (void)s; return 0; }
static inline int libssh2_sftp_shutdown(LIBSSH2_SFTP *s) { (void)s; return 0; }
static inline LIBSSH2_SFTP_HANDLE *libssh2_sftp_open(LIBSSH2_SFTP *s, const char *p, unsigned long f, long m)
{ (void)s; (void)p; (void)f; (void)m; return 0; }
static inline LIBSSH2_SFTP_HANDLE *libssh2_sftp_opendir(LIBSSH2_SFTP *s, const char *p) { (void)s; (void)p; return 0; }
static inline ssize_t libssh2_sftp_read(LIBSSH2_SFTP_HANDLE *h, char *b, size_t n) { (void)h; (void)b; (void)n; return -1; }
static inline int libssh2_sftp_readdir_ex(LIBSSH2_SFTP_HANDLE *h, char *b, size_t n, char *l, size_t ln,
                                          LIBSSH2_SFTP_ATTRIBUTES *a) { (void)h; (void)b; (void)n; (void)l; (void)ln; (void)a; return -1; }
static inline int libssh2_sftp_close(LIBSSH2_SFTP_HANDLE *h) { (void)h; return 0; }
static inline int libssh2_sftp_closedir(LIBSSH2_SFTP_HANDLE *h) { (void)h; return 0; }
