// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

#include <curl/curl.h>

#ifdef __SWITCH__
#include <sys/stat.h>
#include <switch.h>
#endif

#include "charis_stream/crypto.h"
#include "platform.h"

namespace Platform {

namespace {

std::string StorePath() {
#ifdef __SWITCH__
    mkdir("sdmc:/config", 0777);
    mkdir("sdmc:/config/charis-stream", 0777);
    return "sdmc:/config/charis-stream/conta.ini";
#else
    const char* custom = std::getenv("CHARIS_STORE");
    return custom != nullptr ? custom : "charis-stream-conta.ini";
#endif
}

std::atomic<bool> http_cancelled{false};

int Progress(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return http_cancelled ? 1 : 0; // non-zero aborts the transfer
}

size_t Collect(char* data, size_t size, size_t count, void* user) {
    auto* out = static_cast<std::string*>(user);
    if (out->size() > 256 * 1024) {
        return 0;
    }
    out->append(data, size * count);
    return size * count;
}

class CurlHttp final : public CharisStream::Http {
public:
    bool Post(const std::string& url, const std::string& body, std::string& response) override {
        response.clear();
        if (http_cancelled) {
            return false;
        }
        CURL* curl = curl_easy_init();
        if (curl == nullptr) {
            return false;
        }
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Collect);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "CharisStream/2.0");
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, Progress);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        const CURLcode result = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        return result == CURLE_OK;
    }
    void Cancel() override {
        http_cancelled = true;
    }
};

std::mutex log_mutex;
FILE* log_file = nullptr;

std::string LogPath(bool previous) {
#ifdef __SWITCH__
    return previous ? "sdmc:/config/charis-stream/log-anterior.txt"
                    : "sdmc:/config/charis-stream/log.txt";
#else
    const char* custom = std::getenv("CHARIS_LOG");
    const std::string base = custom != nullptr ? custom : "charis-stream-log.txt";
    return previous ? base + ".old" : base;
#endif
}

} // namespace

void Log(const char* format, ...) {
    std::scoped_lock lock{log_mutex};
    if (log_file == nullptr) {
        return;
    }
    const std::time_t now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%H:%M:%S", std::localtime(&now));
    std::fprintf(log_file, "[%s] ", stamp);
    va_list args;
    va_start(args, format);
    std::vfprintf(log_file, format, args);
    va_end(args);
    std::fputc('\n', log_file);
    std::fflush(log_file);
}

namespace {
std::string self_path;

size_t WriteToFile(char* data, size_t size, size_t count, void* user) {
    return std::fwrite(data, size, count, static_cast<FILE*>(user)) * size;
}

struct DownloadProgress {
    const std::function<void(int64_t, int64_t)>* callback;
};

int DownloadProgressCallback(void* user, curl_off_t total, curl_off_t received, curl_off_t,
                             curl_off_t) {
    if (http_cancelled) {
        return 1;
    }
    auto* progress = static_cast<DownloadProgress*>(user);
    if (progress->callback != nullptr && *progress->callback) {
        (*progress->callback)(received, total);
    }
    return 0;
}

CURL* NewGet(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        return nullptr;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CharisStream/2.0");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    return curl;
}
} // namespace

bool FetchText(const std::string& url, std::string& out) {
    out.clear();
    if (http_cancelled) {
        return false;
    }
    CURL* curl = NewGet(url);
    if (curl == nullptr) {
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, Progress);
    const CURLcode result = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return result == CURLE_OK;
}

bool DownloadFile(const std::string& url, const std::string& path,
                  const std::function<void(int64_t, int64_t)>& progress) {
    if (http_cancelled) {
        return false;
    }
    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    CURL* curl = NewGet(url);
    bool ok = false;
    if (curl != nullptr) {
        DownloadProgress state{&progress};
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToFile);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, DownloadProgressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L); // stalled for 30 s: give up
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
        ok = curl_easy_perform(curl) == CURLE_OK;
        curl_easy_cleanup(curl);
    }
    ok = std::fclose(file) == 0 && ok;
    if (!ok) {
        std::remove(path.c_str());
    }
    return ok;
}

std::string SelfPath() {
    return self_path;
}

void Init(int argc, char** argv) {
#ifdef __SWITCH__
    // hbloader passes the .nro's own path as the first argument
    if (argc > 0 && argv[0] != nullptr && std::strstr(argv[0], ".nro") != nullptr) {
        self_path = argv[0];
    }
#else
    (void)argc;
    (void)argv;
    if (const char* custom = std::getenv("CHARIS_SELF_PATH")) {
        self_path = custom;
    }
#endif
#ifdef __SWITCH__
    socketInitializeDefault();
    romfsInit();
    setsysInitialize();
#endif
    curl_global_init(CURL_GLOBAL_DEFAULT);
    StorePath(); // makes the folder
    std::rename(LogPath(false).c_str(), LogPath(true).c_str());
    log_file = std::fopen(LogPath(false).c_str(), "w");
}

