ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
V=$ROOT/vita
S=$ROOT
export VITASDK=$V/sdk PATH=$V/sdk/bin:$V/deps/bin:$PATH
while ! grep -q LIBS_DONE $V/libs.log; do grep -q "FAIL" $V/libs.log && { echo LIBS_FAILED; exit 1; }; sleep 30; done
echo "=== configure $(date +%T)"
cmake -S $S/mpvita -B $V/b/game -G "${GEN:-Unix Makefiles}" -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release -DMPP_DIR=$S/mpp -DLIBSRC=$V/libsrc > $V/b/game_cfg.log 2>&1 || { echo CONFIGURE_FAIL; tail -30 $V/b/game_cfg.log; exit 1; }
echo "=== build $(date +%T)"
cmake --build $V/b/game -j2 -- -k > $V/b/game_build.log 2>&1
echo "=== build exit $? $(date +%T)"
grep -c "error:" $V/b/game_build.log
