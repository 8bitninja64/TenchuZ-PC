#include "boot.h"

#include <windows.h>
#include <bcrypt.h>
#include <commdlg.h>
#include <shlobj.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "host/settings.h"
#include "kernel/kernel.h"
#include "kernel/module_loader.h"
#include "kernel/xdvdfs.h"

namespace tz {
namespace {

// Tenchu Z (USA, Europe) disc default.xex (also checked by tools/recompile.py).
constexpr char kSupportedXex[] = "c727debeaad474dfcf8517202eebc388f9e51705ce3b01499486fa5ff47f9f06";
constexpr wchar_t kTitle[] = L"Tenchu Z";

// The files of a release that make_module.ps1 builds the module from: a module built by an
// older release is rebuilt after an update. Missing files mean a development build, which
// never builds modules itself (tools/build.ps1 does).
constexpr const char* kKitFiles[] = {
    "tools/XenonRecomp.exe", "tools/recompile.py", "tools/xex_image.py", "tools/compile_module.py",
    "tools/make_module.ps1", "tools/vsenv.ps1", "tools/python.ps1", "tools/xbox/xboxkrnl_table.inc",
    "tools/xbox/xam_table.inc", "config/tenchuz.toml", "config/tenchuz_switch_tables.toml",
    "config/tenchuz_switch_tables_manual.toml", "config/overrides.txt", "runtime/guest/ppc_context.template.h",
    "runtime/guest/ppc_config.h", "runtime/guest/module_abi.h", "runtime/guest/module_rt.cpp",
    "third_party/simde/x86/avx.h"};

std::filesystem::path executableDirectory() {
    wchar_t executable[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    return std::filesystem::path(executable).parent_path();
}

std::filesystem::path userDirectory() {
    std::filesystem::path directory = executableDirectory() / "user";
    wchar_t* appData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        directory = std::filesystem::path(appData) / L"TenchuZRecomp";
        CoTaskMemFree(appData);
    }
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return directory;
}

std::wstring widen(const std::string& text) {
    std::wstring result(MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), result.data(), int(result.size()));
    return result;
}

void showError(const std::wstring& message) { MessageBoxW(nullptr, message.c_str(), kTitle, MB_OK | MB_ICONERROR); }

std::string sha256(const std::vector<uint8_t>& data) {
    uint8_t digest[32]{};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
            BCryptHashData(hash, const_cast<PUCHAR>(data.data()), ULONG(data.size()), 0);
            BCryptFinishHash(hash, digest, sizeof(digest), 0);
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    std::string text;
    char cell[3];
    for (const uint8_t byte : digest) {
        snprintf(cell, sizeof(cell), "%02x", byte);
        text += cell;
    }
    return text;
}

// FNV-1a 64 over each kit file's name and bytes; "" without a complete kit.
std::string kitStamp() {
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&](const char* bytes, size_t size) {
        for (size_t i = 0; i < size; ++i) hash = (hash ^ uint8_t(bytes[i])) * 1099511628211ull;
    };
    for (const char* name : kKitFiles) {
        std::ifstream file(executableDirectory() / std::filesystem::u8path(name), std::ios::binary);
        if (!file) return {};
        mix(name, std::strlen(name) + 1);
        const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        mix(bytes.data(), bytes.size());
    }
    char text[17];
    snprintf(text, sizeof(text), "%016llx", (unsigned long long)hash);
    return text;
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// A disc image or an extracted folder with the supported default.xex.
bool openGame(const std::filesystem::path& path, GameSource& source, std::wstring& problem) {
    source = {};
    std::error_code error;
    if (std::filesystem::is_directory(path, error)) {
        std::ifstream file(path / "default.xex", std::ios::binary);
        if (!file) {
            problem = L"'" + path.wstring() + L"' has no default.xex.";
            return false;
        }
        source.xex.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        source.folder = path;
    } else {
        std::string reason;
        source.disc = DiscImage::open(path, reason);
        if (!source.disc) {
            problem = widen(reason) + L": '" + path.wstring() + L"'.";
            return false;
        }
        if (!source.disc->readFile("default.xex", source.xex)) {
            problem = L"'" + path.wstring() + L"' has no default.xex: it is not an Xbox 360 game disc.";
            return false;
        }
    }
    const std::string hash = sha256(source.xex);
    if (hash != kSupportedXex) {
        problem = L"'" + path.wstring() + L"' is not Tenchu Z (USA, Europe).\n\ndefault.xex SHA-256 " + widen(hash) +
                  L"\nexpected " + widen(kSupportedXex);
        return false;
    }
    return true;
}

std::filesystem::path pickDisc(const std::filesystem::path& start) {
    wchar_t file[MAX_PATH]{};
    const std::wstring folder = start.empty() ? std::wstring() : start.parent_path().wstring();
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.lpstrTitle = L"Select your Tenchu Z (USA, Europe) Xbox 360 disc image";
    dialog.lpstrFilter = L"Xbox 360 disc image (*.iso)\0*.iso\0All files\0*.*\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrInitialDir = folder.empty() ? nullptr : folder.c_str();
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    return GetOpenFileNameW(&dialog) ? std::filesystem::path(file) : std::filesystem::path();
}

// The extracted disc next to the executable or above it (the repository layout).
std::filesystem::path findGameFolder() {
    for (auto directory = executableDirectory(); !directory.empty(); directory = directory.parent_path()) {
        if (std::filesystem::exists(directory / "game" / "default.xex")) return directory / "game";
        if (directory == directory.parent_path()) break;
    }
    return {};
}

// The disc: the command line (a file or folder), TENCHU_DISC, the remembered disc, an
// extracted folder (development), or the player's choice.
bool chooseGame(int argc, wchar_t** argv, GameSource& source) {
    std::wstring problem;
    std::filesystem::path given;
    for (int i = 1; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--disc" && i + 1 < argc) given = argv[++i];
        else if (argv[i][0] != L'-') given = argv[i];
    }
    wchar_t value[MAX_PATH]{};
    if (given.empty() && GetEnvironmentVariableW(L"TENCHU_DISC", value, MAX_PATH)) given = value;
    if (!given.empty()) {
        if (openGame(given, source, problem)) return true;
        showError(problem);
        return false;
    }
    auto& settings = host::editableSettings();
    std::filesystem::path remembered;
    try {
        remembered = std::filesystem::u8path(settings.disc);
    } catch (const std::exception&) {
    }
    std::error_code error;
    if (!remembered.empty() && std::filesystem::exists(remembered, error) && openGame(remembered, source, problem))
        return true;
    if (const auto folder = findGameFolder(); !folder.empty() && openGame(folder, source, problem)) return true;
    std::filesystem::path start = remembered;
    for (;;) {
        const std::filesystem::path chosen = pickDisc(start);
        if (chosen.empty()) {
            showError(L"No Tenchu Z disc image was selected.");
            return false;
        }
        if (openGame(chosen, source, problem)) {
            settings.disc = utf8(chosen);
            host::saveSettings();
            return true;
        }
        showError(problem);
        start = chosen;
    }
}

bool tryModule(const std::filesystem::path& path, const std::string& xexHash, bool report) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) return false;
    std::string why;
    if (loadGameModule(path, why, xexHash)) return true;
    logf("[boot] %s", why.c_str());
    if (report) showError(L"The game module could not be loaded:\n" + widen(why));
    return false;
}

