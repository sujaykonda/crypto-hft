#pragma once

#include "exchange/crypto_com_api.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <iostream>

namespace hft {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

// ═══════════════════════════════════════════════════════════════════════════
// WebSocket Client for Crypto.com Exchange
// ═══════════════════════════════════════════════════════════════════════════

class CryptoComWebSocketClient : public std::enable_shared_from_this<CryptoComWebSocketClient> {
public:
    using MessageCallback = std::function<void(const json&)>;
    using ErrorCallback = std::function<void(const std::string&)>;
    using ConnectedCallback = std::function<void()>;
    using DisconnectedCallback = std::function<void()>;

private:
    net::io_context& ioc_;
    ssl::context& ssl_ctx_;
    tcp::resolver resolver_;
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws_;
    beast::flat_buffer buffer_;
    
    std::string host_;
    std::string port_;
    std::string endpoint_;
    
    AuthHandler auth_;
    std::unique_ptr<CryptoComMessageBuilder> msg_builder_;
    
    std::atomic<bool> connected_{false};
    std::atomic<bool> authenticated_{false};
    std::atomic<bool> should_reconnect_{true};
    
    // Callbacks
    MessageCallback on_message_;
    ErrorCallback on_error_;
    ConnectedCallback on_connected_;
    DisconnectedCallback on_disconnected_;
    
    // Send queue (thread-safe)
    std::queue<std::string> send_queue_;
    std::mutex send_mutex_;
    std::atomic<bool> sending_{false};

    // Reconnection
    net::steady_timer reconnect_timer_;
    int reconnect_attempts_{0};
    static constexpr int MAX_RECONNECT_ATTEMPTS = 10;
    static constexpr int RECONNECT_DELAY_MS = 1000;

public:
    CryptoComWebSocketClient(
        net::io_context& ioc,
        ssl::context& ssl_ctx,
        const std::string& api_key,
        const std::string& api_secret,
        bool use_sandbox = false
    )
        : ioc_(ioc)
        , ssl_ctx_(ssl_ctx)
        , resolver_(net::make_strand(ioc))
        , ws_(net::make_strand(ioc), ssl_ctx)
        , auth_(api_key, api_secret)
        , reconnect_timer_(ioc)
    {
        msg_builder_ = std::make_unique<CryptoComMessageBuilder>(auth_);
        
        if (use_sandbox) {
            host_ = CryptoComEndpoints::HOST_SANDBOX;
        } else {
            host_ = CryptoComEndpoints::HOST_PROD;
        }
        port_ = CryptoComEndpoints::PORT;
        endpoint_ = CryptoComEndpoints::PATH_USER;
    }

    ~CryptoComWebSocketClient() {
        should_reconnect_ = false;
        close();
    }

    // ───────────────────────────────────────────────────────────────────
    // Callbacks
    // ───────────────────────────────────────────────────────────────────
    void set_message_callback(MessageCallback cb) { on_message_ = std::move(cb); }
    void set_error_callback(ErrorCallback cb) { on_error_ = std::move(cb); }
    void set_connected_callback(ConnectedCallback cb) { on_connected_ = std::move(cb); }
    void set_disconnected_callback(DisconnectedCallback cb) { on_disconnected_ = std::move(cb); }

    void set_callbacks(MessageCallback on_msg, ErrorCallback on_err, 
                       ConnectedCallback on_conn) {
        on_message_ = std::move(on_msg);
        on_error_ = std::move(on_err);
        on_connected_ = std::move(on_conn);
    }

    // ───────────────────────────────────────────────────────────────────
    // Connection Management
    // ───────────────────────────────────────────────────────────────────
    void connect() {
        should_reconnect_ = true;
        reconnect_attempts_ = 0;
        do_connect();
    }

    void close() {
        should_reconnect_ = false;
        if (connected_) {
            connected_ = false;
            authenticated_ = false;
            beast::error_code ec;
            ws_.close(websocket::close_code::normal, ec);
        }
    }

    bool is_connected() const { return connected_; }
    bool is_authenticated() const { return authenticated_; }

    // ───────────────────────────────────────────────────────────────────
    // Order Operations
    // ───────────────────────────────────────────────────────────────────
    void send_order(const Trade& trade) {
        if (!authenticated_) {
            if (on_error_) on_error_("Not authenticated");
            return;
        }
        
        json msg = msg_builder_->build_create_order(trade);
        async_send(msg.dump());
    }

    void cancel_order(const std::string& instrument, uint64_t order_id, 
                      uint64_t client_oid = 0) {
        if (!authenticated_) return;
        json msg = msg_builder_->build_cancel_order(instrument, order_id, client_oid);
        async_send(msg.dump());
    }

    void cancel_all_orders(const std::string& instrument = "") {
        if (!authenticated_) return;
        json msg = msg_builder_->build_cancel_all_orders(instrument);
        async_send(msg.dump());
    }

    void get_open_orders(const std::string& instrument = "") {
        if (!authenticated_) return;
        json msg = msg_builder_->build_get_open_orders(instrument);
        async_send(msg.dump());
    }

    // ───────────────────────────────────────────────────────────────────
    // Subscriptions
    // ───────────────────────────────────────────────────────────────────
    void subscribe_order_updates(const std::string& instrument = "") {
        if (!authenticated_) return;
        json msg = msg_builder_->build_subscribe_user_order(instrument);
        async_send(msg.dump());
    }

    void subscribe_trade_updates(const std::string& instrument = "") {
        if (!authenticated_) return;
        json msg = msg_builder_->build_subscribe_user_trade(instrument);
        async_send(msg.dump());
    }

