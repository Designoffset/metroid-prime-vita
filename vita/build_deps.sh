ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
V=$ROOT/vita
D=$V/deps; mkdir -p $D/lib $D/include $V/b
set -e
[ -f $D/lib/libgmp.a ] || { cd $V/b && rm -rf gmp && mkdir gmp && cd gmp && $V/src/gmp/configure --prefix=$D --disable-shared --enable-static MAKEINFO=true CFLAGS="-O2 -std=gnu11" >/dev/null && make -j2 MAKEINFO=true >/dev/null && make install MAKEINFO=true >/dev/null; }; echo GMP_OK
cd $V/b && rm -rf mpfr && mkdir mpfr && cd mpfr && $V/src/mpfr/configure --prefix=$D --with-gmp=$D --disable-shared --enable-static MAKEINFO=true >/dev/null && make -j2 MAKEINFO=true >/dev/null && make install MAKEINFO=true >/dev/null && echo MPFR_OK
cd $V/b && rm -rf mpc && mkdir mpc && cd mpc
sed 's/@MPC_HAVE_COMPLEX_H@/1/' $V/src/mpc/src/mpc.h.in > mpc.h
sed 's/@MPC_HAVE_COMPLEX_H@/1/' $V/src/mpc/src/mpc-impl.h.in > mpc-impl.h
printf '#define HAVE_STDLIB_H 1\n#define HAVE_LOCALECONV 1\n#define PACKAGE_VERSION "1.4.2dev"\n' > config.h
for f in $V/src/mpc/src/*.c; do gcc -O2 -DHAVE_CONFIG_H -c -I. -I$V/src/mpc/src -I$D/include $f -o $(basename $f .c).o; done
ar rcs libmpc.a *.o && cp libmpc.a $D/lib/ && cp mpc.h $D/include/ && echo MPC_OK
cd $V/b && rm -rf libelf && cp -r $V/src/libelf libelf && cd libelf && ./configure --prefix=$D --disable-shared --enable-static --disable-nls >/dev/null && make -j2 >/dev/null && make install >/dev/null && echo LIBELF_OK
