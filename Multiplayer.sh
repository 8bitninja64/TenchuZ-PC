#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
echo 'Experimental multiplayer: use the same game version on every PC.'
echo 'Use your LAN address, or your private VPN address for internet play.'
printf 'This PC IPv4 address: '
read -r local_address
printf 'Host IPv4 address (leave blank to host): '
read -r host_address
exec sh "$root/tools/launch_system_link.sh" "$local_address" "$host_address"
