// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>

#include "charis_stream/account.h"

namespace CharisStream {

std::string UrlEncode(const std::string& text) {
    static const char digits[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += digits[c >> 4];
            out += digits[c & 15];
        }
    }
    return out;
}

std::string UrlDecode(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            out += static_cast<char>(std::strtol(text.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else if (text[i] == '+') {
            out += ' ';
        } else {
            out += text[i];
        }
    }
    return out;
}

namespace {

std::string Message(const std::string& error) {
    if (error == "exists") {
        return "Esse nome de usuário já existe. Escolha outro ou entre com a senha dele.";
    }
    if (error == "wrong") {
        return "Usuário ou senha incorretos.";
    }
    if (error == "invalid") {
        return "Use de 3 a 24 letras minúsculas, números, ponto, traço ou sublinhado no "
               "nome de usuário.";
    }
    if (error == "auth") {
        return "O login expirou. Entre na conta de novo.";
    }
    if (error == "full") {
        return "Essa conta já tem o máximo de PCs.";
    }
    if (error == "nohost") {
        return "Esse PC não está mais disponível.";
    }
    return "O servidor da conta Charis respondeu com um erro. Tente de novo em instantes.";
}

} // namespace

Account::Account(Http& http_, std::string base_url_)
    : http{http_}, base_url{std::move(base_url_)} {}

std::string Account::Call(const std::string& route, const Fields& params, Fields& answer) {
    std::string body;
    for (const auto& [key, value] : params) {
        if (!body.empty()) {
            body += '&';
        }
        body += key + "=" + UrlEncode(value);
    }
    std::string response;
    login_expired = false;
    if (!http.Post(base_url + route, body, response)) {
        return "Sem conexão com o servidor da conta Charis. Confira a internet.";
    }
    answer.clear();
    size_t start = 0;
    while (start < response.size()) {
        size_t end = response.find('\n', start);
        if (end == std::string::npos) {
            end = response.size();
        }
        const std::string line = response.substr(start, end - start);
        const size_t equals = line.find('=');
        if (equals != std::string::npos) {
            answer[line.substr(0, equals)] = UrlDecode(line.substr(equals + 1));
        }
        start = end + 1;
    }
    if (answer.count("ok") != 0) {
        return {};
    }
    const std::string error = answer.count("error") != 0 ? answer["error"] : "server";
    login_expired = error == "auth";
    return Message(error);
}

std::string Account::Register(const std::string& user, const std::string& auth_hex,
                              std::string& token) {
    Fields answer;
    const std::string error = Call("register", {{"user", user}, {"auth", auth_hex}}, answer);
    token = answer["token"];
    return error;
}

std::string Account::Login(const std::string& user, const std::string& auth_hex,
                           std::string& token) {
    Fields answer;
    const std::string error = Call("login", {{"user", user}, {"auth", auth_hex}}, answer);
    token = answer["token"];
    return error;
}

std::string Account::HostPoll(const std::string& token, const std::string& host_id,
                              const std::string& name, const std::vector<Address>& local,
                              const Address& public_address,
                              std::vector<ConnectRequest>& requests) {
    Fields answer;
    const std::string error = Call("host/poll",
                                   {{"token", token},
                                    {"host_id", host_id},
                                    {"name", name},
                                    {"local", Address::JoinList(local)},
                                    {"public", public_address.ToString()}},
                                   answer);
    requests.clear();
    if (!error.empty()) {
        return error;
    }
    const int count = std::atoi(answer["count"].c_str());
    for (int i = 0; i < count && i < 16; ++i) {
        const std::string prefix = "req." + std::to_string(i) + ".";
        ConnectRequest request;
        request.client_id = answer[prefix + "client"];
        request.name = answer[prefix + "name"];
        request.public_address = Address::Parse(answer[prefix + "public"]);
        request.local = Address::ParseList(answer[prefix + "local"]);
        requests.push_back(request);
    }
    return {};
}

std::string Account::HostRemove(const std::string& token, const std::string& host_id) {
    Fields answer;
    return Call("host/remove", {{"token", token}, {"host_id", host_id}}, answer);
}

std::string Account::ListHosts(const std::string& token, std::vector<HostEntry>& hosts) {
    Fields answer;
    const std::string error = Call("hosts", {{"token", token}}, answer);
    hosts.clear();
    if (!error.empty()) {
        return error;
    }
    const int count = std::atoi(answer["count"].c_str());
    for (int i = 0; i < count && i < 16; ++i) {
        const std::string prefix = "host." + std::to_string(i) + ".";
        HostEntry host;
        host.id = answer[prefix + "id"];
        host.name = answer[prefix + "name"];
        host.local = Address::ParseList(answer[prefix + "local"]);
        host.public_address = Address::Parse(answer[prefix + "public"]);
        host.online = answer[prefix + "online"] == "1";
        hosts.push_back(host);
    }
    return {};
}

std::string Account::RequestConnect(const std::string& token, const std::string& host_id,
                                    const std::string& client_id, const std::string& name,
                                    const Address& public_address,
                                    const std::vector<Address>& local) {
    Fields answer;
    return Call("connect",
                {{"token", token},
                 {"host_id", host_id},
                 {"client_id", client_id},
                 {"name", name},
                 {"public", public_address.ToString()},
                 {"local", Address::JoinList(local)}},
                answer);
}

} // namespace CharisStream
