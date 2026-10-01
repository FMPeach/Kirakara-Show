#pragma once

#ifndef _WINSOCKAPI_
#include <winsock2.h>
#endif

#include "native_cast_stream.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

class CastHttpServer {
public:
    explicit CastHttpServer(NativeCastStream& stream);
    ~CastHttpServer();

    CastHttpServer(const CastHttpServer&) = delete;
    CastHttpServer& operator=(const CastHttpServer&) = delete;

    [[nodiscard]] bool start(std::uint16_t port);
    void stop();

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] std::uint16_t port() const noexcept;

private:
    void accept_loop();
    void client_loop(SOCKET client);
    void remember_client(SOCKET client);
    void forget_client(SOCKET client);
    void close_active_clients();

    NativeCastStream& stream_;
    SOCKET listen_socket_{INVALID_SOCKET};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> next_connection_id_{0};
    std::uint16_t port_{};
    bool wsa_started_{};
    std::thread accept_thread_;
    mutable std::mutex clients_mutex_;
    std::vector<SOCKET> active_clients_;
    std::vector<std::thread> client_threads_;
};