// Runs tools/make_module.ps1 in its own console window and waits for it.
bool buildModule(const std::filesystem::path& xexPath, const std::filesystem::path& out,
                 const std::filesystem::path& work) {
    const auto script = executableDirectory() / "tools" / "make_module.ps1";
    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, MAX_PATH);
    const std::wstring powershell = std::wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    std::wstring command = L"\"" + powershell + L"\" -NoProfile -ExecutionPolicy Bypass -File \"" + script.wstring() +
                           L"\" -Xex \"" + xexPath.wstring() + L"\" -Out \"" + out.wstring() + L"\" -Work \"" +
                           work.wstring() + L"\" -PauseOnError";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    const std::wstring directory = executableDirectory().wstring();
    // The build ends with the game: closing or killing it also stops the compile.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    if (!CreateProcessW(powershell.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE | CREATE_SUSPENDED,
                        nullptr, directory.c_str(), &startup, &process)) {
        if (job) CloseHandle(job);
        showError(L"Cannot start PowerShell to compile the game code (error " + std::to_wstring(GetLastError()) + L").");
        return false;
    }
    if (job) AssignProcessToJobObject(job, process.hProcess);
    ResumeThread(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (job) CloseHandle(job);
    if (exitCode != 0) {
        showError(L"Compiling the game code failed. The log is in\n" + (work / "build.log").wstring());
        return false;
    }
    return true;
}

