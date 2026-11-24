# Experimental System Link

Multiplayer is under development and disabled by default. Community testers
should start with the [v0.4.0-beta.1 guide](multiplayer-beta.md). The following
is the developer evidence and test procedure; stable v0.3.5 has no multiplayer.
Both players need
the same updated runtime and a game module generated with the full patch series,
including `xenonrecomp-system-link-half.patch`.

## Current evidence (2026-11-24)

- Two Windows processes: discovery, joining, village movement, mission 1 entry,
  mission results with the same shared scoreboard, return to village, saving in
  isolated test profiles, session exit, and immediate rejoining.
- Reconnection was verified while the previous TCP connection was still in
  TIME_WAIT. Outgoing connections now get a fresh source port.
- Windows guest / Linux host: village join and remote movement. Linux ran under
  WSL with llvmpipe software rendering. Mission entry timed out; this does not
  establish whether hardware-rendered Linux missions work.
- A subsequent headless Linux host / rendered Windows guest test entered a
  mission, moved the guest, killed the objective with an assisted host attack,
  and returned to the village. Headless Linux now explicitly caps presents at
  30 FPS; it cannot rely on the display's normal pacing. This verifies Linux
  mission/network logic, not the Linux renderer or SteamOS hardware.
- Two headless Linux processes in separate network namespaces discovered,
  joined and entered a mission with 40 ms +/- 10 ms delay per direction.
  After an assisted objective kill, both returned to village state with
  80 ms +/- 20 ms delay, 1% packet loss and a 5 Mbit/s cap per direction.
  Kernel counters confirmed dropped packets. A subsequent host movement had
  identical position words on the guest. This is simulated WAN testing on one
  machine, not an actual internet session or a visual scoreboard comparison.
- A simulated 100% packet-loss outage disconnected the Linux guest. The host
  stayed responsive and the guest continued processing control commands after
  leaving the session. After connectivity was restored, a restarted guest
  rejoined the original host using a fresh TCP source port, with matching host
  position on both peers. Automatic reconnection without restarting was not
  tested.
- Windows regression suite and Linux socket tests pass. Tests exercise real
  guest-ABI UDP/TCP traffic, event waits, nonblocking errors, refused connections,
  orderly disconnects, repeated reconnects, and the native unprivileged listener.
- The Linux socket suite also passes as `nobody`. The final Windows game pair
  discovers/joins with OS endpoints confirmed on UDP 11000/11001/11004 and TCP
  11003; the guest uses a fresh ephemeral TCP source port.
- Generated half-float unpacking was checked against all 65,536 input bit patterns.
- Four Windows processes: all three guests finish village loading, the full party
  enters mission 1, a host attack kills the target, all reach mission results,
  the host/guest shared scoreboards agree, and the party returns to the village.
  This exposed and fixed reverse address lookup returning a zero Ethernet ID;
  the title compares that ID when matching players. A four-address round-trip
  regression now covers the correction on Windows and Linux.
- After that mission, one guest process was closed unexpectedly. The other
  clients remained responsive; the disconnected guest restarted and rejoined
  the still-running session.
- An experimental Linux package was built in Ubuntu 22.04, passed the glibc
  2.35 ceiling and resolved runtime/generator dependencies in the clean baseline
  image. Its bundled first-start compiler translated the disc with zero semantic
  diagnostics and compiled/linked all 65 module files there. The resulting game
  module is kept outside the package. These are compatibility/setup checks, not
  a claim of SteamOS gameplay verification.

The mission test used authorized cheats in copied test saves. It establishes the
mission/results transition, not comprehensive normal-combat synchronization.
Four-player testing so far uses one Windows PC and assisted test saves; it is
not a separate-device performance test. Mixed FPS, hardware SteamOS missions and
internet play are not yet verified. Voice, relay/NAT traversal, persistent network identity and synchronized
cheat/settings policy are not implemented. Do not present this as finished online
multiplayer or compatibility with Xbox 360 consoles/other implementations.

