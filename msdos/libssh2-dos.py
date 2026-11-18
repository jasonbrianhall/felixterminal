# libssh2 on vmdos's sockets: DJGPP's fcntl() doesn't know them, so
# session_nonblock() uses vmdos's ioctlsocket(), and the socket counts as
# non-blocking (ssh_session.cpp makes it so at connect). Edits src/session.c
# in place (any libssh2 1.11.x); run by build-deps.sh.
import re, sys
p = sys.argv[1]
s = open(p).read()
if '__DJGPP__' in s:
    sys.exit(0)
add = {
    'session_nonblock(': '#if defined(__DJGPP__)  /* MS-DOS: vmdos\'s sockets */\n'
                         '    int flags = nonblock;\n'
                         '    return ioctlsocket(sockfd, FIONBIO, (char *)&flags);\n#elif',
    'get_socket_nonblocking(': '#if defined(__DJGPP__)  /* MS-DOS: made non-blocking at connect */\n'
                               '    (void)sockfd;\n    return 1;\n#elif',
}
for fn, branch in add.items():
    i = s.find(fn)
    j = s.find('#ifdef HAVE_O_NONBLOCK', i)
    if i < 0 or j < 0 or j - i > 400:
        sys.exit('libssh2-dos.py: no ' + fn + ' with #ifdef HAVE_O_NONBLOCK in ' + p)
    s = s[:j] + branch + ' defined(HAVE_O_NONBLOCK)' + s[j + len('#ifdef HAVE_O_NONBLOCK'):]
open(p, 'w').write(s)
