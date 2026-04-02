#pragma once

#include "core/trade_queue.hpp"
#include "exchange/websocket_client.hpp"
#include <unordered_map>
#include <chrono>
#include <iostream>

namespace hft {

// ═══════════════════════════════════════════════════════════════════════════
// Rate Limiter - Crypto.com has rate limits we must respect
// ═══════════════════════════════════════════════════════════════════════════

class RateLimiter {
    const size_t max_requests_per_second_;
    const size_t max_requests_per_100ms_;
    
    std::atomic<size_t> requests_this_second_{0};
    std::atomic<size_t> requests_this_100ms_{0};
    std::atomic<int64_t> current_second_{0};
    std::atomic<int64_t> current_100ms_{0};

public:
    explicit RateLimiter(size_t max_per_second = 100, size_t max_per_100ms = 15)
        : max_requests_per_second_(max_per_second)
        , max_requests_per_100ms_(max_per_100ms) {}

    bool try_acquire() {
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ).count();
        
        int64_t second = ms / 1000;
        int64_t interval_100ms = ms / 100;
        
        int64_t last_second = current_second_.load();
        if (second != last_second) {
            if (current_second_.compare_exchange_strong(last_second, second)) {
                requests_this_second_ = 0;
            }
        }
        
        int64_t last_100ms = current_100ms_.load();
        if (interval_100ms != last_100ms) {
            if (current_100ms_.compare_exchange_strong(last_100ms, interval_100ms)) {
                requests_this_100ms_ = 0;
            }
        }
        
        if (requests_this_second_ >= max_requests_per_second_ ||
            requests_this_100ms_ >= max_requests_per_100ms_) {
            return false;
        }
        
        requests_this_second_.fetch_add(1);
        requests_this_100ms_.fetch_add(1);
        return true;
    }

    void reset() {
        requests_this_second_ = 0;
        requests_this_100ms_ = 0;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Order Callback - Strategy receives execution updates
// ═══════════════════════════════════════════════════════════════════════════

using OrderCallback = std::function<void(const OrderResponse&)>;

// ═══════════════════════════════════════════════════════════════════════════
// Order Executor - Main execution engine
// ═══════════════════════════════════════════════════════════════════════════

class OrderExecutor {
    std::shared_ptr<CryptoComWebSocketClient> ws_client_;
    MPSCTradeQueue<65536> trade_queue_;
    RateLimiter rate_limiter_;
    
    std::unordered_map<uint64_t, OrderCallback> pending_orders_;
    std::mutex pending_mutex_;
    
    std::atomic<bool> running_{false};
    std::thread executor_thread_;
    
    // Statistics
    std::atomic<uint64_t> orders_sent_{0};
    std::atomic<uint64_t> orders_filled_{0};
    std::atomic<uint64_t> orders_rejected_{0};
    std::atomic<int64_t> total_latency_ns_{0};

public:
    explicit OrderExecutor(std::shared_ptr<CryptoComWebSocketClient> client)
        : ws_client_(std::move(client)) {
        
        ws_client_->set_callbacks(
            [this](const json& msg) { handle_message(msg); },
            [this](const std::string& err) { handle_error(err); },
            [this]() { on_connected(); }
        );
    }

    ~OrderExecutor() {
        stop();
    }

    // ───────────────────────────────────────────────────────────────────
    // Submit trade from strategy
    // ───────────────────────────────────────────────────────────────────
    bool submit_trade(const Trade& trade, OrderCallback callback = nullptr) {
        if (!running_) return false;
        
        if (callback) {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_orders_[trade.client_order_id] = std::move(callback);
        }
        
        return trade_queue_.try_push(trade);
    }

    // ───────────────────────────────────────────────────────────────────
    // Start/Stop execution loop
    // ───────────────────────────────────────────────────────────────────
    void start() {
        running_ = true;
        ws_client_->connect();
        executor_thread_ = std::thread(&OrderExecutor::execution_loop, this);
    }

    void stop() {
        running_ = false;
        if (executor_thread_.joinable()) {
            executor_thread_.join();
        }
        ws_client_->close();
    }

    bool is_running() const { return running_; }

    // ───────────────────────────────────────────────────────────────────
    // Cancel operations
    // ───────────────────────────────────────────────────────────────────
    void cancel_order(const std::string& instrument, uint64_t order_id, 
                      uint64_t client_oid = 0) {
        ws_client_->cancel_order(instrument, order_id, client_oid);
    }

    void cancel_all_orders(const std::string& instrument = "") {
        ws_client_->cancel_all_orders(instrument);
    }

    // ───────────────────────────────────────────────────────────────────
    // Statistics
    // ───────────────────────────────────────────────────────────────────
    struct Stats {
        uint64_t orders_sent;
        uint64_t orders_filled;
        uint64_t orders_rejected;
        double avg_latency_us;
    };

    Stats get_stats() const {
        uint64_t sent = orders_sent_.load();
        return Stats{
            sent,
            orders_filled_.load(),
            orders_rejected_.load(),
            sent > 0 ? total_latency_ns_.load() / (sent * 1000.0) : 0.0
        };
    }

    void reset_stats() {
        orders_sent_ = 0;
        orders_filled_ = 0;
        orders_rejected_ = 0;
        total_latency_ns_ = 0;
    }

private:
    void execution_loop() {
        while (running_) {
            if (!ws_client_->is_authenticated()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            
            bool processed = false;
            while (auto trade = trade_queue_.try_pop()) {
                if (!rate_limiter_.try_acquire()) {
                    trade_queue_.try_push(*trade);
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    break;
                }
                
                ws_client_->send_order(*trade);
                orders_sent_.fetch_add(1);
                processed = true;
            }
            
            if (!processed) {
                std::this_thread::yield();
            }
        }
    }

    void handle_message(const json& msg) {
        if (!msg.contains("method")) return;
        
        std::string method = msg["method"].get<std::string>();
        
        if (method == "private/create-order" || 
            method == "private/cancel-order" ||
            CryptoComResponseParser::is_order_update(msg)) {
            
            OrderResponse response = CryptoComResponseParser::parse_order_response(msg);
            
            if (response.status == OrderStatus::FILLED) {
                orders_filled_.fetch_add(1);
            } else if (response.status == OrderStatus::REJECTED) {
                orders_rejected_.fetch_add(1);
            }
            
            if (response.exchange_timestamp_ns > 0) {
                auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::high_resolution_clock::now().time_since_epoch()
                ).count();
                total_latency_ns_.fetch_add(now_ns - response.exchange_timestamp_ns);
            }
            
            OrderCallback callback;
            {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                auto it = pending_orders_.find(response.client_order_id);
                if (it != pending_orders_.end()) {
                    callback = std::move(it->second);
                    if (response.status == OrderStatus::FILLED ||
                        response.status == OrderStatus::CANCELED ||
                        response.status == OrderStatus::REJECTED ||
                        response.status == OrderStatus::EXPIRED) {
                        pending_orders_.erase(it);
                    }
                }
            }
            
            if (callback) {
                callback(response);
            }
        }
    }

    void handle_error(const std::string& error) {
        std::cerr << "[OrderExecutor] Error: " << error << std::endl;
    }

    void on_connected() {
        std::cout << "[OrderExecutor] Connected and authenticated" << std::endl;
        
        // Subscribe to order updates
        ws_client_->subscribe_order_updates();
        ws_client_->subscribe_trade_updates();
    }
};

} // namespace hft
