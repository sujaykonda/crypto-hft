#pragma once

#include <cstdint>
#include <string>
#include <chrono>
#include <atomic>
#include <optional>
#include <cstring>

namespace hft {

// ═══════════════════════════════════════════════════════════════════════════
// Order Side & Type Enums
// ═══════════════════════════════════════════════════════════════════════════

enum class OrderSide : uint8_t {
    BUY  = 0,
    SELL = 1
};

enum class OrderType : uint8_t {
    LIMIT          = 0,
    MARKET         = 1,
    STOP_LOSS      = 2,
    STOP_LIMIT     = 3,
    TAKE_PROFIT    = 4,
    TAKE_PROFIT_LIMIT = 5
};

enum class TimeInForce : uint8_t {
    GTC = 0,  // Good Till Cancel
    IOC = 1,  // Immediate Or Cancel
    FOK = 2,  // Fill Or Kill
    GTD = 3   // Good Till Date
};

enum class ExecType : uint8_t {
    POST_ONLY = 0,  // Maker only
    DEFAULT   = 1   // Taker allowed
};

// ═══════════════════════════════════════════════════════════════════════════
// Trade Structure - Core format for all strategies
// ═══════════════════════════════════════════════════════════════════════════
// 
// Design goals:
//   - Cache-line aligned for lock-free queue efficiency
//   - Fixed-size strings avoid heap allocations in hot path
//   - Nanosecond timestamps for latency measurement
//   - Strategy ID for multi-strategy correlation

struct alignas(64) Trade {
    // ─────────────────────────────────────────────────────────────────────
    // Identifiers
    // ─────────────────────────────────────────────────────────────────────
    uint64_t client_order_id;              // Unique ID (monotonic counter)
    uint32_t strategy_id;                  // Source strategy identifier
    char     ticker[16];                   // Instrument symbol (e.g., "BTC_USDT")

    // ─────────────────────────────────────────────────────────────────────
    // Order Parameters
    // ─────────────────────────────────────────────────────────────────────
    OrderSide   side;
    OrderType   type;
    TimeInForce time_in_force;
    ExecType    exec_type;

    double quantity;                       // Order size
    double price;                          // Limit price (0 for market orders)
    double trigger_price;                  // For stop/take-profit orders

    // ─────────────────────────────────────────────────────────────────────
    // Timing & Priority
    // ─────────────────────────────────────────────────────────────────────
    int64_t  timestamp_ns;                 // Creation timestamp (nanoseconds)
    uint8_t  priority;                     // 0 = highest, 255 = lowest

    // ═══════════════════════════════════════════════════════════════════
    // Factory Methods
    // ═══════════════════════════════════════════════════════════════════

    static Trade create_limit_order(
        uint32_t strategy_id,
        const char* ticker,
        OrderSide side,
        double quantity,
        double price,
        TimeInForce tif = TimeInForce::GTC,
        ExecType exec = ExecType::DEFAULT
    ) {
        static std::atomic<uint64_t> order_id_counter{0};
        
        Trade t{};
        t.client_order_id = order_id_counter.fetch_add(1, std::memory_order_relaxed);
        t.strategy_id = strategy_id;
        std::strncpy(t.ticker, ticker, sizeof(t.ticker) - 1);
        t.ticker[sizeof(t.ticker) - 1] = '\0';
        t.side = side;
        t.type = OrderType::LIMIT;
        t.time_in_force = tif;
        t.exec_type = exec;
        t.quantity = quantity;
        t.price = price;
        t.trigger_price = 0.0;
        t.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()
        ).count();
        t.priority = 128;  // Default priority
        return t;
    }

    static Trade create_market_order(
        uint32_t strategy_id,
        const char* ticker,
        OrderSide side,
        double quantity
    ) {
        Trade t = create_limit_order(strategy_id, ticker, side, quantity, 0.0);
        t.type = OrderType::MARKET;
        t.time_in_force = TimeInForce::IOC;
        t.priority = 0;  // Market orders get highest priority
        return t;
    }

    static Trade create_stop_order(
        uint32_t strategy_id,
        const char* ticker,
        OrderSide side,
        double quantity,
        double trigger_price,
        std::optional<double> limit_price = std::nullopt
    ) {
        Trade t = create_limit_order(strategy_id, ticker, side, quantity, 
                                     limit_price.value_or(0.0));
        t.type = limit_price.has_value() ? OrderType::STOP_LIMIT : OrderType::STOP_LOSS;
        t.trigger_price = trigger_price;
        t.priority = 64;  // Higher priority than regular limit orders
        return t;
    }
};

static_assert(alignof(Trade) == 64, "Trade must be 64-byte aligned");
static_assert(sizeof(Trade) % 64 == 0, "Trade size must be a multiple of cache-line size");

// ═══════════════════════════════════════════════════════════════════════════
// Order Response from Exchange
// ═══════════════════════════════════════════════════════════════════════════

enum class OrderStatus : uint8_t {
    NEW        = 0,
    FILLED     = 1,
    PARTIALLY_FILLED = 2,
    CANCELED   = 3,
    REJECTED   = 4,
    EXPIRED    = 5
};

struct OrderResponse {
    uint64_t    client_order_id;
    uint64_t    exchange_order_id;
    OrderStatus status;
    double      filled_quantity;
    double      remaining_quantity;
    double      avg_price;
    double      fee;
    char        fee_currency[8];
    int64_t     exchange_timestamp_ns;
    int32_t     error_code;
    char        error_message[128];

    OrderResponse() 
        : client_order_id(0)
        , exchange_order_id(0)
        , status(OrderStatus::NEW)
        , filled_quantity(0.0)
        , remaining_quantity(0.0)
        , avg_price(0.0)
        , fee(0.0)
        , exchange_timestamp_ns(0)
        , error_code(0) {
        fee_currency[0] = '\0';
        error_message[0] = '\0';
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Trade Event - For strategy callbacks
// ═══════════════════════════════════════════════════════════════════════════

struct TradeEvent {
    char     ticker[16];
    double   price;
    double   quantity;
    bool     is_buyer_maker;
    int64_t  timestamp_ns;
    uint64_t trade_id;
};

// ═══════════════════════════════════════════════════════════════════════════
// Quote Update - Best bid/ask
// ═══════════════════════════════════════════════════════════════════════════

struct QuoteUpdate {
    char   ticker[16];
    double bid_price;
    double bid_size;
    double ask_price;
    double ask_size;
    int64_t timestamp_ns;
};

} // namespace hft
