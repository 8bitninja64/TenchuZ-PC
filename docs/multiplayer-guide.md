# Multiplayer guide

**Experimental System Link / online multiplayer is included since v0.3.7.**
Reports of both problems and successful sessions are welcome. Online play
currently needs a private VPN connecting the players; there is no built-in
matchmaking, account service, relay or automatic NAT traversal. Real internet
sessions and SteamOS graphics still need community verification.

## Install

1. Install the game normally from the
   [latest release](https://github.com/8bitninja64/TenchuZ-PC/releases/latest)
   and use the same version on every PC.
2. Run `TenchuZ.exe` (Windows) or `TenchuZ` (Linux) once and select your own disc
   image. Let preparation finish, create/load a character, then close the game.
   Do not copy a compiled game module from an older release.

Starting the game normally does not enable multiplayer; only the launchers below do.
Back up your saves before your first sessions. Do not share disc images, game
data, compiled modules or your saves in reports.

## Host or join

For LAN play, use each PC's LAN IPv4 address. For internet play, connect all
players through a private VPN and use its IPv4 addresses. All players must be
able to reach each other. Tailscale is one option; see its
[connection documentation](https://tailscale.com/docs/reference/connection-types).
VPN installation and authentication are separate from the game.

1. Windows: double-click **Multiplayer.cmd**. Linux/SteamOS Desktop Mode: run
   **`sh Multiplayer.sh`** in a terminal in the game folder.
2. Enter **this PC's** LAN/VPN IPv4 address.
3. The host leaves the host-address question blank. Everyone else enters the
   host's LAN/VPN IPv4 address.
4. Load a character and select **System Link**. The host chooses **Create Game**;
   guests choose **Search for Game**, then join the host.
5. The host selects a mission through the village leader. Each guest accepts
   that mission at the leader. Wait for everyone to be ready before leaving.

The launcher uses 30 FPS, keeps controller input enabled, and disables startup
update checks for this session. Keep everyone at 30 FPS. Allow the game
executable on the trusted LAN/VPN in your firewall: UDP 11000/11001/11004 and
TCP 11003. Do not disable your firewall or forward these ports to the public
internet. There is no built-in voice chat.

For SteamOS, use Desktop Mode first. To launch directly with known addresses:
`sh tools/launch_system_link.sh YOUR_IPV4 HOST_IPV4` (omit HOST_IPV4 to host).
SteamOS Game Mode/controller integration has not yet been verified.

## What to report

- Two-, three- and four-player sessions, especially Windows/Linux combinations.
- Village movement, mission entry, combat/enemy synchronization and completion.
- Whether everyone sees matching results and returns to the village.
- Guest/host exits, connection interruptions and rejoining.
- Real internet sessions: whether the VPN connection is direct or relayed.
- SteamOS launch, graphics, controls and mission transitions.

Use the [multiplayer report form](https://github.com/8bitninja64/TenchuZ-PC/issues/new?template=multiplayer.yml).
Include the exact game version, OS/GPU for each player, player count, host/guest
roles, LAN or VPN, mission, FPS settings, steps to reproduce, and expected versus
actual behavior. A short video helps with synchronization problems. Successful
sessions are welcome too: tell us exactly which steps worked.

Attach logs from both host and affected guests after closing the game. They are
in the game's data folder: `%APPDATA%\TenchuZRecomp` on Windows,
`~/.local/share/TenchuZRecomp` on Linux, or `user` beside the executable for a
portable install. Look for `TenchuZ.log` and, on Windows, `multiplayer.log`; the
Windows launcher also prints the folder. Review logs for local paths and IP
addresses before posting. Save a copy before the next run overwrites them.
First-start failures also have `module_build/build.log` there.

## What has been verified

Four Windows processes on one PC completed an assisted mission and returned.
A headless Linux host and rendered Windows guest entered a mission and returned.
Two headless Linux peers progressed through a mission under simulated latency,
then returned with jitter and packet loss. A restarted guest rejoined after a
simulated outage. v0.3.6 was checked with a two-player join on one Windows PC.
These tests used copied profiles and authorized test cheats; they do not
establish normal combat accuracy, hardware SteamOS support or real internet
reliability. See [detailed evidence](multiplayer.md).