## Same-PC developer test

Run from the repository root in PowerShell:

```powershell
./tools/start_system_link_test.ps1 -SaveDirectory '<copied fixture user folder>'
```

The helper creates portable copies in `build/multiplayer/host` and `guest`, using
127.0.0.1 and 127.0.0.2. It copies the supplied save folder only on first use and
never writes back to it. It starts hidden windows at 30 FPS. `-Visible` displays
them. Control files replace controller input; drive each with `tools/drive.py`.
Append `quit` to each control file to stop these copies.
For a human playtest, add `-ControllerInput -Visible`; control-file commands are
then disabled and the game uses normal keyboard/controller input. Use `-Side host`
or `-Side guest` when testing against another device, with the corresponding
`-HostAddress` and `-GuestAddress` LAN/VPN addresses. Close the game normally when
controller input is enabled.

Use `-Side all` for four copies (`host`, `guest`, `guest2`, `guest3`), bound to
127.0.0.1 through 127.0.0.4. `-Guest2Address` and `-Guest3Address` override the
extra guest addresses. Each copy has its own portable save folder and control
file. Four processes share one GPU; record actual FPS before drawing conclusions
about gameplay timing.

In the game, load a character, choose System Link, then Create Game on the host.
Choose Search for Game and Join on the guest. The host selects a mission through
the village leader; the guest then accepts that mission through the same leader.
Wait for both player names to show OK before departing.

## Virtual network test

`tools/virtual_network.sh` creates two disposable Linux network namespaces,
`tenchu-vhost` and `tenchu-vguest`, connected only to each other. It requires
root and iproute2. It does not change the machine's external routes or firewall.
Create them with `sudo bash tools/virtual_network.sh create`, then choose
`profile moderate`, `profile lossy`, or `profile outage`. Profiles affect both
directions; `status` prints addresses and packet-drop counters.

Run each game in a separate terminal/session, keeping that session attached:

```sh
sudo env TENCHU_TEST_RUNTIME=/absolute/path/to/runtime \
  TENCHU_TEST_MODULE=/absolute/path/to/tenchu_game.so \
  ip netns exec tenchu-vhost bash tools/start_system_link_linux_test.sh wan-host 10.203.0.1
sudo env TENCHU_TEST_RUNTIME=/absolute/path/to/runtime \
  TENCHU_TEST_MODULE=/absolute/path/to/tenchu_game.so \
  ip netns exec tenchu-vguest bash tools/start_system_link_linux_test.sh wan-guest 10.203.0.2 10.203.0.1
```

The helper defaults to the existing copied fixture in
`build/fps-effects-baseline/user`; override it with `TENCHU_TEST_SAVE` if needed.
It creates independent portable folders, copies the fixture only on first use,
and runs headless at 30 FPS. Wait for its startup message before appending input
to `build/multiplayer/<instance>/control.txt`; startup clears that file. Headless
mode cannot capture screenshots. Use synchronized memory probes/logs or a
rendered peer to establish game state, not merely the presence of TCP traffic.

Append `quit` to each control file, wait for both processes to exit, then run
`sudo bash tools/virtual_network.sh delete`. Deletion refuses active peers.
These tests do not cover GPU rendering, SteamOS input integration, public
internet routing, NAT traversal, or VPN relays.

## Two-machine LAN test

Use separate portable test folders and copied saves on both machines. Keep the
normal installation and saves separate. Launch each updated executable from its
own test folder, supplying its game-data path as usual. Set these environment
variables for that process:

Build the Linux handoff with `tools/build_linux_release.sh` in Ubuntu 22.04 and
package with `TENCHU_MAX_GLIBC=2.35`. The local WSL development runtime currently
requires glibc 2.43 and must not be assumed compatible with the Blackbox. Actual
SteamOS launch, GPU renderer and mission testing remain pending device access.