// A module translated from this default.xex: TENCHU_MODULE, the one next to the executable
// (a source build), or the one this release built in the user folder; otherwise build one.
bool loadModule(const GameSource& source) {
    const std::string xexHash = sha256(source.xex);
    wchar_t value[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"TENCHU_MODULE", value, MAX_PATH)) {
        if (tryModule(value, xexHash, true)) return true;
        showError(L"TENCHU_MODULE does not name a usable game module:\n" + std::wstring(value));
        return false;
    }
    if (tryModule(executableDirectory() / L"tenchu_game.dll", xexHash, false)) return true;

    const std::string stamp = kitStamp();
    if (stamp.empty()) {
        showError(L"This build has no game module and cannot compile one.\n\nBuild it with tools\\build.ps1, "
                  L"or use a release of Tenchu Z Recomp.");
        return false;
    }
    const auto user = userDirectory();
    const auto built = user / L"tenchu_game.dll";
    const auto stampFile = user / L"tenchu_game.dll.stamp";
    if (readText(stampFile) == stamp && tryModule(built, xexHash, false)) return true;

    // One build at a time.
    const auto work = user / L"module_build";
    std::error_code error;
    std::filesystem::create_directories(work, error);
    HANDLE lock = CreateFileW((work / L"lock").c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (lock == INVALID_HANDLE_VALUE) {
        showError(L"Another copy of Tenchu Z is compiling the game code. Start the game again when it has finished.");
        return false;
    }
    struct Unlock { HANDLE handle; ~Unlock() { CloseHandle(handle); } } unlock{lock};

    if (!GetEnvironmentVariableW(L"TENCHU_AUTO_BUILD", nullptr, 0) &&
        MessageBoxW(nullptr,
                    L"Tenchu Z needs to compile the game code from your disc. This is done once and takes "
                    L"a few minutes (about 2 on an 8-core PC, longer on fewer cores).\n\n"
                    L"Requirements: Python 3.11 or newer, and Visual Studio 2022 Build Tools with "
                    L"'Desktop development with C++' and 'C++ Clang tools for Windows'.\n\nCompile now?",
                    L"Tenchu Z - first start", MB_YESNO | MB_ICONINFORMATION) != IDYES)
        return false;

    const auto xexPath = work / L"default.xex";
    {
        std::ofstream file(xexPath, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(source.xex.data()), std::streamsize(source.xex.size()));
        if (!file) {
            showError(L"Cannot write " + xexPath.wstring());
            return false;
        }
    }
    std::filesystem::remove(stampFile, error);
    if (!buildModule(xexPath, built, work)) return false;
    if (!tryModule(built, xexHash, true)) return false;
    std::ofstream(stampFile, std::ios::binary | std::ios::trunc) << stamp;
    std::filesystem::remove(xexPath, error);
    return true;
}

}  // namespace

bool prepareGame(int argc, wchar_t** argv, GameSource& source) {
    if (!chooseGame(argc, argv, source)) return false;
    logf("[boot] game: %s", utf8(source.disc ? source.disc->path() : source.folder).c_str());
    return loadModule(source);
}

}  // namespace tz
