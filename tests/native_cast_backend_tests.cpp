#include "../apps/show_host/cast/native_cast_backend.h"

#ifndef _WINSOCKAPI_
#include <winsock2.h>
#endif

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

class WsaScope {
public:
    WsaScope() {
        WSADATA wsa{};
        ok_ = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }
    ~WsaScope() {
        if (ok_) WSACleanup();
    }
    [[nodiscard]] bool ok() const noexcept { return ok_; }

private:
    bool ok_{};
};

SOCKET connect_local(std::uint16_t port) {
    SOCKET socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    expect(socket_handle != INVALID_SOCKET, "socket() failed");

    DWORD timeout = 2000;
    setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    expect(connect(socket_handle, reinterpret_cast<sockaddr*>(&addr),
               sizeof(addr)) != SOCKET_ERROR,
        "connect() failed");
    return socket_handle;
}

void send_stage_request(SOCKET socket_handle) {
    constexpr const char* request =
        "GET /cast/stage.ts HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n";
    const auto sent = send(socket_handle, request,
        static_cast<int>(std::strlen(request)), 0);
    expect(sent == static_cast<int>(std::strlen(request)),
        "request send failed");
}

std::string recv_until(SOCKET socket_handle, const std::string& needle) {
    std::string data;
    char buffer[1024]{};
    while (data.find(needle) == std::string::npos && data.size() < 8192U) {
        const auto received = recv(socket_handle, buffer, sizeof(buffer), 0);
        expect(received > 0, "recv_until failed");
        data.append(buffer, buffer + received);
    }
    return data;
}

void backend_owns_http_stream_lifecycle() {
    WsaScope wsa;
    expect(wsa.ok(), "WSAStartup failed");

    NativeCastBackend backend;
    expect(backend.start_http(0), "backend failed to start HTTP");
    expect(backend.is_running(), "backend should be running");
    expect(backend.port() != 0, "backend did not expose a port");

    SOCKET client = connect_local(backend.port());
    send_stage_request(client);
    const auto headers = recv_until(client, "\r\n\r\n");
    expect(headers.find("HTTP/1.1 200 OK") != std::string::npos,
        "backend HTTP endpoint did not return 200");

    const std::vector<std::uint8_t> keyframe{
        0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21};
    backend.submit_h264_access_unit(
        keyframe.data(), keyframe.size(), 90000, true);

    char body[188]{};
    const auto received = recv(client, body, sizeof(body), 0);
    expect(received > 0, "backend did not stream TS bytes");
    expect(static_cast<std::uint8_t>(body[0]) == 0x47,
        "backend stream missing TS sync byte");

    closesocket(client);
    backend.stop();
    expect(!backend.is_running(), "backend should stop cleanly");
}

}  // namespace

int main() {
    backend_owns_http_stream_lifecycle();
    std::cout << "NativeCastBackend test passed.\n";
    return 0;
}