void Exit() {
    Log("app fechado");
    {
        std::scoped_lock lock{log_mutex};
        if (log_file != nullptr) {
            std::fclose(log_file);
            log_file = nullptr;
        }
    }
    curl_global_cleanup();
#ifdef __SWITCH__
    KeepAwake(false);
    setsysExit();
    romfsExit();
    socketExit();
#endif
}

bool Running() {
#ifdef __SWITCH__
    return appletMainLoop();
#else
    return true;
#endif
}

Saved Load() {
    Saved saved;
    std::ifstream file(StorePath());
    std::map<std::string, std::string> values;
    std::string line;
    while (std::getline(file, line)) {
        const size_t equals = line.find('=');
        if (equals != std::string::npos) {
            values[line.substr(0, equals)] = line.substr(equals + 1);
        }
    }
    saved.user = values["user"];
    saved.token = values["token"];
    saved.key_hex = values["key"];
    saved.auth_hex = values["auth"];
    saved.create_pending = values["create_pending"] == "1";
    saved.client_id = values["client"];
    saved.last_host = values["last_host"];
    saved.quality = std::atoi(values["quality"].c_str());
    if (saved.quality < 0 || saved.quality > 2) {
        saved.quality = 0;
    }
    uint8_t id[8];
    if (saved.client_id.size() != 16 || !CharisStream::FromHex(saved.client_id, id, sizeof(id))) {
        CharisStream::RandomBytes(id, sizeof(id));
        saved.client_id = CharisStream::ToHex(id, sizeof(id));
        Store(saved);
    }
    return saved;
}

void Store(const Saved& saved) {
    std::ofstream file(StorePath(), std::ios::trunc);
    file << "user=" << saved.user << "\n"
         << "token=" << saved.token << "\n"
         << "key=" << saved.key_hex << "\n"
         << "auth=" << saved.auth_hex << "\n"
         << "create_pending=" << (saved.create_pending ? "1" : "0") << "\n"
         << "client=" << saved.client_id << "\n"
         << "last_host=" << saved.last_host << "\n"
         << "quality=" << saved.quality << "\n";
}

void CancelHttp() {
    http_cancelled = true;
}

std::unique_ptr<CharisStream::Http> MakeHttp() {
    return std::make_unique<CurlHttp>();
}

bool Keyboard(const std::string& header, const std::string& guide, const std::string& initial,
              bool password, int max_length, std::string& out) {
#ifdef __SWITCH__
    SwkbdConfig keyboard;
    if (R_FAILED(swkbdCreate(&keyboard, 0))) {
        return false;
    }
    if (password) {
        swkbdConfigMakePresetPassword(&keyboard);
    } else {
        swkbdConfigMakePresetDefault(&keyboard);
    }
    swkbdConfigSetHeaderText(&keyboard, header.c_str());
    swkbdConfigSetGuideText(&keyboard, guide.c_str());
    swkbdConfigSetInitialText(&keyboard, initial.c_str());
    swkbdConfigSetStringLenMax(&keyboard, static_cast<u32>(max_length));
    swkbdConfigSetOkButtonText(&keyboard, "OK");
    char text[512] = {};
    const Result result = swkbdShow(&keyboard, text, sizeof(text));
    swkbdClose(&keyboard);
    if (R_FAILED(result)) {
        return false;
    }
    out = text;
    return true;
#else
    // On a PC test run the text comes from the environment, or from the terminal
    const char* scripted = std::getenv("CHARIS_KEYBOARD");
    if (scripted != nullptr) {
        out = scripted;
        return true;
    }
    std::printf("%s (%s): ", header.c_str(), guide.c_str());
    std::fflush(stdout);
    char text[512] = {};
    if (std::fgets(text, sizeof(text), stdin) == nullptr) {
        return false;
    }
    out = text;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    (void)initial;
    (void)password;
    (void)max_length;
    return true;
#endif
}

bool Docked() {
#ifdef __SWITCH__
    return appletGetOperationMode() == AppletOperationMode_Console;
#else
    return std::getenv("CHARIS_DOCKED") != nullptr;
#endif
}

void KeepAwake(bool streaming) {
#ifdef __SWITCH__
    appletSetMediaPlaybackState(streaming);
#else
    (void)streaming;
#endif
}

std::string DeviceName() {
#ifdef __SWITCH__
    SetSysDeviceNickName nickname{};
    if (R_SUCCEEDED(setsysGetDeviceNickname(&nickname)) && nickname.nickname[0] != 0) {
        std::string name = nickname.nickname;
        return name.substr(0, 31);
    }
    return "Nintendo Switch";
#else
    return "Switch de teste";
#endif
}

std::string FontPath(bool bold) {
#ifdef __SWITCH__
    return bold ? "romfs:/Archivo-ExtraBold.ttf" : "romfs:/Archivo-Regular.ttf";
#else
    const char* custom = std::getenv(bold ? "CHARIS_FONT_BOLD" : "CHARIS_FONT");
    if (custom != nullptr) {
        return custom;
    }
    return bold ? "romfs/Archivo-ExtraBold.ttf" : "romfs/Archivo-Regular.ttf";
#endif
}

} // namespace Platform
