#include "luau_vm/vm.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {
constexpr std::size_t kMaxBody = 256 * 1024;
constexpr std::size_t kMaxHeaders = 16 * 1024;
constexpr int kMaxClients = 32;
std::atomic<int> active_clients{0};

std::string json_escape(std::string_view input) {
    std::ostringstream out;
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : input) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) out << "\\u00" << hex[c >> 4] << hex[c & 0x0f];
                else out << static_cast<char>(c);
        }
    }
    return out.str();
}

std::string trim(std::string s) {
    const auto nonspace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), nonspace));
    s.erase(std::find_if(s.rbegin(), s.rend(), nonspace).base(), s.end());
    return s;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

// Small JSON reader for the API envelope. It validates JSON structure and extracts
// only the top-level string field named "source"; it is not a general JSON library.
class JsonSourceReader {
public:
    explicit JsonSourceReader(std::string_view input) : input_(input) {}

    bool read_source(std::string& source) {
        skip_ws();
        if (!take('{')) return false;
        skip_ws();
        if (take('}')) { skip_ws(); return false; }
        bool found = false;
        while (pos_ < input_.size()) {
            std::string key;
            if (!read_string(key)) return false;
            skip_ws();
            if (!take(':')) return false;
            skip_ws();
            if (key == "source") {
                if (found || !read_string(source)) return false;
                found = true;
            } else if (!skip_value(0)) {
                return false;
            }
            skip_ws();
            if (take('}')) { skip_ws(); return found && pos_ == input_.size(); }
            if (!take(',')) return false;
            skip_ws();
        }
        return false;
    }

private:
    void skip_ws() { while (pos_ < input_.size() && (input_[pos_] == ' ' || input_[pos_] == '\n' || input_[pos_] == '\r' || input_[pos_] == '\t')) ++pos_; }
    bool take(char c) { if (pos_ < input_.size() && input_[pos_] == c) { ++pos_; return true; } return false; }

