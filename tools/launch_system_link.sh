#!/bin/sh
# Experimental System Link launcher; normal controller input stays enabled.
set -eu
if [ "$#" -lt 1 ] || [ "$#" -gt 3 ]; then
    echo "Usage: $0 LOCAL_IPV4 [HOST_IPV4] [GAME_DATA_FOLDER]" >&2
    echo "Omit HOST_IPV4 on the host. Use each machine's LAN or VPN address." >&2
    exit 2
fi
valid_ip() {
    printf '%s\n' "$1" | awk -F. '
        NF != 4 { exit 1 }
        { for (i=1;i<=4;i++) if ($i !~ /^[0-9]+$/ || $i > 255 || (length($i)>1 && substr($i,1,1)=="0")) exit 1;
          if ($1 == 0 || $1 >= 224) exit 1 }'
}
valid_ip "$1" || { echo "LOCAL_IPV4 must be a unicast IPv4 address on this machine." >&2; exit 2; }
if [ -n "${2:-}" ]; then
    valid_ip "$2" || { echo "HOST_IPV4 is invalid." >&2; exit 2; }
fi
game_path=
if [ -n "${3:-}" ]; then
    [ -d "$3" ] || { echo "GAME_DATA_FOLDER must be an existing directory." >&2; exit 2; }
    game_path=$(CDPATH= cd -- "$3" && pwd)
fi
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
[ -x "$root/TenchuZ" ] || { echo "Place this script in the game package's tools folder." >&2; exit 1; }
export TENCHU_SYSTEM_LINK=1 TENCHU_NET_BIND="$1" TENCHU_NET_JOIN="${2:-}"
export TENCHU_NET_NAME="${TENCHU_NET_NAME:-Player}" TENCHU_FRAME_RATE=30
export TENCHU_TRACE="${TENCHU_TRACE:-net,xam}" TENCHU_NO_UPDATE_CHECK=1
unset TENCHU_CONTROL
cd "$root"
if [ -n "$game_path" ]; then exec ./TenchuZ "$game_path"; else exec ./TenchuZ; fi
