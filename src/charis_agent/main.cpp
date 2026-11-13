// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

// Charis Switch helper. Starts with Windows and stays out of sight; while Charis Switch itself
// is closed, it keeps this PC on the account and on the home network for the Switch app, and
// when the Switch wants to play it opens Charis Switch, which then answers the Switch.
// It never streams anything itself. "--stop" asks a running helper to quit (the installer).

#ifdef _WIN32

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include <windows.h>
#include <dpapi.h>
#include <shlobj.h>

#include "charis_stream/account.h"
#include "charis_stream/crypto.h"
#include "charis_stream/host.h"
#include "charis_stream/presence.h"
#include "charis_stream/sources.h"

namespace {

constexpr wchar_t AgentMutex[] = L"Local\\CharisSwitchAgent";
constexpr wchar_t AppMutex[] = L"Local\\CharisSwitchApp"; // held by Charis Switch while open
constexpr wchar_t QuitEvent[] = L"Local\\CharisSwitchAgentQuit";

std::wstring ExeDir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    return dir.substr(0, dir.find_last_of(L"\\/"));
}

bool IsDirectory(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// Same place Charis Switch keeps its data: a "user" folder next to it, or %APPDATA%\eden
std::wstring AccountFile() {
    const std::wstring portable = ExeDir() + L"\\user";
    if (IsDirectory(portable)) {
        return portable + L"\\charis\\account.ini";
    }
    wchar_t appdata[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata);
    return std::wstring(appdata) + L"\\eden\\charis\\account.ini";
}

std::string Base64Decode(const std::string& text) {
    static const std::string alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int value = 0;
    int bits = -8;
    for (const char c : text) {
        const size_t index = alphabet.find(c);
        if (index == std::string::npos) {
            continue;
        }
        value = (value << 6) + static_cast<int>(index);
        bits += 6;
        if (bits >= 0) {
            out += static_cast<char>((value >> bits) & 0xFF);
            bits -= 8;
        }
    }
    return out;
}

// Values Charis Switch sealed to this Windows user ("dpapi:...") or kept as "plain:..."
std::string Unseal(const std::string& text) {
    const size_t colon = text.find(':');
    if (colon == std::string::npos) {
        return {};
    }
    const std::string kind = text.substr(0, colon);
    const std::string data = Base64Decode(text.substr(colon + 1));
    if (kind == "plain") {
        return data;
    }
    if (kind != "dpapi") {
        return {};
    }
    DATA_BLOB in{static_cast<DWORD>(data.size()),
                 reinterpret_cast<BYTE*>(const_cast<char*>(data.data()))};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                            &out)) {
        return {};
    }
    std::string plain(reinterpret_cast<const char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return plain;
}

struct Settings {
    std::string user;
    std::string token;
    std::string auth_hex;
    bool create_pending = false;
    CharisStream::Key32 key{};
    std::string host_id;
    std::string pc_name;
    bool available = true;
    bool from_closed = true; // "open Charis Switch from the Switch when it is closed"
    bool Ready() const {
        bool has_key = false;
        for (const uint8_t byte : key) {
            has_key |= byte != 0;
        }
        return !user.empty() && has_key && host_id.size() == 16 && available && from_closed;
    }
};

// account.ini is written by Qt (QSettings, INI format): [group] then key=value, strings with
// special characters in double quotes
Settings LoadSettings() {
    Settings settings;
    std::ifstream file{std::filesystem::path{AccountFile()}};
    std::string group;
    std::map<std::string, std::string> values;
    std::string line;
    while (std::getline(file, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.size() > 2 && line.front() == '[' && line.back() == ']') {
            group = line.substr(1, line.size() - 2);
            continue;
        }
        const size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        std::string value = line.substr(equals + 1);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        values[group + "/" + line.substr(0, equals)] = value;
    }
    settings.user = values["account/user"];
    settings.token = Unseal(values["account/token"]);
    settings.auth_hex = Unseal(values["account/auth"]);
    settings.create_pending = values["account/create_pending"] == "true";
    const std::string key = Unseal(values["account/key"]);
    if (key.size() == settings.key.size()) {
        std::memcpy(settings.key.data(), key.data(), key.size());
    }
    settings.host_id = values["pc/id"];
    settings.pc_name = values["pc/name"];
    if (settings.pc_name.empty()) {
        wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
        DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
        GetComputerNameW(name, &size);
        char utf8[64] = {};
        WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
        settings.pc_name = utf8;
    }
    settings.available = values["pc/available"] != "false";
    settings.from_closed = values["pc/open_from_switch"] != "false";
    return settings;
}

