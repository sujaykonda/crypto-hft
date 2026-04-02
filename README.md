# Crypto HFT Platform

A high-frequency trading platform for crypto.com exchange written in C++20.

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           HFT Platform Architecture                         │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐                      │
│  │  Strategy A  │  │  Strategy B  │  │  Strategy N  │                      │
│  │  (BTC_USDT)  │  │  (ETH_USDT)  │  │  (SOL_USDT)  │                      │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘                      │
│         │                 │                 │                               │
│         └────────────┬────┴────────────────┘                               │
│                      ▼                                                      │
│         ┌────────────────────────┐                                         │
│         │   Lock-Free Trade      │                                         │
│         │   Queue (MPSC)         │                                         │
│         └───────────┬────────────┘                                         │
│                     ▼                                                       │
│         ┌────────────────────────┐                                         │
│         │   Order Executor       │◄──── Rate Limiter                       │
│         └───────────┬────────────┘                                         │
│                     ▼                                                       │
│         ┌────────────────────────┐                                         │
│         │  WebSocket Client      │◄──── Auth (HMAC-SHA256)                 │
│         │  (Boost.Beast)         │                                         │
│         └───────────┬────────────┘                                         │
│                     ▼                                                       │
│            wss://stream.crypto.com/exchange/v1/user                        │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

## Requirements

- C++20 compatible compiler (GCC 10+, Clang 12+, MSVC 2019+)
- CMake 3.16+
- Boost 1.75+ (system component)
- OpenSSL 1.1+
- nlohmann_json 3.9+ (auto-fetched if not found)

### macOS

```bash
brew install cmake boost openssl nlohmann-json
```

### Ubuntu/Debian

```bash
sudo apt install cmake libboost-all-dev libssl-dev nlohmann-json3-dev
```

## Building

```bash
cd ~/crypto_hft

# Create build directory
mkdir -p build && cd build

# Configure (Release mode)
cmake -DCMAKE_BUILD_TYPE=Release ..

# Build
make -j$(nproc)
```

### Build Options

```bash
# Debug build with sanitizers
cmake -DCMAKE_BUILD_TYPE=Debug ..

# Build with tests
cmake -DBUILD_TESTS=ON ..
```

## Running

### Environment Variables

```bash
# Required: API credentials
export CRYPTO_COM_API_KEY='your_api_key'
export CRYPTO_COM_API_SECRET='your_api_secret'

# Optional: Use production (default is sandbox)
export CRYPTO_COM_SANDBOX=false
```

### Start the Platform

```bash
./hft_platform
```

## Trade Format

The core trade structure is 64-byte cache-aligned for lock-free queue efficiency:

```cpp
struct alignas(64) Trade {
    uint64_t client_order_id;    // Unique monotonic ID
    uint32_t strategy_id;        // Source strategy
    char     ticker[16];         // e.g., "BTC_USDT"
    
    OrderSide   side;            // BUY or SELL
    OrderType   type;            // LIMIT, MARKET, STOP_LOSS, etc.
    TimeInForce time_in_force;   // GTC, IOC, FOK
    ExecType    exec_type;       // DEFAULT or POST_ONLY
    
    double quantity;
    double price;
    double trigger_price;
    
    int64_t timestamp_ns;        // Nanosecond precision
    uint8_t priority;            // 0=highest, 255=lowest
};
```

## Creating Custom Strategies

Inherit from `StrategyBase`:

```cpp
class MyStrategy : public hft::StrategyBase {
public:
    MyStrategy(uint32_t id, const std::string& ticker)
        : StrategyBase(id, "MyStrategy", {ticker}) {}

    void on_quote(const std::string& ticker, 
                  double bid, double ask,
                  double bid_size, double ask_size) override {
        // React to quote updates
        if (should_buy(bid, ask)) {
            place_limit_order(ticker.c_str(), OrderSide::BUY, 
                            0.001, bid + 0.01);
        }
    }

    void on_trade(const std::string& ticker,
                  double price, double size, bool is_buy) override {
        // React to trade events
    }
};
```

## Project Structure

```
crypto_hft/
├── CMakeLists.txt
├── README.md
├── include/
│   ├── types/
│   │   └── trade.hpp           # Trade format & order types
│   ├── core/
│   │   ├── trade_queue.hpp     # Lock-free SPSC/MPSC queues
│   │   └── order_executor.hpp  # Order execution engine
│   ├── exchange/
│   │   ├── auth_handler.hpp    # HMAC-SHA256 authentication
│   │   ├── crypto_com_api.hpp  # Message builder & parser
│   │   └── websocket_client.hpp # Boost.Beast WebSocket
│   └── strategy/
│       └── strategy_base.hpp   # Strategy interface
└── src/
    └── main.cpp                # Example application
```

## Key Features

- **Lock-free queues**: SPSC and MPSC implementations for low-latency order flow
- **Cache-aligned trades**: 64-byte struct eliminates false sharing
- **Rate limiting**: Respects crypto.com's 100 req/s and 15 req/100ms limits
- **Auto-reconnection**: Exponential backoff with configurable max attempts
- **Heartbeat handling**: Automatic response to keep WebSocket alive
- **Priority queuing**: Market orders processed before limit orders

## Crypto.com API Methods

The platform supports:

| Method | Description |
|--------|-------------|
| `private/create-order` | Submit new order |
| `private/cancel-order` | Cancel by order_id or client_oid |
| `private/cancel-all-orders` | Cancel all (optionally by instrument) |
| `private/get-open-orders` | Query active orders |
| `subscribe user.order` | Real-time order updates |
| `subscribe user.trade` | Real-time fill notifications |

## Performance Tips

1. **Use POST_ONLY** for maker-only orders (lower fees)
2. **Batch order updates** rather than individual cancels/creates
3. **Monitor rate limits** via `RateLimiter::try_acquire()`
4. **Use priority queue** for time-sensitive orders

## License

MIT License - Use at your own risk for trading.
# temp
