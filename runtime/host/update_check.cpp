// Updates (update_check.h). HTTPS through WinHTTP (Linux: the system's curl), no bundled TLS
// library; the package is unpacked with the system's tar (Windows 10 1803+ ships bsdtar, which
// reads .zip). The Linux side follows the NFL Blitz 2003 port's updater.
#include "host/update_check.h"

#include "platform/crypto.h"
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "kernel/kernel.h"

#ifndef TENCHU_VERSION
#define TENCHU_VERSION "dev"
#endif

namespace fs = std::filesystem;

namespace tz::host::update {
namespace {

// Never destroyed: a check still running when the program exits must not touch freed statics.
std::mutex& g_mutex = *new std::mutex;
Release& g_release = *new Release;
Install& g_install = *new Install;

#ifdef _WIN32
constexpr const char* kPackage = "TenchuZ-windows-x64.zip";
constexpr const char* kExe = "TenchuZ.exe";
#else
constexpr const char* kPackage = "TenchuZ-linux-x64.tar.xz";
constexpr const char* kExe = "TenchuZ";
#endif
constexpr const char* kBackup = ".old-update";

fs::path exeDir() {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return fs::path(std::wstring(buffer, length)).parent_path();
#else
    std::error_code error;
    return fs::read_symlink("/proc/self/exe", error).parent_path();
#endif
}

#ifndef _WIN32
// Runs a program (searched on PATH) and returns its exit status (-1: it could not run).
int runProgram(const std::vector<std::string>& arguments) {
    std::vector<char*> argv;  // made before fork: the child only execs
    for (const std::string& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid == 0) {
        execvp(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
#endif

std::string environment(const char* name) {
    char value[512]{};
    return GetEnvironmentVariableA(name, value, sizeof(value)) ? value : "";
}

bool validRepo(const std::string& repo) {  // owner/name
    if (repo.empty() || repo.size() > 200 || repo.find('/') == std::string::npos || repo.find('/') != repo.rfind('/'))
        return false;
    for (char c : repo)
        if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.' && c != '/') return false;
    return repo.front() != '/' && repo.back() != '/' && repo.find("..") == std::string::npos;
}

bool validUrl(const std::string& url) {  // characters a release URL uses
    if (url.size() > 2000) return false;
    for (char c : url)
        if (!isalnum((unsigned char)c) && !strchr(":/._~%-+=?&", c)) return false;
    return true;
}

std::string apiBase() {
    const std::string test = environment("TENCHU_UPDATE_API");
    return test.empty() ? "https://api.github.com" : test;
}

std::string sha256Hex(const std::vector<uint8_t>& data) { return crypto::sha256Hex(data.data(), data.size()); }

// ---- A small JSON reader (the release API's answer) --------------------------------------
struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    double number = 0;
    std::string text;
    std::vector<Json> items;
    std::vector<Json> members;  // an object's values, each named by its key
    std::string key;
    const Json* get(const char* key) const {
        for (const Json& value : members)
            if (value.key == key) return &value;
        return nullptr;
    }
    std::string string(const char* key) const {
        const Json* value = get(key);
        return value && value->type == String ? value->text : std::string();
    }
};

class JsonReader {
public:
    explicit JsonReader(const std::string& text) : p_(text.data()), end_(text.data() + text.size()) {}
    bool read(Json& out) {
        if (!value(out, 0)) return false;
        space();
        return p_ == end_;
    }

private:
    const char* p_;
    const char* end_;
    void space() {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
    }
    bool literal(const char* word) {
        const size_t length = strlen(word);
        if (size_t(end_ - p_) < length || memcmp(p_, word, length) != 0) return false;
        p_ += length;
        return true;
    }
    static void utf8(std::string& out, uint32_t code) {
        if (code < 0x80) {
            out += char(code);
        } else if (code < 0x800) {
            out += char(0xC0 | (code >> 6));
            out += char(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += char(0xE0 | (code >> 12));
            out += char(0x80 | ((code >> 6) & 0x3F));
            out += char(0x80 | (code & 0x3F));
        } else {
            out += char(0xF0 | (code >> 18));
            out += char(0x80 | ((code >> 12) & 0x3F));
            out += char(0x80 | ((code >> 6) & 0x3F));
            out += char(0x80 | (code & 0x3F));
        }
    }
    bool hex4(uint32_t& code) {
        if (end_ - p_ < 4) return false;
        code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = *p_++;
            code <<= 4;
            if (c >= '0' && c <= '9') code |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') code |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') code |= uint32_t(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& out) {
        if (p_ >= end_ || *p_ != '"') return false;
        ++p_;
        while (p_ < end_ && *p_ != '"') {
            if (*p_ != '\\') {
                out += *p_++;
                continue;
            }
            if (++p_ >= end_) return false;
            const char escape = *p_++;
            switch (escape) {
            case '"': case '\\': case '/': out += escape; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t code;
                if (!hex4(code)) return false;
                if (code >= 0xD800 && code < 0xDC00 && end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
                    p_ += 2;
                    uint32_t low;
                    if (!hex4(low)) return false;
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                }
                utf8(out, code);
                break;
            }
            default: return false;
            }
        }
        if (p_ >= end_) return false;
        ++p_;
        return true;
    }
    bool value(Json& out, int depth) {
        if (depth > 64) return false;
        space();
        if (p_ >= end_) return false;
        if (*p_ == '"') {
            out.type = Json::String;
            return string(out.text);
        }
        if (*p_ == '{') {
            out.type = Json::Object;
            ++p_;
            space();
            if (p_ < end_ && *p_ == '}') return ++p_, true;
            for (;;) {
                std::string name;
                space();
                if (!string(name)) return false;
                space();
                if (p_ >= end_ || *p_++ != ':') return false;
                Json member;
                if (!value(member, depth + 1)) return false;
                member.key = std::move(name);
                out.members.push_back(std::move(member));
                space();
                if (p_ < end_ && *p_ == ',') { ++p_; continue; }
                if (p_ < end_ && *p_ == '}') return ++p_, true;
                return false;
            }
        }
        if (*p_ == '[') {
            out.type = Json::Array;
            ++p_;
            space();
            if (p_ < end_ && *p_ == ']') return ++p_, true;
            for (;;) {
                Json item;
                if (!value(item, depth + 1)) return false;
                out.items.push_back(std::move(item));
                space();
                if (p_ < end_ && *p_ == ',') { ++p_; continue; }
                if (p_ < end_ && *p_ == ']') return ++p_, true;
                return false;
            }
        }
        if (literal("true")) { out.type = Json::Bool; out.number = 1; return true; }
        if (literal("false")) { out.type = Json::Bool; return true; }
        if (literal("null")) return true;
        char* stop = nullptr;
        const std::string number(p_, size_t((std::min)(end_ - p_, ptrdiff_t(64))));
        out.number = strtod(number.c_str(), &stop);
        if (stop == number.c_str()) return false;
        out.type = Json::Number;
        p_ += stop - number.c_str();
        return true;
    }
};

// GET url, the body streamed to `sink` (false: stop). Returns the HTTP status (0: no
// connection, `error` says why).
int httpGet(const std::string& url, bool api, const std::function<bool(const char*, size_t)>& sink, std::string& error) {
    if (!validUrl(url)) {
        error = "unexpected address " + url;
        return 0;
    }
#ifndef _WIN32
    // API answers are small: the status code is appended on its own line. Downloads stream the
    // body; -f turns an HTTP error into curl's exit status 22. LD_PRELOAD (Steam's overlay) is dropped.
    const std::string agent = std::string("TenchuZRecomp/") + TENCHU_VERSION;
    const std::string command =
        std::string("env -u LD_PRELOAD curl -sL --connect-timeout 10 ") +
        (api ? "--max-time 15 -w '\\n%{http_code}' " : "-f ") + "-A '" + agent + "' " +
        (api ? "-H 'Accept: application/vnd.github+json' -H 'X-GitHub-Api-Version: 2022-11-28' " : "") + "'" + url +
        "' 2>/dev/null";
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        error = "could not run curl";
        return 0;
    }
    std::string body;
    std::vector<char> buffer(1 << 16);
    bool stopped = false;
    for (size_t count; (count = fread(buffer.data(), 1, buffer.size(), pipe)) > 0;) {
        if (api) {
            if (body.size() < (1u << 20)) body.append(buffer.data(), count);
        } else if (!sink(buffer.data(), count)) {
            stopped = true;
            break;
        }
    }
    const int status = pclose(pipe);
    const int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (exitCode == 127) {
        error = "curl is not installed";
        return 0;
    }
    if (api) {
        const size_t newline = body.rfind('\n');
        const int code = newline == std::string::npos ? 0 : atoi(body.c_str() + newline + 1);
        if (newline != std::string::npos) body.resize(newline);
        if (!code) {
            error = "no connection to github.com";
            return 0;
        }
        sink(body.data(), body.size());
        return code;
    }
    if (stopped) return 200;
    if (exitCode == 22) {
        error = "the server refused the download";
        return 404;
    }
    if (exitCode != 0) {
        error = "the download failed (curl " + std::to_string(exitCode) + ")";
        return 0;
    }
    return 200;
#else
    const std::wstring wideUrl(url.begin(), url.end());  // ASCII (validUrl)
    wchar_t host[256] = {}, path[2048] = {}, extra[2048] = {};
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.lpszHostName = host;
    components.dwHostNameLength = 256;
    components.lpszUrlPath = path;
    components.dwUrlPathLength = 2048;
    components.lpszExtraInfo = extra;
    components.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &components)) {
        error = "unexpected address " + url;
        return 0;
    }
    const std::string agent8 = std::string("TenchuZRecomp/") + TENCHU_VERSION;
    const std::wstring agent(agent8.begin(), agent8.end());
    int code = 0;
    HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connection = session ? WinHttpConnect(session, host, components.nPort, 0) : nullptr;
    const std::wstring object = std::wstring(path) + extra;
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", object.c_str(), nullptr,
                                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                         components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                                   : nullptr;
    if (request) {
        WinHttpSetTimeouts(request, 10000, 10000, 15000, 30000);
        const wchar_t* headers = api ? L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n"
                                     : WINHTTP_NO_ADDITIONAL_HEADERS;
        if (WinHttpSendRequest(request, headers, api ? DWORD(-1L) : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD status = 0, length = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX);
            code = int(status);
            std::vector<char> buffer(1 << 16);
            for (DWORD got = 0;;) {
                if (!WinHttpReadData(request, buffer.data(), DWORD(buffer.size()), &got)) {
                    error = "the download was interrupted";
                    code = 0;
                    break;
                }
                if (!got || !sink(buffer.data(), got)) break;
            }
        }
    }
    if (!code && error.empty())
        error = "no connection to " + std::string(host, host + wcslen(host)) + " (error " +
                std::to_string(GetLastError()) + ")";
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    return code;
#endif
}

std::vector<long> versionParts(const std::string& version) {
    size_t i = 0;
    if (i < version.size() && (version[i] == 'v' || version[i] == 'V')) ++i;
    std::vector<long> parts;
    while (i < version.size() && isdigit((unsigned char)version[i])) {
        long n = 0;
        while (i < version.size() && isdigit((unsigned char)version[i])) n = n * 10 + (version[i++] - '0');
        parts.push_back(n);
        if (i < version.size() && version[i] == '.') ++i;
        else break;
    }
    return parts;
}

// Downloads come only from the repository's own release files (GitHub then redirects to its
// storage host), or from the test server.
bool downloadAllowed(const std::string& url) {
    const std::string test = environment("TENCHU_UPDATE_API");
    if (!test.empty()) return url.rfind(test + "/", 0) == 0;
    return url.rfind("https://github.com/" + std::string(repository()) + "/releases/download/", 0) == 0;
}

void setInstall(Install::State state, float fraction = 0, const std::string& error = {}) {
    std::lock_guard lock(g_mutex);
    g_install.state = state;
    g_install.fraction = fraction;
    g_install.error = error;
}

bool extract(const fs::path& archive, const fs::path& directory, std::string& error) {
#ifndef _WIN32
    std::error_code ignored;
    fs::create_directories(directory, ignored);
    if (runProgram({"tar", "-xJf", archive.string(), "-C", directory.string()}) == 0) return true;
    // Minimal installs: tar without xz. The installed package's own unpacker (Python's tarfile).
    if (runProgram({"python3", "-I", (exeDir() / "tools" / "compile_module.py").string(), "unpack", archive.string(),
                    directory.string()}) == 0)
        return true;
    error = "the package could not be unpacked (tar and python3 failed)";
    return false;
#else
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    const fs::path tar = fs::path(system) / "tar.exe";
    std::error_code ignored;
    if (!fs::exists(tar, ignored)) {
        error = "this Windows has no tar.exe (Windows 10 1803 or newer is needed)";
        return false;
    }
    std::wstring command = L"\"" + tar.wstring() + L"\" -xf \"" + archive.wstring() + L"\" -C \"" + directory.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(tar.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &process)) {
        error = "could not run tar.exe";
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (code) {
        error = "the package could not be unpacked (tar " + std::to_string(code) + ")";
        return false;
    }
    return true;
#endif
}

Release checkNow() {
    Release release;
    const std::string repo = repository();
    if (!validRepo(repo)) {
        release.state = Release::Failed;
        release.error = "invalid repository name";
        return release;
    }
    std::string body, error;
    const int code = httpGet(apiBase() + "/repos/" + repo + "/releases/latest", true, [&](const char* data, size_t n) {
        if (body.size() < (8u << 20)) body.append(data, n);
        return true;
    }, error);
    if (code == 404) {
        release.state = Release::NoReleases;
        return release;
    }
    if (code != 200) {
        release.state = Release::Failed;
        release.error = code ? "GitHub answered " + std::to_string(code) +
                                   (code == 403 || code == 429 ? " (rate limit; try later)" : "")
                             : error;
        return release;
    }
    Json json;
    if (!JsonReader(body).read(json) || json.type != Json::Object || json.string("tag_name").empty()) {
        release.state = Release::Failed;
        release.error = "unexpected answer from GitHub";
        return release;
    }
    release.latest = json.string("tag_name");
    release.page = json.string("html_url");
    if (release.page.rfind("https://", 0) != 0) release.page = "https://github.com/" + repo + "/releases/latest";
    release.notes = json.string("body");
    if (const Json* assets = json.get("assets"); assets && assets->type == Json::Array)
        for (const Json& asset : assets->items) {
            if (asset.type != Json::Object || asset.string("name") != kPackage) continue;
            release.asset.name = kPackage;
            release.asset.url = asset.string("browser_download_url");
            if (const Json* size = asset.get("size"); size && size->type == Json::Number && size->number >= 0)
                release.asset.size = uint64_t(size->number);
            const std::string digest = asset.string("digest");  // "sha256:<hex>"
            if (digest.rfind("sha256:", 0) == 0) release.asset.sha256 = digest.substr(7);
        }
    release.state = versionNewer(release.latest, TENCHU_VERSION) ? Release::Available : Release::UpToDate;
    return release;
}

Install installNow(const Release& release) {
    auto fail = [](const std::string& error) {
        setInstall(Install::Failed, 0, error);
        logf("[update] failed: %s", error.c_str());
        return installStatus();
    };
    std::string why, error;
    if (!canInstall(release, why)) return fail(why);
    const Asset& asset = release.asset;
    if (!downloadAllowed(asset.url)) return fail("unexpected download address");
    std::string expected = asset.sha256;
    for (char& c : expected) c = char(tolower((unsigned char)c));
    if (expected.size() != 64) return fail("the release publishes no SHA-256 checksum for " + asset.name);
    const fs::path directory = exeDir(), work = directory / ".update";
    std::error_code ignored;
    fs::remove_all(work / "new", ignored);
    fs::create_directories(work / "new", ignored);
    const fs::path archive = work / asset.name;

    logf("[update] downloading %s (%llu bytes)", asset.url.c_str(), (unsigned long long)asset.size);
    setInstall(Install::Downloading, 0);
    std::vector<uint8_t> data;
    data.reserve(size_t(asset.size));
    const int code = httpGet(asset.url, false, [&](const char* bytes, size_t n) {
        data.insert(data.end(), bytes, bytes + n);
        if (asset.size) setInstall(Install::Downloading, (std::min)(1.0f, float(data.size()) / float(asset.size)));
        return data.size() <= (size_t(1) << 30);
    }, error);
    if (code != 200) return fail(code ? "the download failed (HTTP " + std::to_string(code) + ")" : error);

    setInstall(Install::Verifying, 1);
    const std::string got = sha256Hex(data);
    if (got != expected)
        return fail("the download is damaged (SHA-256 " + got.substr(0, 12) + "... instead of " +
                    expected.substr(0, 12) + "...)");
    {
        std::ofstream file(archive, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        if (!file) return fail("cannot write " + tz::utf8(archive));
    }
    data.clear();
    data.shrink_to_fit();

    // The package holds the program at its top (or in one folder).
    setInstall(Install::Extracting, 1);
    if (!extract(archive, work / "new", error)) return fail(error);
    fs::path root;
    if (fs::is_regular_file(work / "new" / kExe, ignored)) {
        root = work / "new";
    } else {
        for (const auto& entry : fs::directory_iterator(work / "new", ignored))
            if (entry.is_directory() && fs::is_regular_file(entry.path() / kExe, ignored)) {
                root = entry.path();
                break;
            }
    }
    if (root.empty()) return fail(std::string("the package does not contain ") + kExe);

    // Replace the files; every replaced file is kept as *.old-update until the next start.
    setInstall(Install::Installing, 1);
    std::vector<std::pair<fs::path, fs::path>> moved;  // (destination, its backup or empty)
    auto rollback = [&] {
        for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
            std::error_code e;
            fs::remove(it->first, e);
            if (!it->second.empty()) fs::rename(it->second, it->first, e);
        }
    };
    std::vector<fs::path> files;
    std::error_code walk;
    for (auto it = fs::recursive_directory_iterator(root, walk); it != fs::recursive_directory_iterator(); it.increment(walk)) {
        if (walk) break;
        if (it->is_regular_file()) files.push_back(fs::relative(it->path(), root));
    }
    for (const fs::path& relative : files) {
        const fs::path source = root / relative, destination = directory / relative;
        std::error_code e;
        fs::create_directories(destination.parent_path(), e);
        fs::path backup;
        if (fs::exists(destination, e)) {
            backup = destination.native() + fs::path(kBackup).native();
            fs::remove(backup, e);
            fs::rename(destination, backup, e);
            if (e) {
                rollback();
                return fail("cannot replace " + tz::utf8(destination) + " (" + e.message() + ")");
            }
        }
        fs::rename(source, destination, e);
        if (e) {
            if (!backup.empty()) {
                std::error_code e2;
                fs::rename(backup, destination, e2);
            }
            rollback();
            return fail("cannot install " + tz::utf8(destination) + " (" + e.message() + ")");
        }
        moved.emplace_back(destination, backup);
    }
    {
        std::ofstream list(work / "cleanup.txt", std::ios::app);
        for (const auto& [destination, backup] : moved)
            if (!backup.empty()) list << tz::utf8(backup) << "\n";
    }
    fs::remove_all(work / "new", ignored);
    fs::remove(archive, ignored);
    logf("[update] installed %s (%zu files)", release.latest.c_str(), moved.size());
    setInstall(Install::Done, 1);
    return installStatus();
}

}  // namespace

