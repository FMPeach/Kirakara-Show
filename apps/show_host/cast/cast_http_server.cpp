#include "cast_http_server.h"
#include "cast_pipeline_diagnostics.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>

namespace {

constexpr std::size_t kMaxClientLagBytes = 1024U * 1024U;
constexpr std::size_t kClientReadBytes =
    MpegTsMuxer::kPacketSize * 348U;

void close_socket(SOCKET& socket) {
    if (socket == INVALID_SOCKET) return;
    shutdown(socket, SD_BOTH);
    closesocket(socket);
    socket = INVALID_SOCKET;
}

bool send_all(SOCKET socket, const char* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const auto chunk = static_cast<int>(
            std::min<std::size_t>(size - offset, 64U * 1024U));
        const auto sent = send(socket, data + offset, chunk, 0);
        if (sent <= 0) return false;
        offset += static_cast<std::size_t>(sent);
    }
    return true;
}

bool send_text(SOCKET socket, const char* text) {
    return send_all(socket, text, std::strlen(text));
}

std::string read_request(SOCKET socket) {
    std::string request;
    request.reserve(1024);
    char buffer[1024]{};
    while (request.size() < 8192U) {
        const auto received = recv(socket, buffer, sizeof(buffer), 0);
        if (received <= 0) return {};
        request.append(buffer, buffer + received);
        if (request.find("\r\n\r\n") != std::string::npos) break;
    }
    return request;
}

enum class RequestMethod {
    invalid,
    get,
    head,
};

RequestMethod stage_request_method(const std::string& request) {
    const char* prefix = nullptr;
    RequestMethod method = RequestMethod::invalid;
    if (request.rfind("GET ", 0) == 0) {
        prefix = "GET ";
        method = RequestMethod::get;
    } else if (request.rfind("HEAD ", 0) == 0) {
        prefix = "HEAD ";
        method = RequestMethod::head;
    } else {
        return RequestMethod::invalid;
    }
    const auto path_start = std::strlen(prefix);
    const auto path_end = request.find(' ', path_start);
    if (path_end == std::string::npos) return RequestMethod::invalid;
    const auto path = request.substr(path_start, path_end - path_start);
    return path == "/cast/stage.ts" ? method : RequestMethod::invalid;
}

}  // namespace

CastHttpServer::CastHttpServer(NativeCastStream& stream) : stream_(stream) {}

CastHttpServer::~CastHttpServer() { stop(); }

bool CastHttpServer::start(std::uint16_t port) {
    if (running_.load()) return true;

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    wsa_started_ = true;

    listen_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket_ == INVALID_SOCKET) {
        stop();
        return false;
    }

    BOOL reuse = TRUE;
    setsockopt(listen_socket_, SOL_SOCKET, SO_REUSEADDR,
        reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    u_long nonblocking = 1;
    ioctlsocket(listen_socket_, FIONBIO, &nonblocking);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listen_socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr))
            == SOCKET_ERROR) {
        stop();
        return false;
    }

    if (listen(listen_socket_, SOMAXCONN) == SOCKET_ERROR) {
        stop();
        return false;
    }

    sockaddr_in bound{};
    int bound_size = sizeof(bound);
    if (getsockname(listen_socket_,
            reinterpret_cast<sockaddr*>(&bound), &bound_size)
            == SOCKET_ERROR) {
        stop();
        return false;
    }

    port_ = ntohs(bound.sin_port);
    running_.store(true);
    accept_thread_ = std::thread(&CastHttpServer::accept_loop, this);
    return true;
}

void CastHttpServer::stop() {
    running_.store(false);
    close_socket(listen_socket_);
    if (accept_thread_.joinable()) accept_thread_.join();
    close_active_clients();

    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        threads.swap(client_threads_);
    }
    for (auto& thread : threads) {
        if (thread.joinable()) thread.join();
    }

    port_ = 0;
    if (wsa_started_) {
        WSACleanup();
        wsa_started_ = false;
    }
}

bool CastHttpServer::is_running() const noexcept {
    return running_.load();
}

std::uint16_t CastHttpServer::port() const noexcept {
    return port_;
}

