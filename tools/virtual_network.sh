#!/bin/bash
# Disposable Linux network for two game test processes. Never changes host routes,
# firewall rules, or the host's network-interface qdiscs. Requires root/iproute2.
set -euo pipefail
host=tenchu-vhost
guest=tenchu-vguest
usage() { echo "Usage: $0 create | profile {lan|moderate|lossy|outage} | status | delete" >&2; exit 2; }
exists() { ip netns list | awk '{print $1}' | grep -Fxq "$1"; }
owned_link() {
    local ns=$1 address=$2
    ip -n "$ns" -4 -o addr show dev tz0 | grep -Fq "inet $address/24"
}
case ${1:-} in
create)
    if exists "$host" || exists "$guest"; then echo "Test namespace already exists; inspect it before reusing." >&2; exit 1; fi
    made_host=0; made_guest=0
    trap 'if (( made_guest )); then ip netns del "$guest"; fi; if (( made_host )); then ip netns del "$host"; fi' ERR
    ip netns add "$host"; made_host=1
    ip netns add "$guest"; made_guest=1
    ip -n "$host" link add tz0 type veth peer name tz0 netns "$guest"
    ip -n "$host" addr add 10.203.0.1/24 dev tz0
    ip -n "$guest" addr add 10.203.0.2/24 dev tz0
    for ns in "$host" "$guest"; do ip -n "$ns" link set lo up; ip -n "$ns" link set tz0 up; done
    trap - ERR
    echo "Created $host (10.203.0.1) and $guest (10.203.0.2), with no external route."
    ;;
profile)
    owned_link "$host" 10.203.0.1
    owned_link "$guest" 10.203.0.2
    case ${2:-} in
      lan) args=(delay 0ms loss 0%);;
      moderate) args=(delay 40ms 10ms distribution normal loss 0% rate 20mbit);;
      lossy) args=(delay 80ms 20ms distribution normal loss 1% rate 5mbit);;
      outage) args=(loss 100%);;
      *) usage;;
    esac
    for ns in "$host" "$guest"; do
        ip netns exec "$ns" tc qdisc replace dev tz0 root netem limit 10000 "${args[@]}"
    done
    echo "Applied $2 in BOTH directions."
    ;;
status)
    for ns in "$host" "$guest"; do
        echo "[$ns]"
        ip -n "$ns" -4 addr show dev tz0
        ip netns exec "$ns" tc -s qdisc show dev tz0
    done
    ;;
delete)
    for ns in "$host" "$guest"; do
        if exists "$ns" && [ -n "$(ip netns pids "$ns")" ]; then
            echo "$ns still has processes. Stop the test copies before deleting the network." >&2
            exit 1
        fi
    done
    if exists "$host"; then owned_link "$host" 10.203.0.1; ip netns del "$host"; fi
    # Deleting one endpoint removes both veth devices; only the empty guest remains.
    if exists "$guest"; then ip netns del "$guest"; fi
    ;;
*) usage;;
esac