const char* version() { return TENCHU_VERSION; }

const char* repository() {
    static const std::string repo = [] {
        const std::string test = environment("TENCHU_UPDATE_REPO");
        return test.empty() ? std::string("8bitninja64/TenchuZ-PC") : test;
    }();
    return repo.c_str();
}

bool versionNewer(const std::string& latest, const std::string& current) {
    std::vector<long> a = versionParts(latest), b = versionParts(current);
    if (a.empty()) return false;  // not a version tag
    a.resize((std::max)(a.size(), b.size()));
    b.resize(a.size());
    return a > b;
}

void checkStart() {
    {
        std::lock_guard lock(g_mutex);
        if (g_release.state == Release::Checking) return;
        g_release = {};
        g_release.state = Release::Checking;
    }
    std::thread([] {
        const Release release = checkNow();
        logf("[update] check: state %d, latest %s, package %s %s", int(release.state), release.latest.c_str(),
             release.asset.name.c_str(), release.error.c_str());
        std::lock_guard lock(g_mutex);
        g_release = release;
    }).detach();  // a slow network never delays the game
}

Release checkStatus() {
    std::lock_guard lock(g_mutex);
    return g_release;
}

bool canInstall(const Release& release, std::string& why) {
    if (release.state != Release::Available) {
        why = "no newer release";
        return false;
    }
    if (release.asset.name.empty()) {
        why = "the release has no Windows package";
        return false;
    }
    const fs::path directory = exeDir();
    std::error_code ignored;
    if (!fs::exists(directory / "tools" / "make_module.ps1", ignored) || !fs::exists(directory / kExe, ignored)) {
        why = "this is not an installed release (a development build)";
        return false;
    }
    const fs::path probe = directory / ".update-probe";
    {
        std::ofstream file(probe);
        if (!file) {
            why = "the program folder is not writable";
            return false;
        }
    }
    fs::remove(probe, ignored);
    return true;
}

