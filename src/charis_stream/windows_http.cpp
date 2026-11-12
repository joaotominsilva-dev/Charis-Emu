// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#include "charis_stream/sources.h"

#ifndef _WIN32

namespace CharisStream {
std::unique_ptr<Http> MakeSystemHttp() {
    return nullptr;
}
} // namespace CharisStream

#else

#include <atomic>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>
#include <winhttp.h>

namespace CharisStream {

namespace {

std::wstring Widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

class WinHttp final : public Http {
public:
    WinHttp() {
        // The system's fixed proxy setting, without automatic proxy discovery: that discovery
        // can take seconds on every start and would hold up the app when it closes
        session = WinHttpOpen(L"CharisSwitch/1.2", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session != nullptr) {
            // resolve, connect, send, receive: a slow network gives up in seconds, not minutes
            WinHttpSetTimeouts(session, 4000, 4000, 6000, 6000);
        }
    }
    ~WinHttp() override {
        if (session != nullptr) {
            WinHttpCloseHandle(session);
        }
    }

    void Cancel() override {
        std::scoped_lock lock{mutex};
        cancelled = true;
        if (active != nullptr) {
            // Closing the request handle makes the blocked call in the other thread return
            WinHttpCloseHandle(active);
            active = nullptr;
        }
    }

    bool Post(const std::string& url, const std::string& body, std::string& response) override {
        response.clear();
        if (session == nullptr || cancelled) {
            return false;
        }
        const std::wstring wide_url = Widen(url);
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        wchar_t host[256] = {};
        wchar_t path[2048] = {};
        parts.lpszHostName = host;
        parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
        if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts)) {
            return false;
        }
        const HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
        if (connection == nullptr) {
            return false;
        }
        const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET request =
            WinHttpOpenRequest(connection, L"POST", path, nullptr, WINHTTP_NO_REFERER,
                               WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        {
            std::scoped_lock lock{mutex};
            if (cancelled && request != nullptr) {
                WinHttpCloseHandle(request);
                request = nullptr;
            }
            active = request;
        }
        bool ok = false;
        if (request != nullptr) {
            const wchar_t* headers = L"Content-Type: application/x-www-form-urlencoded\r\n";
            ok = WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L),
                                    const_cast<char*>(body.data()),
                                    static_cast<DWORD>(body.size()),
                                    static_cast<DWORD>(body.size()), 0) &&
                 WinHttpReceiveResponse(request, nullptr);
            while (ok) {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(request, &available)) {
                    ok = false;
                    break;
                }
                if (available == 0) {
                    break;
                }
                std::vector<char> chunk(available);
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), available, &read)) {
                    ok = false;
                    break;
                }
                response.append(chunk.data(), read);
                if (response.size() > 256 * 1024) {
                    ok = false;
                    break;
                }
            }
            std::scoped_lock lock{mutex};
            if (active == request) { // not already closed by Cancel
                WinHttpCloseHandle(request);
                active = nullptr;
            }
        }
        WinHttpCloseHandle(connection);
        return ok && !cancelled;
    }

private:
    HINTERNET session = nullptr;
    std::mutex mutex;
    HINTERNET active = nullptr;
    std::atomic<bool> cancelled{false};
};

} // namespace

std::unique_ptr<Http> MakeSystemHttp() {
    return std::make_unique<WinHttp>();
}

} // namespace CharisStream

#endif
