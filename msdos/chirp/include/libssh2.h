/* libssh2.h for Felix Chirp on MS-DOS: Chirp on DOS has no SSH session, so
   its remote (SFTP) browsing is never reached: ssh_get_session() is always
   NULL (chirp_dos.cpp). These only let felixchirp/ compile. */
#pragma once
#include <stddef.h>
#include <sys/types.h>
typedef struct _LIBSSH2_SESSION LIBSSH2_SESSION;
static inline void libssh2_session_set_blocking(LIBSSH2_SESSION *s, int b) { (void)s; (void)b; }