    void send_raw(const std::string& message) {
        async_send(message);
    }

private:
    void do_connect() {
        ws_.~stream();
        new (&ws_) websocket::stream<beast::ssl_stream<beast::tcp_stream>>(
            net::make_strand(ioc_), ssl_ctx_
        );
        
        resolver_.async_resolve(
            host_,
            port_,
            beast::bind_front_handler(
                &CryptoComWebSocketClient::on_resolve,
                shared_from_this()
            )
        );
    }

    void on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
        if (ec) {
            handle_error("Resolve failed: " + ec.message());
            schedule_reconnect();
            return;
        }
        
        beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(30));
        beast::get_lowest_layer(ws_).async_connect(
            results,
            beast::bind_front_handler(
                &CryptoComWebSocketClient::on_connect,
                shared_from_this()
            )
        );
    }

    void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
        if (ec) {
            handle_error("Connect failed: " + ec.message());
            schedule_reconnect();
            return;
        }
        
        if (!SSL_set_tlsext_host_name(ws_.next_layer().native_handle(), host_.c_str())) {
            handle_error("Failed to set SNI hostname");
            schedule_reconnect();
            return;
        }
        
        beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(30));
        ws_.next_layer().async_handshake(
            ssl::stream_base::client,
            beast::bind_front_handler(
                &CryptoComWebSocketClient::on_ssl_handshake,
                shared_from_this()
            )
        );
    }

    void on_ssl_handshake(beast::error_code ec) {
        if (ec) {
            handle_error("SSL handshake failed: " + ec.message());
            schedule_reconnect();
            return;
        }
        
        beast::get_lowest_layer(ws_).expires_never();
        
        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
        ws_.set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req) {
                req.set(beast::http::field::user_agent, "HFT-Platform/1.0");
            }
        ));
        
        ws_.async_handshake(
            host_,
            endpoint_,
            beast::bind_front_handler(
                &CryptoComWebSocketClient::on_handshake,
                shared_from_this()
            )
        );
    }

    void on_handshake(beast::error_code ec) {
        if (ec) {
            handle_error("WebSocket handshake failed: " + ec.message());
            schedule_reconnect();
            return;
        }
        
        connected_ = true;
        reconnect_attempts_ = 0;
        
        do_read();
        
        json auth_msg = msg_builder_->build_auth_request();
        async_send(auth_msg.dump());
    }

    void do_read() {
        ws_.async_read(
            buffer_,
            beast::bind_front_handler(
                &CryptoComWebSocketClient::on_read,
                shared_from_this()
            )
        );
    }

    void on_read(beast::error_code ec, std::size_t) {
        if (ec) {
            connected_ = false;
            authenticated_ = false;
            
            if (ec != websocket::error::closed) {
                handle_error("Read error: " + ec.message());
            }
            
            if (on_disconnected_) on_disconnected_();
            schedule_reconnect();
            return;
        }
        
        std::string msg_str = beast::buffers_to_string(buffer_.data());
        buffer_.consume(buffer_.size());
        
        try {
            json msg = json::parse(msg_str);
            
            if (CryptoComResponseParser::is_heartbeat(msg)) {
                json response = msg_builder_->build_heartbeat_response(
                    CryptoComResponseParser::get_heartbeat_id(msg)
                );
                async_send(response.dump());
            }
            else if (msg.contains("method") && msg["method"] == "public/auth") {
                if (msg.contains("code") && msg["code"].get<int>() == 0) {
                    authenticated_ = true;
                    if (on_connected_) on_connected_();
                } else {
                    std::string err = "Authentication failed";
                    if (msg.contains("message")) {
                        err += ": " + msg["message"].get<std::string>();
                    }
                    handle_error(err);
                }
            }
            else if (on_message_) {
                on_message_(msg);
            }
        } catch (const std::exception& e) {
            handle_error(std::string("JSON parse error: ") + e.what());
        }
        
        if (connected_) {
            do_read();
        }
    }

    void async_send(const std::string& msg) {
        {
            std::lock_guard<std::mutex> lock(send_mutex_);
            send_queue_.push(msg);
        }
        
        bool expected = false;
        if (sending_.compare_exchange_strong(expected, true)) {
            do_send();
        }
    }

    void do_send() {
        std::string msg;
        {
            std::lock_guard<std::mutex> lock(send_mutex_);
            if (send_queue_.empty()) {
                sending_ = false;
                return;
            }
            msg = std::move(send_queue_.front());
            send_queue_.pop();
        }
        
        auto self = shared_from_this();
        auto msg_ptr = std::make_shared<std::string>(std::move(msg));
        
        ws_.async_write(
            net::buffer(*msg_ptr),
            [self, msg_ptr](beast::error_code ec, std::size_t) {
                if (ec) {
                    self->handle_error("Write error: " + ec.message());
                    self->sending_ = false;
                    return;
                }
                self->do_send();
            }
        );
    }

    void schedule_reconnect() {
        if (!should_reconnect_ || reconnect_attempts_ >= MAX_RECONNECT_ATTEMPTS) {
            if (reconnect_attempts_ >= MAX_RECONNECT_ATTEMPTS) {
                handle_error("Max reconnection attempts reached");
            }
            return;
        }
        
        ++reconnect_attempts_;
        int delay = RECONNECT_DELAY_MS * reconnect_attempts_;
        
        reconnect_timer_.expires_after(std::chrono::milliseconds(delay));
        reconnect_timer_.async_wait([self = shared_from_this()](beast::error_code ec) {
            if (!ec && self->should_reconnect_) {
                self->do_connect();
            }
        });
    }

    void handle_error(const std::string& error) {
        if (on_error_) {
            on_error_(error);
        }
    }
};

} // namespace hft