void CastHttpServer::accept_loop() {
    while (running_.load()) {
        fd_set read_set{};
        FD_ZERO(&read_set);
        FD_SET(listen_socket_, &read_set);
        timeval timeout{0, 100000};
        const auto ready = select(0, &read_set, nullptr, nullptr, &timeout);
        if (!running_.load()) break;
        if (ready <= 0 || !FD_ISSET(listen_socket_, &read_set)) continue;

        sockaddr_in client_addr{};
        int client_addr_size = sizeof(client_addr);
        SOCKET client = accept(listen_socket_,
            reinterpret_cast<sockaddr*>(&client_addr), &client_addr_size);
        if (client == INVALID_SOCKET) continue;

        // Bound kernel-side buffering as well as the app ring. A receiver that
        // cannot drain within the live budget is disconnected rather than
        // being allowed to accumulate seconds of stale Stage frames.
        u_long blocking = 0;
        static_cast<void>(ioctlsocket(client, FIONBIO, &blocking));
        int send_buffer = static_cast<int>(kMaxClientLagBytes / 2U);
        static_cast<void>(setsockopt(
            client, SOL_SOCKET, SO_SNDBUF,
            reinterpret_cast<const char*>(&send_buffer),
            sizeof(send_buffer)));
        DWORD send_timeout_ms = 1000;
        static_cast<void>(setsockopt(
            client, SOL_SOCKET, SO_SNDTIMEO,
            reinterpret_cast<const char*>(&send_timeout_ms),
            sizeof(send_timeout_ms)));
        BOOL no_delay = TRUE;
        static_cast<void>(setsockopt(
            client, IPPROTO_TCP, TCP_NODELAY,
            reinterpret_cast<const char*>(&no_delay),
            sizeof(no_delay)));

        remember_client(client);
        std::lock_guard<std::mutex> lock(clients_mutex_);
        client_threads_.emplace_back(&CastHttpServer::client_loop, this, client);
    }
}

void CastHttpServer::client_loop(SOCKET client) {
    auto connection_id = next_connection_id_.fetch_add(
        1, std::memory_order_relaxed) + 1;
    if (connection_id == 0) {
        connection_id = next_connection_id_.fetch_add(
            1, std::memory_order_relaxed) + 1;
    }
    const auto request = read_request(client);
    const auto method = stage_request_method(request);
    if (method == RequestMethod::invalid) {
        send_text(client,
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n");
        forget_client(client);
        closesocket(client);
        return;
    }

    if (!send_text(client,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: video/MP2T\r\n"
            "transferMode.dlna.org: Streaming\r\n"
            "contentFeatures.dlna.org: DLNA.ORG_OP=00;DLNA.ORG_CI=0;"
            "DLNA.ORG_FLAGS=01700000000000000000000000000000\r\n"
            "Accept-Ranges: none\r\n"
            "Cache-Control: no-store\r\n"
            "EXT:\r\n"
            "Server: Windows UPnP/1.1 Kirakara/0.1\r\n"
            "Connection: close\r\n\r\n")) {
        forget_client(client);
        closesocket(client);
        return;
    }

    if (method == RequestMethod::head) {
        forget_client(client);
        closesocket(client);
        return;
    }

    auto cursor = stream_.cursor_from_latest_keyframe();
    MpegTsContinuityRewriter continuity;
    std::vector<std::uint8_t> buffer(kClientReadBytes);
    bool sent_first_payload{};
    std::uint64_t previous_send_finished_qpc{};
    while (running_.load()) {
        const auto rebased = stream_.rebase_to_latest_keyframe_if_lagging(
            cursor, kMaxClientLagBytes);
        if (rebased) {
            std::vector<std::uint8_t> discontinuity;
            continuity.append_discontinuity_packets(discontinuity);
            if (!discontinuity.empty()
                    && !send_all(client,
                        reinterpret_cast<const char*>(discontinuity.data()),
                        discontinuity.size())) {
                break;
            }
        }
        const auto result = stream_.read(
            cursor, buffer.data(), buffer.size(),
            std::chrono::milliseconds(100));
        if (result.timed_out || result.bytes == 0) continue;
        if (!continuity.rewrite_packets(buffer.data(), result.bytes)) break;
        const auto send_started_qpc = CastPipelineDiagnostics::qpc_now();
        if (!send_all(client,
                reinterpret_cast<const char*>(buffer.data()),
                result.bytes)) {
            break;
        }
        const auto send_finished_qpc = CastPipelineDiagnostics::qpc_now();
        if (!sent_first_payload) {
            stream_.record_http_first_byte(
                cursor.sequence, connection_id, result.bytes);
            sent_first_payload = true;
        }
        stream_.record_http_send(
            cursor.sequence, connection_id, result.bytes,
            previous_send_finished_qpc,
            send_started_qpc,
            send_finished_qpc);
        previous_send_finished_qpc = send_finished_qpc;
    }

    forget_client(client);
    closesocket(client);
}

void CastHttpServer::remember_client(SOCKET client) {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    active_clients_.push_back(client);
}

void CastHttpServer::forget_client(SOCKET client) {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    active_clients_.erase(std::remove(
        active_clients_.begin(), active_clients_.end(), client),
        active_clients_.end());
}

void CastHttpServer::close_active_clients() {
    std::vector<SOCKET> sockets;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        sockets.swap(active_clients_);
    }
    for (auto socket : sockets) {
        shutdown(socket, SD_BOTH);
        closesocket(socket);
    }
}
