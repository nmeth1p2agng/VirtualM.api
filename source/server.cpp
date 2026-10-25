#include "luau_vm/vm.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
constexpr std::size_t kMaxBody = 1024 * 1024;

std::string json_escape(std::string_view input) {
    std::ostringstream out;
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
                if (c < 0x20) {
                    constexpr char hex[] = "0123456789abcdef";
                    out << "\\u00" << hex[c >> 4] << hex[c & 0x0f];
                } else out << static_cast<char>(c);
        }
    }
    return out.str();
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

bool extract_json_string(std::string_view body, std::string_view wanted, std::string& result) {
    std::size_t i = 0;
    while (i < body.size()) {
        if (body[i] != '"') { ++i; continue; }
        ++i;
        std::string key;
        while (i < body.size() && body[i] != '"') {
            if (body[i] == '\\' && i + 1 < body.size()) { key.push_back(body[i + 1]); i += 2; }
            else key.push_back(body[i++]);
        }
        if (i >= body.size()) return false;
        ++i;
        while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
        if (i >= body.size() || body[i] != ':') continue;
        ++i;
        while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
        if (key != wanted || i >= body.size() || body[i] != '"') continue;
        ++i;
        while (i < body.size() && body[i] != '"') {
            char c = body[i++];
            if (c != '\\') { result.push_back(c); continue; }
            if (i >= body.size()) return false;
            const char e = body[i++];
            switch (e) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                default: return false; // Reject unsupported escapes rather than misinterpret source.
            }
        }
        return i < body.size() && body[i] == '"';
    }
    return false;
}

std::string value_json(const luau_vm::Value& value) {
    if (std::holds_alternative<luau_vm::Nil>(value)) return "{\"type\":\"nil\",\"value\":null}";
    if (const auto* b = std::get_if<bool>(&value)) return std::string("{\"type\":\"boolean\",\"value\":") + (*b ? "true}" : "false}");
    if (const auto* n = std::get_if<double>(&value)) {
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
             << "Access-Control-Allow-Headers: Content-Type\r\n"
             << "Connection: close\r\n"
             << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    const std::string data = response.str();
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto n = ::send(client, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) break;
        sent += static_cast<std::size_t>(n);
    }
}

void handle_client(int client) {
    std::string request;
    char buffer[4096];
    std::size_t header_end = std::string::npos;
    while (request.size() < kMaxBody + 16384) {
        const auto n = ::recv(client, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        request.append(buffer, static_cast<std::size_t>(n));
        header_end = request.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            std::size_t content_length = 0;
            std::istringstream headers(request.substr(0, header_end));
            std::string line;
            std::getline(headers, line);
            while (std::getline(headers, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const auto colon = line.find(':');
                if (colon == std::string::npos) continue;
                std::string name = line.substr(0, colon);
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (name == "content-length") {
                    try { content_length = static_cast<std::size_t>(std::stoull(trim(line.substr(colon + 1)))); }
                    catch (...) { send_response(client, 400, "Bad Request", "{\"error\":\"Invalid Content-Length\"}"); return; }
                }
            }
            if (content_length > kMaxBody) { send_response(client, 413, "Payload Too Large", "{\"error\":\"Request body exceeds 1 MiB\"}"); return; }
            if (request.size() >= header_end + 4 + content_length) break;
        }
    }
    if (header_end == std::string::npos) { send_response(client, 400, "Bad Request", "{\"error\":\"Malformed HTTP request\"}"); return; }

    std::istringstream first_line(request.substr(0, request.find("\r\n")));
    std::string method, path, version;
    first_line >> method >> path >> version;
    if (method == "OPTIONS") { send_response(client, 204, "No Content", ""); return; }
    if (method == "GET" && (path == "/" || path == "/health")) {
        send_response(client, 200, "OK", "{\"status\":\"ok\",\"engine\":\"VirtualM.api\",\"mode\":\"experimental-subset\"}"); return;
    }
    if (method != "POST" || (path != "/api/execute" && path != "/execute")) {
        send_response(client, 404, "Not Found", "{\"error\":\"Route not found\",\"routes\":[\"GET /health\",\"POST /api/execute\"]}"); return;
    }
    const std::string body = request.substr(header_end + 4);
    std::string source;
    if (!extract_json_string(body, "source", source)) {
        send_response(client, 400, "Bad Request", "{\"error\":\"Expected JSON body with a string field named source\"}"); return;
    }
    if (source.size() > kMaxBody) { send_response(client, 413, "Payload Too Large", "{\"error\":\"Source exceeds 1 MiB\"}"); return; }

    luau_vm::VM vm;
    const auto result = vm.execute_source(source);
    if (!result) {
        send_response(client, 400, "Bad Request", "{\"ok\":false,\"error\":\"" + json_escape(result.error()) + "\"}"); return;
    }
    send_response(client, 200, "OK", "{\"ok\":true,\"result\":" + value_json(result.value()) + "}");
}
} // namespace

int main() {
    const char* port_env = std::getenv("PORT");
    int port = 80;
    if (port_env && *port_env) {
        try { port = std::stoi(port_env); } catch (...) { std::cerr << "Invalid PORT\n"; return 2; }
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
    if (::listen(server, 128) < 0) { std::perror("listen"); ::close(server); return 1; }
    std::cout << "VirtualM.api listening on port " << port << std::endl;
    while (true) {
        const int client = ::accept(server, nullptr, nullptr);
        if (client < 0) { if (errno == EINTR) continue; std::perror("accept"); continue; }
        handle_client(client);
        ::close(client);
    }
}
