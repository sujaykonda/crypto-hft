#pragma once

#include "ipc/client_protocol.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace hft::ipc {

class UnixDomainCommandServer {
public:
    using SubscribeCallback = std::function<nlohmann::json(const std::vector<std::string>&)>;
    using SnapshotCallback = std::function<nlohmann::json(const std::string&)>;

    explicit UnixDomainCommandServer(std::string socket_path)
        : socket_path_(std::move(socket_path)) {}

    ~UnixDomainCommandServer() {
        stop();
    }

    UnixDomainCommandServer(const UnixDomainCommandServer&) = delete;
    UnixDomainCommandServer& operator=(const UnixDomainCommandServer&) = delete;

    void set_subscribe_callback(SubscribeCallback callback) {
        subscribe_callback_ = std::move(callback);
    }

    void set_snapshot_callback(SnapshotCallback callback) {
        snapshot_callback_ = std::move(callback);
    }

    void start() {
        if (running_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            running_.store(false, std::memory_order_release);
            throw std::runtime_error("failed to create unix socket: " + last_errno());
        }

        ::unlink(socket_path_.c_str());

        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (socket_path_.size() >= sizeof(address.sun_path)) {
            close_listen_socket();
            running_.store(false, std::memory_order_release);
            throw std::runtime_error("unix socket path too long");
        }
        std::strncpy(address.sun_path, socket_path_.c_str(), sizeof(address.sun_path) - 1);

        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            const std::string error = last_errno();
            close_listen_socket();
            running_.store(false, std::memory_order_release);
            throw std::runtime_error("failed to bind unix socket: " + error);
        }

        if (::listen(listen_fd_, 16) != 0) {
            const std::string error = last_errno();
            close_listen_socket();
            running_.store(false, std::memory_order_release);
            throw std::runtime_error("failed to listen on unix socket: " + error);
        }

        thread_ = std::thread(&UnixDomainCommandServer::accept_loop, this);
    }

    void stop() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }

        close_listen_socket();
        if (thread_.joinable()) {
            thread_.join();
        }
        ::unlink(socket_path_.c_str());
    }

    bool is_running() const {
        return running_.load(std::memory_order_acquire);
    }

private:
    static std::string last_errno() {
        return std::strerror(errno);
    }

    void close_listen_socket() {
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
    }

    void accept_loop() {
        while (running_.load(std::memory_order_acquire)) {
            const int client_fd = ::accept(listen_fd_, nullptr, nullptr);
            if (client_fd < 0) {
                if (running_.load(std::memory_order_acquire)) {
                    continue;
                }
                break;
            }
            handle_client(client_fd);
            ::close(client_fd);
        }
    }

    void handle_client(int client_fd) {
        std::string line;
        char buffer[1024];
        while (line.find('\n') == std::string::npos) {
            const ssize_t read_count = ::read(client_fd, buffer, sizeof(buffer));
            if (read_count <= 0) {
                break;
            }
            line.append(buffer, static_cast<size_t>(read_count));
            if (line.size() > 64 * 1024) {
                write_response(client_fd, make_error_response("request too large"));
                return;
            }
        }

        const size_t newline = line.find('\n');
        if (newline != std::string::npos) {
            line.resize(newline);
        }

        std::string error;
        const auto command = parse_command_line(line, &error);
        if (!command.has_value()) {
            write_response(client_fd, make_error_response(error));
            return;
        }

        switch (command->type) {
            case CommandType::Ping:
                write_response(client_fd, make_ok_response({{"pong", true}}));
                break;
            case CommandType::Subscribe:
                if (!subscribe_callback_) {
                    write_response(client_fd, make_error_response("subscribe unavailable"));
                    break;
                }
                write_response(client_fd, make_ok_response(subscribe_callback_(command->tickers)));
                break;
            case CommandType::Snapshot:
                if (!snapshot_callback_) {
                    write_response(client_fd, make_error_response("snapshot unavailable"));
                    break;
                }
                write_response(client_fd, make_ok_response(snapshot_callback_(command->tickers.front())));
                break;
            case CommandType::Unknown:
                write_response(client_fd, make_error_response("unknown command"));
                break;
        }
    }

    static void write_response(int client_fd, const std::string& response) {
        const char* data = response.data();
        size_t remaining = response.size();
        while (remaining > 0) {
            const ssize_t written = ::write(client_fd, data, remaining);
            if (written <= 0) {
                return;
            }
            data += written;
            remaining -= static_cast<size_t>(written);
        }
    }

    std::string socket_path_;
    SubscribeCallback subscribe_callback_;
    SnapshotCallback snapshot_callback_;
    std::atomic<bool> running_{false};
    int listen_fd_{-1};
    std::thread thread_;
};

}  // namespace hft::ipc