void installStart(const Release& release) {
    {
        std::lock_guard lock(g_mutex);
        if (g_install.state != Install::Idle && g_install.state != Install::Failed) return;
        g_install = {};
        g_install.state = Install::Downloading;
    }
    std::thread([release] { installNow(release); }).detach();
}

Install installStatus() {
    std::lock_guard lock(g_mutex);
    return g_install;
}

bool restart() {
    const fs::path exe = exeDir() / kExe;
#ifndef _WIN32
    // The original arguments (/proc/self/cmdline) with the new executable.
    std::vector<std::string> arguments;
    {
        std::ifstream file("/proc/self/cmdline", std::ios::binary);
        for (std::string argument; std::getline(file, argument, '\0');) arguments.push_back(argument);
    }
    const std::string path = exe.string();
    std::vector<char*> argv;  // made before fork: the child only execs
    argv.push_back(const_cast<char*>(path.c_str()));
    for (size_t i = 1; i < arguments.size(); ++i) argv.push_back(arguments[i].data());
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid == 0) {
        setsid();
        usleep(500000);  // let this process release the window and audio device first
        execv(path.c_str(), argv.data());
        _exit(127);
    }
    return pid > 0;
#else
    // The original command line (arguments included) with the new executable.
    std::wstring command = L"\"" + exe.wstring() + L"\"";
    int count = 0;
    if (LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count)) {
        for (int i = 1; i < count; ++i) command += L" \"" + std::wstring(arguments[i]) + L"\"";
        LocalFree(arguments);
    }
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    const std::wstring directory = exeDir().wstring();
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, directory.c_str(), &startup,
                        &process))
        return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
#endif
}

void cleanup() {
    const fs::path work = exeDir() / ".update", list = work / "cleanup.txt";
    std::error_code ignored;
    if (!fs::exists(work, ignored)) return;
    std::vector<std::string> left;
    {
        std::ifstream file(list);
        for (std::string line; std::getline(file, line);) {
            if (line.size() < strlen(kBackup) || line.compare(line.size() - strlen(kBackup), std::string::npos, kBackup) != 0)
                continue;  // only the backups this program made
            const fs::path path(std::u8string(line.begin(), line.end()));
            std::error_code e;
            fs::remove(path, e);
            std::error_code e2;
            if (e && fs::exists(path, e2)) left.push_back(line);  // still in use (the old process is exiting)
        }
    }
    if (left.empty()) {
        fs::remove_all(work, ignored);
        return;
    }
    std::ofstream file(list, std::ios::trunc);
    for (const auto& line : left) file << line << "\n";
}

void openPage(const std::string& url) {
    if (url.rfind("https://", 0) != 0 || !validUrl(url)) return;
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    if (fork() == 0) {
        execlp("xdg-open", "xdg-open", url.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
#endif
}

}  // namespace tz::host::update
