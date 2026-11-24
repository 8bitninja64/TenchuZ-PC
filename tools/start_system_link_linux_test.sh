#!/bin/bash
# Headless Linux game test. Can run under `ip netns exec tenchu-vhost ...`.
# Set TENCHU_TEST_RUNTIME and TENCHU_TEST_MODULE to built Linux artifacts.
set -euo pipefail
if (( $# < 2 || $# > 3 )) || [[ ! $1 =~ ^[a-z0-9_-]+$ ]]; then
    echo "Usage: $0 INSTANCE LOCAL_IPV4 [HOST_IPV4]" >&2; exit 2
fi
root=$(cd "$(dirname "$0")/.." && pwd)
runtime=$(realpath "${TENCHU_TEST_RUNTIME:-$root/build/linux-clang}")
module=$(realpath "${TENCHU_TEST_MODULE:-$runtime/tenchu_game.so}")
fixture=$(realpath "${TENCHU_TEST_SAVE:-$root/build/fps-effects-baseline/user}")
game=$(realpath "${TENCHU_TEST_GAME:-$root/game}")
[[ -x $runtime/TenchuZ && -f $module && -d $fixture && -d $game ]] || {
    echo "Runtime, module, test-save fixture or game directory is missing." >&2; exit 1;
}
out=$root/build/multiplayer/$1
if [[ -f $out/pid ]] && kill -0 "$(cat "$out/pid")" 2>/dev/null; then
    echo "The recorded test PID is still active; inspect it before restarting $out." >&2; exit 1
fi
mkdir -p "$out"
cp "$runtime/TenchuZ" "$out/TenchuZ"
cp -L "$runtime"/lib*.so* "$out/"
touch "$out/portable.txt"
if [[ ! -d $out/user ]]; then cp -R "$fixture" "$out/user"; fi
cat > "$out/TenchuZ.ini" <<'EOF'
[game]
update_check=false
language=en
[display]
fullscreen=false
frame_rate_limit=30
vsync=false
EOF
: > "$out/control.txt"
export TENCHU_SYSTEM_LINK=1 TENCHU_NET_BIND="$2" TENCHU_NET_JOIN="${3:-}" TENCHU_NET_NAME="$1"
export TENCHU_MODULE="$module" TENCHU_CONTROL="$out/control.txt" TENCHU_HEADLESS=1
export TENCHU_TRACE="${TENCHU_TEST_TRACE:-net,netpack,thread,xam}"
export TENCHU_FRAME_RATE=30 TENCHU_SKIP_MOVIES=1 TENCHU_NO_UPDATE_CHECK=1
export LD_LIBRARY_PATH="$out${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
cd "$out"
echo $$ > pid
echo "Starting $1 PID $$; control: $out/control.txt; game log: $out/user/TenchuZ.log"
# Remain attached: automation runners and WSL can tear down detached children
# when the launching command ends. The control file's quit command stops it.
exec ./TenchuZ "$game" > run.log 2>&1 < /dev/null
