ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
V=$ROOT/vita
D=$V/deps; P=$V/sdk; T=arm-vita-eabi; B=$V/b
export PATH=$P/bin:$D/bin:$PATH MAKEINFO=true
set -e
step(){ echo "=== $(date +%H:%M:%S) $*"; }
while ! grep -q LIBELF_OK $V/deps.log; do sleep 20; grep -q "Error\|error:" $V/deps.log && { echo DEPS_FAILED; exit 1; }; done
COMMON="--enable-languages=c,c++ --disable-decimal-float --disable-libcc1 --disable-libffi --disable-libmudflap --disable-libquadmath --disable-libssp --disable-libstdcxx-pch --disable-nls --disable-shared --disable-tls --with-gnu-as --with-gnu-ld --with-newlib --without-zstd --disable-multilib --with-arch=armv7-a --with-tune=cortex-a9 --with-fpu=neon --with-float=hard --with-mode=thumb --without-isl --with-gmp=$D --with-mpfr=$D --with-mpc=$D"

step libzip
if [ ! -f $D/lib/libzip.a ]; then
cmake -S $V/src/libzip -B $B/libzip -DCMAKE_INSTALL_PREFIX=$D -DBUILD_SHARED_LIBS=OFF -DENABLE_BZIP2=OFF -DENABLE_LZMA=OFF -DENABLE_ZSTD=OFF -DENABLE_GNUTLS=OFF -DENABLE_OPENSSL=OFF -DENABLE_MBEDTLS=OFF -DENABLE_COMMONCRYPTO=OFF -DENABLE_WINDOWS_CRYPTO=OFF -DBUILD_TOOLS=OFF -DBUILD_REGRESS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_DOC=OFF -DBUILD_OSSFUZZ=OFF >/dev/null
cmake --build $B/libzip -j2 >/dev/null; cmake --install $B/libzip >/dev/null
fi

step binutils
if [ ! -x $P/bin/$T-ld ]; then
rm -rf $B/binutils && mkdir -p $B/binutils && cd $B/binutils
CFLAGS="-O2 -std=gnu11" $V/src/binutils/configure --target=$T --prefix=$P --with-sysroot=$P/$T --disable-nls --disable-werror --disable-separate-code --enable-interwork --enable-plugins --without-zstd --disable-gdb --disable-gdbserver --disable-sim --disable-gprofng --disable-libdecnumber --disable-readline >/dev/null || { echo CONFIGURE_FAIL; exit 1; }
make -j2 MAKEINFO=true >$B/binutils.log 2>&1 || { echo FAIL; exit 1; }
make install MAKEINFO=true >>$B/binutils.log 2>&1 || { echo FAIL; exit 1; }

fi
step vita-toolchain
if [ ! -x $P/bin/vita-elf-create ]; then
cmake -S $V/src/vita-toolchain -B $B/vita-toolchain -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_PREFIX_PATH=$D -DCMAKE_BUILD_TYPE=Release >$B/vtc.log 2>&1
cmake --build $B/vita-toolchain -j2 >>$B/vtc.log 2>&1 || { echo FAIL; exit 1; }
cmake --install $B/vita-toolchain >>$B/vtc.log 2>&1 || { echo FAIL; exit 1; }

fi
step gcc-stage1
rm -rf $B/gcc1 && mkdir -p $B/gcc1 && cd $B/gcc1
$V/src/gcc/configure --target=$T --prefix=$P --with-sysroot=$P/$T $COMMON --enable-languages=c --without-headers --disable-threads --disable-libgomp --disable-libstdcxx >$B/gcc1.log 2>&1 || { echo CONFIGURE_FAIL; exit 1; }
make -j2 all-gcc all-target-libgcc MAKEINFO=true >>$B/gcc1.log 2>&1 || { echo FAIL; exit 1; }
make install-gcc install-target-libgcc MAKEINFO=true >>$B/gcc1.log 2>&1 || { echo FAIL; exit 1; }

step vita-headers
rm -rf $B/vh && mkdir -p $B/vh
$P/bin/vita-libs-gen-2 -yml=$V/src/vita-headers/db -output=$B/vh >$B/vh.log 2>&1 || { echo FAIL; exit 1; }
cd $B/vh && make -j2 ARCH=$P/bin/$T >>$B/vh.log 2>&1 || { echo FAIL; exit 1; }
mkdir -p $P/$T/lib $P/$T/include && cp $B/vh/*.a $P/$T/lib/ && cp -r $V/src/vita-headers/include/. $P/$T/include/
mkdir -p $P/share/vita-headers && cp -r $V/src/vita-headers/db $P/share/vita-headers/

step newlib
rm -rf $B/newlib && mkdir -p $B/newlib && cd $B/newlib
$V/src/newlib/configure "CFLAGS_FOR_TARGET=-g -O2 -ffunction-sections -fdata-sections" --target=$T --prefix=/ --with-build-sysroot=$P/$T --enable-newlib-io-long-long --enable-newlib-register-fini --disable-newlib-supplied-syscalls --enable-newlib-long-time_t --disable-nls --enable-newlib-iconv --enable-newlib-mb >$B/newlib.log 2>&1 || { echo CONFIGURE_FAIL; exit 1; }
make -j2 MAKEINFO=true >>$B/newlib.log 2>&1 || { echo FAIL; exit 1; }
make install DESTDIR=$P MAKEINFO=true >>$B/newlib.log 2>&1 || { echo FAIL; exit 1; }

step pthread
rm -rf $B/pthread && cp -r $V/src/pthread-embedded $B/pthread
make -C $B/pthread/platform/vita PREFIX=$P >$B/pthread.log 2>&1 || { echo FAIL; exit 1; }
make -C $B/pthread/platform/vita PREFIX=$P/$T install >>$B/pthread.log 2>&1 || { echo FAIL; exit 1; }

step gcc-final
rm -rf $B/gcc2 && mkdir -p $B/gcc2 && cd $B/gcc2
$V/src/gcc/configure --target=$T --prefix=$P --with-sysroot=$P/$T $COMMON --with-headers=yes --enable-threads=posix --disable-libgomp >$B/gcc2.log 2>&1 || { echo CONFIGURE_FAIL; exit 1; }
make -j2 MAKEINFO=true >>$B/gcc2.log 2>&1 || { echo FAIL; exit 1; }
make install MAKEINFO=true >>$B/gcc2.log 2>&1 || { echo FAIL; exit 1; }
step TOOLCHAIN_DONE
