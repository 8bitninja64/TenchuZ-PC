# Multiplayer beta — community testing

**v0.4.0-beta.1 is an experimental System Link / online multiplayer beta.**
Please help test it and report both problems and successful sessions. Online
play currently needs a private VPN connecting the players; there is no built-in
matchmaking, account service, relay or automatic NAT traversal. Real internet
sessions and SteamOS graphics still need community verification.

## Install

1. Download the Windows ZIP or Linux archive from the
   [beta release](https://github.com/8bitninja64/TenchuZ-PC/releases/tag/v0.4.0-beta.1).
2. Extract into a **new folder**, keeping the included `portable.txt`. Use the
   same beta on every PC. Keep your stable installation and saves separate.
3. Run `TenchuZ.exe` (Windows) or `TenchuZ` (Linux) once and select your own disc
   image. Let preparation finish, create/load a character, then close the game.
   The beta keeps its prepared module and saves in its own `user` folder.
   Do not copy a compiled game module from an older release.

You may copy backed-up saves into the beta's `user` folder while the game is
closed. Start with a fresh beta profile if you are unsure which files to copy.
Do not share disc images, game data, compiled modules or your saves in reports.

## Host or join

For LAN play, use each PC's LAN IPv4 address. For internet testing, connect all
players through a private VPN and use its IPv4 addresses. All players must be
able to reach each other. Tailscale is one option; see its
[connection documentation](https://tailscale.com/docs/reference/connection-types).
VPN installation and authentication are separate from this beta.

1. Windows: double-click **Multiplayer.cmd**. Linux/SteamOS Desktop Mode: run
   **`sh Multiplayer.sh`** in a terminal in the extracted folder.
2. Enter **this PC's** LAN/VPN IPv4 address.
3. The host leaves the host-address question blank. Everyone else enters the
   host's LAN/VPN IPv4 address.
4. Load a character and select **System Link**. The host chooses **Create Game**;
   guests choose **Search for Game**, then join the host.
5. The host selects a mission through the village leader. Each guest accepts
   that mission at the leader. Wait for everyone to be ready before leaving.

The launcher uses 30 FPS, keeps controller input enabled, and disables startup
update checks for this session. Keep everyone at 30 FPS during initial tests.
Allow the beta executable on the trusted LAN/VPN in your firewall: UDP
11000/11001/11004 and TCP 11003. Do not disable your firewall or forward these
ports to the public internet. There is no built-in voice chat.

For SteamOS, test from Desktop Mode first. To launch directly with known addresses:
`sh tools/launch_system_link.sh YOUR_IPV4 HOST_IPV4` (omit HOST_IPV4 to host).
SteamOS Game Mode/controller integration has not yet been verified.

## What to test and report

- Two-, three- and four-player sessions, especially Windows/Linux combinations.
- Village movement, mission entry, combat/enemy synchronization and completion.
- Whether everyone sees matching results and returns to the village.
- Guest/host exits, connection interruptions and rejoining.
- Real internet sessions: whether the VPN connection is direct or relayed.
- SteamOS launch, graphics, controls and mission transitions.

Use the [Multiplayer beta report form](https://github.com/8bitninja64/TenchuZ-PC/issues/new?template=multiplayer-beta.yml).
Include the exact beta version, OS/GPU for each player, player count, host/guest
roles, LAN or VPN, mission, FPS settings, steps to reproduce, and expected versus
actual behavior. A short video helps with synchronization problems. Successful
tests are welcome too: tell us exactly which steps passed.

Attach logs from both host and affected guests after closing the game. Look in
the beta's `user` folder for `TenchuZ.log` and, on Windows, `multiplayer.log`.
Review logs for local paths and IP addresses before posting. Save a copy before
the next run overwrites them. First-start failures also have
`user/module_build/build.log`.

## What has been verified

Four Windows processes on one PC completed an assisted mission and returned.
A headless Linux host and rendered Windows guest entered a mission and returned.
Two headless Linux peers progressed through a mission under simulated latency,
then returned with jitter and packet loss. A restarted guest rejoined after a
simulated outage. These tests used copied profiles and authorized test cheats;
they do not establish normal combat accuracy, hardware SteamOS support or real
internet reliability. See [detailed evidence](multiplayer.md).

This is an opt-in prerelease. Stable users remain on v0.3.5. Download future
betas manually; the normal updater does not select prereleases.