The Linux test package includes `tools/launch_system_link.sh`. From its folder:

```sh
# Host: use this machine's address.
sh tools/launch_system_link.sh 192.168.1.10
# Guest: this machine's address, followed by the host's address.
sh tools/launch_system_link.sh 192.168.1.11 192.168.1.10
```

Replace these example addresses with the actual LAN or VPN addresses. An optional
third argument supplies an existing extracted game-data folder (on the host,
pass an empty second argument). Without it, the game's normal setup/location
flow runs. The launcher requires a portable test folder, disables update checks,
uses 30 FPS and leaves ordinary controller input enabled.

| Variable | Host | Guest |
| --- | --- | --- |
| `TENCHU_SYSTEM_LINK` | `1` | `1` |
| `TENCHU_NET_BIND` | Host's LAN IPv4 | Guest's LAN IPv4 |
| `TENCHU_NET_JOIN` | Unset | Host's LAN IPv4 |
| `TENCHU_NET_NAME` | `Host` | `Guest` |
| `TENCHU_FRAME_RATE` | `30` | `30` |

Leave `TENCHU_CONTROL` unset for normal controllers. `TENCHU_NET_BIND` must be an
address on the machine running that copy, not the other player's address.

Both peers use UDP 11000, 11001 and 11004 and TCP 11003 on the LAN. The socket
bridge maps the original low game ports to these unprivileged ports so Linux
does not require root. The host listens on TCP 11003; clients use ephemeral
source ports. Allow the test executable on the private LAN in the OS firewall.
An explicit block rule takes precedence over an allow rule. Do not disable the
firewall or expose the game to the public internet for this test.

Test village movement in both directions, mission 1 entry, sword damage and
enemy death seen by both peers, mission completion, return to village, guest
exit, host exit, immediate rejoin and an interrupted connection. Save logs from
both peers with `TENCHU_TRACE=net,xam`; add `netpack` only when diagnosing packet
flow. Record each machine's FPS and GPU renderer when a mission transition fails.

## Internet verification plan

Use a private virtual network for the first external test. Every participant
must be able to reach every other participant, not just the host. Set
`TENCHU_NET_BIND` to that machine's virtual IPv4 address and `TENCHU_NET_JOIN` to
the host's virtual IPv4 address. The explicit join address avoids relying on
broadcast discovery crossing the virtual network.

Tailscale supports direct connections and relay fallback when direct connectivity
is unavailable ([connection types](https://tailscale.com/docs/reference/connection-types)).
Its [Steam Deck installer project](https://github.com/tailscale-dev/deck-tailscale)
documents SteamOS setup. No VPN has been installed or authenticated as part of
these tests, and internet gameplay has not been verified.

For evidence, use two physical machines on different networks and record
`tailscale ping <peer>` and `tailscale status`, including whether the connection
is direct or relayed. Then repeat the mission/exit/rejoin checklist above. A
same-PC or same-LAN run is not evidence of internet play. Test relayed connections
separately if the initial run is direct. Do not publish a claim of internet
support based only on socket unit tests or VPN reachability.

## Implementation notes

The bridge uses native IPv4 sockets and preserves the title's existing System
Link protocol. Direct discovery and replies use the specified host address.
It translates guest big-endian integers, socket structures and Windows socket
errors on Linux. Unconnected TCP sockets are not polled for close notifications.
The title's teardown drains the receive side before joining its worker; sending
FIN first avoids both peers waiting indefinitely for the other's EOF.

Player identity currently derives from the configured bind address. This is a
prototype limitation, not an account/authentication service.

Protocol research references:

- [Tenchu Z System Link investigation](https://github.com/Llamapixels/TenchuZRecomp/blob/main/docs/multiplayer.md)
- [Xenia XGI implementation](https://github.com/xenia-project/xenia/blob/master/src/xenia/kernel/xam/apps/xgi_app.cc)
