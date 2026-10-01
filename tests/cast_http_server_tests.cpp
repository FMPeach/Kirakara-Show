#include "../apps/show_host/cast/cast_http_server.h"

#ifndef _WINSOCKAPI_
#include <winsock2.h>
#endif

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
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

SOCKET connect_local(
        std::uint16_t port, int receive_buffer_bytes = 0) {
    SOCKET socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    expect(socket_handle != INVALID_SOCKET, "socket() failed");

    if (receive_buffer_bytes > 0) {
        expect(setsockopt(socket_handle, SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char*>(&receive_buffer_bytes),
                   sizeof(receive_buffer_bytes)) != SOCKET_ERROR,
            "setting receive buffer failed");
    }

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

void send_request(SOCKET socket_handle, const char* path,
        const char* method = "GET") {
    const std::string request =
        std::string(method) + " " + path + " HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n";
    const auto sent = send(socket_handle, request.data(),
        static_cast<int>(request.size()), 0);
    expect(sent == static_cast<int>(request.size()), "request send failed");
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

std::vector<std::uint8_t> recv_body_bytes(
        SOCKET socket_handle, std::size_t min_bytes) {
    std::vector<std::uint8_t> data;
    data.reserve(min_bytes);
    char buffer[2048]{};
    while (data.size() < min_bytes) {
        const auto received = recv(socket_handle, buffer, sizeof(buffer), 0);
        expect(received > 0, "body recv failed");
        data.insert(data.end(),
            reinterpret_cast<std::uint8_t*>(buffer),
            reinterpret_cast<std::uint8_t*>(buffer + received));
    }
    return data;
}

std::uint16_t pid_at(
        const std::vector<std::uint8_t>& data, std::size_t packet) {
    const auto offset = packet * MpegTsMuxer::kPacketSize;
    return static_cast<std::uint16_t>(
        ((data[offset + 1] & 0x1FU) << 8U) | data[offset + 2]);
}

void stage_endpoint_streams_ts_bytes() {
    WsaScope wsa;
    expect(wsa.ok(), "WSAStartup failed");

    NativeCastStream stream;
    CastHttpServer server(stream);
    expect(server.start(0), "server failed to start");
    expect(server.port() != 0, "server did not report a port");

    SOCKET client = connect_local(server.port());
    send_request(client, "/cast/stage.ts");
    const auto headers = recv_until(client, "\r\n\r\n");
    expect(headers.find("HTTP/1.1 200 OK") != std::string::npos,
        "stage endpoint did not return 200");
    expect(headers.find("Content-Type: video/MP2T") != std::string::npos,
        "stage endpoint returned wrong content type");
    expect(headers.find("transferMode.dlna.org: Streaming") != std::string::npos,
        "stage endpoint omitted DLNA transfer mode");
    expect(headers.find("contentFeatures.dlna.org:") != std::string::npos,
        "stage endpoint omitted DLNA content features");

    const std::vector<std::uint8_t> keyframe{
        0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21};
    stream.append_h264_access_unit(
        keyframe.data(), keyframe.size(), 90000, true);
    const auto body = recv_body_bytes(client, MpegTsMuxer::kPacketSize);
    expect(body[0] == 0x47, "stage stream missing TS sync byte");

    closesocket(client);
    server.stop();
}

void stage_endpoint_supports_head_probe() {
    WsaScope wsa;
    expect(wsa.ok(), "WSAStartup failed");

    NativeCastStream stream;
    CastHttpServer server(stream);
    expect(server.start(0), "server failed to start");

    SOCKET client = connect_local(server.port());
    send_request(client, "/cast/stage.ts", "HEAD");
    const auto headers = recv_until(client, "\r\n\r\n");
    expect(headers.find("HTTP/1.1 200 OK") != std::string::npos,
        "HEAD stage probe did not return 200");
    expect(headers.find("Content-Type: video/MP2T") != std::string::npos,
        "HEAD stage probe returned wrong content type");

    char byte{};
    const auto received = recv(client, &byte, 1, 0);
    expect(received == 0, "HEAD stage probe unexpectedly returned a body");

    closesocket(client);
    server.stop();
}

void unknown_endpoint_returns_404() {
    WsaScope wsa;
    expect(wsa.ok(), "WSAStartup failed");

    NativeCastStream stream;
    CastHttpServer server(stream);
    expect(server.start(0), "server failed to start");

    SOCKET client = connect_local(server.port());
    send_request(client, "/missing");
    const auto headers = recv_until(client, "\r\n\r\n");
    expect(headers.find("HTTP/1.1 404 Not Found") != std::string::npos,
        "unknown endpoint did not return 404");

    closesocket(client);
    server.stop();
}

void stalled_client_does_not_block_live_producer_or_next_client() {
    WsaScope wsa;
    expect(wsa.ok(), "WSAStartup failed");

    NativeCastStream stream;
    CastHttpServer server(stream);
    expect(server.start(0), "server failed to start");

    SOCKET stalled = connect_local(server.port(), 4096);
    send_request(stalled, "/cast/stage.ts");
    const auto stalled_headers = recv_until(stalled, "\r\n\r\n");
    expect(stalled_headers.find("HTTP/1.1 200 OK") != std::string::npos,
        "stalled client did not receive headers");

    std::vector<std::uint8_t> large_keyframe(4U * 1024U * 1024U, 0x55);
    large_keyframe[0] = 0x00;
    large_keyframe[1] = 0x00;
    large_keyframe[2] = 0x01;
    large_keyframe[3] = 0x65;
    stream.append_h264_access_unit(
        large_keyframe.data(), large_keyframe.size(), 90000, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const std::vector<std::uint8_t> live_keyframe{
        0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21};
    const auto append_started = std::chrono::steady_clock::now();
    stream.append_h264_access_unit(
        live_keyframe.data(), live_keyframe.size(), 135000, true);
    const auto append_elapsed = std::chrono::steady_clock::now()
        - append_started;
    expect(append_elapsed < std::chrono::milliseconds(500),
        "stalled HTTP client blocked the live TS producer");

    SOCKET live = connect_local(server.port());
    send_request(live, "/cast/stage.ts");
    const auto live_response = recv_until(live, "\r\n\r\n");
    expect(live_response.find("HTTP/1.1 200 OK") != std::string::npos,
        "next client did not receive headers");
    const auto body_offset = live_response.find("\r\n\r\n") + 4U;
    std::vector<std::uint8_t> live_body;
    if (body_offset < live_response.size()) {
        live_body.insert(live_body.end(),
            reinterpret_cast<const std::uint8_t*>(
                live_response.data() + body_offset),
            reinterpret_cast<const std::uint8_t*>(
                live_response.data() + live_response.size()));
    }
    while (live_body.size() < MpegTsMuxer::kPacketSize * 3U) {
        char bytes[1024]{};
        const auto received = recv(live, bytes, sizeof(bytes), 0);
        expect(received > 0, "next client did not receive live TS");
        live_body.insert(live_body.end(),
            reinterpret_cast<std::uint8_t*>(bytes),
            reinterpret_cast<std::uint8_t*>(bytes + received));
    }
    expect(live_body[0] == 0x47
            && live_body[MpegTsMuxer::kPacketSize] == 0x47
            && live_body[MpegTsMuxer::kPacketSize * 2U] == 0x47,
        "next client received misaligned MPEG-TS packets");
    expect(pid_at(live_body, 0) == MpegTsMuxer::kPatPid
            && pid_at(live_body, 1) == MpegTsMuxer::kPmtPid
            && pid_at(live_body, 2) == MpegTsMuxer::kVideoPid,
        "next client did not start at the latest decodable keyframe");
    closesocket(live);

    const auto stop_started = std::chrono::steady_clock::now();
    server.stop();
    const auto stop_elapsed = std::chrono::steady_clock::now()
        - stop_started;
    expect(stop_elapsed < std::chrono::milliseconds(1500),
        "server stop waited on a stalled HTTP client");
    closesocket(stalled);
}

}  // namespace

int main() {
    stage_endpoint_streams_ts_bytes();
    stage_endpoint_supports_head_probe();
    unknown_endpoint_returns_404();
    stalled_client_does_not_block_live_producer_or_next_client();
    std::cout << "CastHttpServer test passed.\n";
    return 0;
}
