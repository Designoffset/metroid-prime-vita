ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
set -x
cd src
git clone -q --depth 1 --branch releases/gcc-15.2.0 https://github.com/gcc-mirror/gcc.git gcc &
git clone -q --depth 1 --branch binutils-2_46_1 https://github.com/devkitPro/binutils-gdb.git binutils &
git clone -q --depth 1 --branch v6.2.1 https://github.com/alisw/GMP.git gmp &
git clone -q --depth 1 https://github.com/alisw/MPFR.git mpfr &
git clone -q --depth 1 https://github.com/BrianGladman/mpc.git mpc &
git clone -q --depth 1 https://github.com/Distrotech/libelf.git libelf &
for r in newlib vita-toolchain vita-headers pthread-embedded vdpm samples; do git clone -q --depth 1 https://github.com/vitasdk/$r.git $r & done
wait
echo FETCH_DONE
