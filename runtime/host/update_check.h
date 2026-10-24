#pragma once
// Updates from the project's GitHub releases (F1 > Update), ported from the NFL Blitz 2003
// port's updater. Check: one anonymous HTTPS GET to api.github.com for the latest release; its
// tag is compared with this build's version (TENCHU_VERSION, set by the release workflow).
// Install (only when the player chooses it): downloads TenchuZ-windows-x64.zip from that
// release, checks its SHA-256 against the digest GitHub publishes (no checksum, no install),
// unpacks it with Windows' tar and replaces the program files in place. Settings next to the
// executable, saves and the compiled game module (%APPDATA%) are not touched; the module stamp
// (app/boot.cpp) rebuilds the game code when the new release's build kit changed. Replaced
// files are renamed to *.old-update first (a running program's files can be renamed), restored
// if anything fails and removed at the next start. Then the program restarts.
// Test hooks: TENCHU_UPDATE_API=http://127.0.0.1:PORT replaces https://api.github.com (downloads
// are then accepted only from that server), TENCHU_UPDATE_REPO=owner/name.
#include <cstdint>
#include <string>

namespace tz::host::update {

struct Asset {
    std::string name, url, sha256;
    uint64_t size = 0;
};

struct Release {
    enum State { Idle, Checking, UpToDate, Available, NoReleases, Failed } state = Idle;
    std::string latest;  // the release's tag, e.g. "v0.2.0"
    std::string error;   // Failed: what went wrong, for the player
    std::string page;    // the release's web page
    std::string notes;   // the release notes (Markdown as written)
    Asset asset;         // the Windows package (name empty: the release has none)
};

struct Install {
    enum State { Idle, Downloading, Verifying, Extracting, Installing, Done, Failed } state = Idle;
    float fraction = 0;  // Downloading: 0..1
    std::string error;
};

const char* version();     // this build's version ("dev" outside releases)
const char* repository();  // "owner/name"
void checkStart();         // background check (ignored while one runs)
Release checkStatus();
bool versionNewer(const std::string& latest, const std::string& current);
// Whether `release` can be installed in place here; `why` says why not (the menu then offers
// the release page).
bool canInstall(const Release& release, std::string& why);
void installStart(const Release& release);  // background install
Install installStatus();
// Starts the program's executable again with this process's arguments; the caller then quits.
bool restart();
// At startup: removes the files the last install replaced.
void cleanup();
// Opens a web page in the browser.
void openPage(const std::string& url);

}  // namespace tz::host::update