    static int hex_digit(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    bool read_u16(unsigned& value) {
        if (pos_ + 4 > input_.size()) return false;
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const int digit = hex_digit(input_[pos_++]);
            if (digit < 0) return false;
            value = (value << 4) | static_cast<unsigned>(digit);
        }
        return true;
    }
    static void append_utf8(std::string& out, unsigned cp) {
        if (cp <= 0x7f) out.push_back(static_cast<char>(cp));
        else if (cp <= 0x7ff) {
            out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else if (cp <= 0xffff) {
            out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
    }
    bool read_string(std::string& out) {
        if (!take('"')) return false;
        while (pos_ < input_.size()) {
            const unsigned char c = static_cast<unsigned char>(input_[pos_++]);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') { out.push_back(static_cast<char>(c)); continue; }
            if (pos_ >= input_.size()) return false;
            const char e = input_[pos_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned cp = 0;
                    if (!read_u16(cp)) return false;
                    if (cp >= 0xd800 && cp <= 0xdbff) {
                        if (pos_ + 2 > input_.size() || input_[pos_] != '\\' || input_[pos_ + 1] != 'u') return false;
                        pos_ += 2;
                        unsigned low = 0;
                        if (!read_u16(low) || low < 0xdc00 || low > 0xdfff) return false;
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                    } else if (cp >= 0xdc00 && cp <= 0xdfff) return false;
                    append_utf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool skip_value(unsigned depth) {
        if (depth > 32 || pos_ >= input_.size()) return false;
        skip_ws();
        if (pos_ >= input_.size()) return false;
        if (input_[pos_] == '"') { std::string ignored; return read_string(ignored); }
        if (input_[pos_] == '{') {
            ++pos_; skip_ws(); if (take('}')) return true;
            while (true) {
                std::string key;
                if (!read_string(key)) return false;
                skip_ws(); if (!take(':')) return false;
                skip_ws(); if (!skip_value(depth + 1)) return false;
                skip_ws(); if (take('}')) return true;
                if (!take(',')) return false;
                skip_ws();
            }
        }
        if (input_[pos_] == '[') {
            ++pos_; skip_ws(); if (take(']')) return true;
            while (true) {
                if (!skip_value(depth + 1)) return false;
                skip_ws(); if (take(']')) return true;
                if (!take(',')) return false;
                skip_ws();
            }
        }
        for (std::string_view literal : {"true", "false", "null"}) {
            if (input_.substr(pos_, literal.size()) == literal) { pos_ += literal.size(); return true; }
        }
        const std::size_t start = pos_;
        if (take('-')) {}
        if (take('0')) {}
        else {
            if (pos_ >= input_.size() || input_[pos_] < '1' || input_[pos_] > '9') return false;
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_;
        }
        if (take('.')) {
            const auto before = pos_;
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_;
            if (before == pos_) return false;
        }
        if (take('e') || take('E')) {
            if (!take('+')) take('-');
            const auto before = pos_;
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_;
            if (before == pos_) return false;
        }
        return pos_ > start;
    }

    std::string_view input_;
    std::size_t pos_{};
};

std::string value_json(const luau_vm::Value& value) {
    if (std::holds_alternative<luau_vm::Nil>(value)) return "{\"type\":\"nil\",\"value\":null}";
    if (const auto* b = std::get_if<bool>(&value)) return std::string("{\"type\":\"boolean\",\"value\":") + (*b ? "true}" : "false}");
    if (const auto* n = std::get_if<double>(&value)) {
        if (!std::isfinite(*n)) return "{\"type\":\"number\",\"value\":null}";
        std::ostringstream out; out.precision(17); out << "{\"type\":\"number\",\"value\":" << *n << '}'; return out.str();
    }
    if (const auto* s = std::get_if<std::string>(&value)) return "{\"type\":\"string\",\"value\":\"" + json_escape(*s) + "\"}";
    if (std::holds_alternative<luau_vm::TableRef>(value)) return "{\"type\":\"table\",\"value\":null}";
    return "{\"type\":\"function\",\"value\":null}";
}

void send_response(int client, int status, std::string_view status_text, std::string_view body) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status << ' ' << status_text << "\r\n"
             << "Content-Type: application/json; charset=utf-8\r\n"
             << "Access-Control-Allow-Origin: *\r\n"
             << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
             << "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
             << "X-Content-Type-Options: nosniff\r\n"
             << "Cache-Control: no-store\r\n"
             << "Connection: close\r\n"
             << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    const std::string data = response.str();
    std::size_t sent = 0;
    while (sent < data.size()) {
#ifdef MSG_NOSIGNAL
        const auto n = ::send(client, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
#else
        const auto n = ::send(client, data.data() + sent, data.size() - sent, 0);
#endif
        if (n <= 0) break;
        sent += static_cast<std::size_t>(n);
    }
}

void handle_client(int client) {
    timeval timeout{10, 0};
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    std::string request;
    char buffer[4096];
    std::size_t header_end = std::string::npos;
    std::size_t content_length = 0;
    while (request.size() < kMaxHeaders + kMaxBody) {
        const auto n = ::recv(client, buffer, sizeof(buffer), 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            send_response(client, 408, "Request Timeout", "{\"error\":\"Request timed out\"}"); return;
        }
        if (n <= 0) break;
        request.append(buffer, static_cast<std::size_t>(n));
        header_end = request.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            if (request.size() > kMaxHeaders) { send_response(client, 431, "Request Header Fields Too Large", "{\"error\":\"Headers too large\"}"); return; }
            continue;
        }
        std::istringstream headers(request.substr(0, header_end));
        std::string line;
        std::getline(headers, line);
        bool bad_length = false;
        bool transfer_encoding = false;
        while (std::getline(headers, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            const auto name = lower(trim(line.substr(0, colon)));
            const auto value = lower(trim(line.substr(colon + 1)));
            if (name == "content-length") {
                try {
                    const auto parsed = std::stoull(value);
                    if (parsed > kMaxBody) { send_response(client, 413, "Payload Too Large", "{\"error\":\"Request body exceeds 256 KiB\"}"); return; }
                    content_length = static_cast<std::size_t>(parsed);
                } catch (...) { bad_length = true; }
            }
            if (name == "transfer-encoding" && value != "identity") transfer_encoding = true;
        }
        if (bad_length || transfer_encoding) { send_response(client, 400, "Bad Request", "{\"error\":\"Invalid Content-Length or unsupported Transfer-Encoding\"}"); return; }
        if (request.size() >= header_end + 4 + content_length) break;
    }
    if (header_end == std::string::npos) { send_response(client, 400, "Bad Request", "{\"error\":\"Malformed HTTP request\"}"); return; }
    if (request.size() < header_end + 4 + content_length) { send_response(client, 400, "Bad Request", "{\"error\":\"Incomplete request body\"}"); return; }

    std::istringstream first_line(request.substr(0, request.find("\r\n")));
    std::string method, path, version;
    first_line >> method >> path >> version;
    if (version != "HTTP/1.1" && version != "HTTP/1.0") { send_response(client, 400, "Bad Request", "{\"error\":\"Invalid HTTP version\"}"); return; }
    if (method == "OPTIONS") { send_response(client, 204, "No Content", ""); return; }
    if (method == "GET" && (path == "/" || path == "/health")) {
        send_response(client, 200, "OK", "{\"status\":\"ok\",\"engine\":\"VirtualM.api\",\"mode\":\"experimental-subset\",\"maxSourceBytes\":262144}"); return;
    }
    if (method == "GET" && path == "/api/info") {
        send_response(client, 200, "OK", "{\"name\":\"VirtualM.api\",\"version\":\"0.6.0\",\"routes\":[\"GET /health\",\"GET /api/info\",\"POST /api/execute\"],\"sourceLimitBytes\":262144,\"languageSupport\":\"experimental subset; not full Luau compatibility\"}"); return;
    }
    if (method != "POST" || (path != "/api/execute" && path != "/execute")) {
        send_response(client, 404, "Not Found", "{\"error\":\"Route not found\",\"routes\":[\"GET /health\",\"GET /api/info\",\"POST /api/execute\"]}"); return;
    }

    const auto header_text = request.substr(0, header_end);
    if (lower(header_text).find("content-type:") == std::string::npos) {
        send_response(client, 415, "Unsupported Media Type", "{\"error\":\"Use Content-Type: application/json\"}"); return;
    }
    const std::string body = request.substr(header_end + 4, content_length);
    std::string source;
    JsonSourceReader reader(body);
    if (!reader.read_source(source)) {
        send_response(client, 400, "Bad Request", "{\"error\":\"Expected valid JSON object with one top-level string field named source\"}"); return;
    }
    if (source.empty()) { send_response(client, 400, "Bad Request", "{\"error\":\"Source code is empty\"}"); return; }
    if (source.size() > kMaxBody) { send_response(client, 413, "Payload Too Large", "{\"error\":\"Source exceeds 256 KiB\"}"); return; }

    try {
        luau_vm::VM vm;
        const auto result = vm.execute_source(source);
        if (!result) {
            send_response(client, 400, "Bad Request", "{\"ok\":false,\"error\":\"" + json_escape(result.error()) + "\"}"); return;
        }
        send_response(client, 200, "OK", "{\"ok\":true,\"result\":" + value_json(result.value()) + "}");
    } catch (const std::exception& e) {
        send_response(client, 500, "Internal Server Error", "{\"ok\":false,\"error\":\"Internal execution error\"}");
        std::cerr << "Execution exception: " << e.what() << '\n';
    }
}

void client_worker(int client) {
    handle_client(client);
    ::shutdown(client, SHUT_RDWR);
    ::close(client);
    --active_clients;
}
} // namespace

int main() {
    const char* port_env = std::getenv("PORT");
    int port = 10000;
    if (port_env && *port_env) {
        try { port = std::stoi(port_env); }
        catch (...) { std::cerr << "Invalid PORT\n"; return 2; }
    }
    if (port < 1 || port > 65535) { std::cerr << "PORT must be between 1 and 65535\n"; return 2; }

    const int server = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) { std::perror("socket"); return 1; }
    int reuse = 1;
    ::setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) { std::perror("bind"); ::close(server); return 1; }
    if (::listen(server, 64) < 0) { std::perror("listen"); ::close(server); return 1; }
    std::cout << "VirtualM.api v0.6.0 listening on port " << port << std::endl;

    while (true) {
        const int client = ::accept(server, nullptr, nullptr);
        if (client < 0) { if (errno == EINTR) continue; std::perror("accept"); continue; }
        int current = active_clients.load();
        while (current < kMaxClients && !active_clients.compare_exchange_weak(current, current + 1)) {}
        if (current >= kMaxClients) {
            send_response(client, 503, "Service Unavailable", "{\"error\":\"Server is busy; retry shortly\"}");
            ::close(client);
            continue;
        }
        try { std::thread(client_worker, client).detach(); }
        catch (...) {
            --active_clients;
            send_response(client, 503, "Service Unavailable", "{\"error\":\"Unable to start request worker\"}");
            ::close(client);
        }
    }
}