FILETIME SettingsWritten() {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(AccountFile().c_str(), GetFileExInfoStandard, &data)) {
        return FILETIME{};
    }
    return data.ftLastWriteTime;
}

bool AppIsOpen() {
    const HANDLE handle = OpenMutexW(SYNCHRONIZE, FALSE, AppMutex);
    if (handle == nullptr) {
        return false;
    }
    CloseHandle(handle);
    return true;
}

bool OpenApp() {
    const std::wstring exe = ExeDir() + L"\\CharisSwitch.exe";
    std::wstring command = L"\"" + exe + L"\" --from-switch";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        ExeDir().c_str(), &startup, &process)) {
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

// Listens for the Switch while Charis Switch is closed. Returns when the app opened (by us or
// by the user), when the settings changed, or when asked to quit.
void Listen(const Settings& settings, HANDLE quit) {
    CharisStream::HostConfig config;
    config.name = settings.pc_name;
    config.key = settings.key;
    config.launcher = true;
    CharisStream::FromHex(settings.host_id, config.host_id.data(), config.host_id.size());
    std::atomic<bool> launch{false};
    CharisStream::HostCallbacks callbacks;
    callbacks.on_launch = [&launch](const std::string&) { launch = true; };
    CharisStream::Host host{config, callbacks};
    std::string error;
    if (!host.Start(error)) {
        return; // the port is busy (Charis Switch is starting): try again shortly
    }
    auto http = CharisStream::MakeSystemHttp();
    std::unique_ptr<CharisStream::Presence> presence;
    if (http) {
        CharisStream::Credentials credentials;
        credentials.user = settings.user;
        credentials.auth_hex = settings.auth_hex;
        credentials.token = settings.token;
        credentials.create_account = settings.create_pending;
        presence = std::make_unique<CharisStream::Presence>(*http, host, credentials,
                                                            settings.host_id, settings.pc_name);
        presence->SetIdlePollMs(30000);
        presence->Start();
    }
    const FILETIME written = SettingsWritten();
    while (true) {
        if (WaitForSingleObject(quit, 300) == WAIT_OBJECT_0) {
            break;
        }
        if (launch || AppIsOpen()) {
            break;
        }
        // Logging out or turning the option off in Charis Switch takes effect here
        const FILETIME now_written = SettingsWritten();
        if (CompareFileTime(&written, &now_written) != 0) {
            break;
        }
    }
    if (presence) {
        presence->Stop();
    }
    host.Stop(); // frees the port for Charis Switch
    if (launch && !AppIsOpen()) {
        OpenApp();
        // Give it time to start and take over before listening again
        for (int i = 0; i < 100 && !AppIsOpen(); ++i) {
            if (WaitForSingleObject(quit, 300) == WAIT_OBJECT_0) {
                return;
            }
        }
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR command_line, int) {
    if (command_line != nullptr && std::wcsstr(command_line, L"--stop") != nullptr) {
        if (const HANDLE quit = OpenEventW(EVENT_MODIFY_STATE, FALSE, QuitEvent)) {
            SetEvent(quit);
            CloseHandle(quit);
        }
        // Wait until it is gone, so the installer can replace the file
        for (int i = 0; i < 50; ++i) {
            const HANDLE running = OpenMutexW(SYNCHRONIZE, FALSE, AgentMutex);
            if (running == nullptr) {
                break;
            }
            CloseHandle(running);
            Sleep(100);
        }
        return 0;
    }
    const HANDLE single = CreateMutexW(nullptr, TRUE, AgentMutex);
    if (single == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0; // already running
    }
    const HANDLE quit = CreateEventW(nullptr, TRUE, FALSE, QuitEvent);
    while (WaitForSingleObject(quit, 0) != WAIT_OBJECT_0) {
        if (AppIsOpen()) {
            WaitForSingleObject(quit, 1000);
            continue;
        }
        const Settings settings = LoadSettings();
        if (!settings.Ready()) {
            WaitForSingleObject(quit, 5000);
            continue;
        }
        Listen(settings, quit);
        WaitForSingleObject(quit, 500);
    }
    CloseHandle(quit);
    ReleaseMutex(single);
    CloseHandle(single);
    return 0;
}

#else

int main() {
    return 0;
}

#endif
