ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd $ROOT/vita
export VITASDK=$PWD/sdk PATH=$PWD/sdk/bin:$PWD/deps/bin:$PATH
echo "start $(date +%T)"
ninja -C b/game -k 0 -j2 > b/game_build.log 2>&1
echo "exit $? $(date +%T)"
