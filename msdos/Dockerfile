# The build environment for `make DOCKER=1`: the djfdyuruiry/djgpp image
# (DJGPP, as SDLPoPDOS's DOS build uses) plus what build-deps.sh needs
# beside the cross compiler: git (the sources), python3 and perl (OpenSSL's
# Configure, vmdos's errno list, the libssh2 edit), a host gcc with -m32
# (Watt-32's errno table) and upx. The Makefile builds it as felixterminal-djgpp.
FROM djfdyuruiry/djgpp

USER root
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        gcc gcc-multilib make perl python3 git ca-certificates upx-ucl && \
    rm -rf /var/lib/apt/lists/*

# In the image, plain gcc (ar, ...) is DJGPP's. build-deps.sh and vmdos's
# build-openssl.sh want DJGPP by its prefixed names (i586-pc-msdosdjgpp-gcc,
# ...), with DJGPP's headers at bin/../i586-pc-msdosdjgpp/sys-include, and
# gcc to be the host's (gcc -m32: Watt-32's and OpenSSL's errno tables). So:
#   /opt/djgpp/bin   links to the prefixed DJGPP tools only (an
#                    i386-pc-msdosdjgpp toolchain under i586 names too), and
#                    /opt/djgpp/i586-pc-msdosdjgpp to its target directory
#   /opt/host/bin    gcc, cc: the host's own compiler, by its Debian name,
#                    run without the image's DJGPP variables (GCC_EXEC_PREFIX
#                    and the like would send it looking for DJGPP's cc1).
#                    It builds one thing: Watt-32's errnos.c, with DJGPP's
#                    errno.h forced in, whose sys_errlist an old glibc's
#                    stdio.h declares too, with another type; for that file
#                    DJGPP's is renamed while its header is read (the program
#                    doesn't use it).
# and only those two before /usr/bin and /bin on the PATH.
RUN set -e; \
    dj=$(find / -xdev -path /proc -prune -o \( -name 'i586-pc-msdosdjgpp-gcc' -o -name 'i386-pc-msdosdjgpp-gcc' \) \
          \( -type f -o -type l \) -print 2>/dev/null | head -n 1); \
    [ -n "$dj" ] || { echo "no DJGPP gcc (i586/i386-pc-msdosdjgpp-gcc) in the image" >&2; exit 1; }; \
    bin=$(dirname "$dj"); root=$(dirname "$bin"); pre=$(basename "$dj" gcc); \
    mkdir -p /opt/djgpp/bin /opt/host/bin; \
    for t in "$bin/$pre"*; do ln -sf "$t" "/opt/djgpp/bin/i586-pc-msdosdjgpp-${t##*/$pre}"; done; \
    ln -sfn "$root/${pre%-}" /opt/djgpp/i586-pc-msdosdjgpp; \
    host=$(ls /usr/bin/x86_64-linux-gnu-gcc /usr/bin/x86_64-linux-gnu-gcc-[0-9]* 2>/dev/null | head -n 1); \
    [ -n "$host" ] || { echo "no host gcc (x86_64-linux-gnu-gcc)" >&2; exit 1; }; \
    printf '#undef sys_errlist\n' > /opt/host/errlist-undef.h; \
    { printf '#!/bin/sh\n'; \
      printf 'case " $* " in *errnos.c*) set -- -Dsys_errlist=djgpp_sys_errlist "$@" -include /opt/host/errlist-undef.h ;; esac\n'; \
      printf 'exec env -u GCC_EXEC_PREFIX -u COMPILER_PATH -u LIBRARY_PATH -u CPATH -u C_INCLUDE_PATH -u CPLUS_INCLUDE_PATH -u DJDIR -u DJGPP %s "$@"\n' "$host"; \
    } > /opt/host/bin/gcc; \
    chmod +x /opt/host/bin/gcc; ln -sf gcc /opt/host/bin/cc; \
    export PATH=/opt/djgpp/bin:/opt/host/bin:/usr/bin:/bin; \
    i586-pc-msdosdjgpp-gcc --version | head -n 1; \
    ls /opt/djgpp/i586-pc-msdosdjgpp/sys-include/errno.h; \
    c=/tmp/flt-check; printf 'int main(void) { return 0; }\n' > $c.c; \
    gcc -m32 $c.c -o $c || { echo "host gcc -m32 doesn't work ($host); the environment:" >&2; \
        env | grep -i -E 'gcc|djgpp|djdir|path' >&2; exit 1; }; $c; \
    i586-pc-msdosdjgpp-gcc -c $c.c -o $c.o || { echo "DJGPP gcc doesn't work ($dj)" >&2; exit 1; }; \
    echo "host gcc -m32: $host; DJGPP gcc: $dj"; rm -f $c $c.c $c.o

ENV PATH=/opt/djgpp/bin:/opt/host/bin:/usr/bin:/bin
