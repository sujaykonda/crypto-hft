# SIF Crypto HFT Platform Overview

Last updated: 2026-05-06

This document summarizes the current project state, how the pieces fit together,
how to build and test it, and what still requires live exchange credentials.

## Current Status

The repository now builds as an offline-testable C++20 MVP for a crypto HFT
platform. It includes:

- Storage interfaces and JSONL/in-memory implementations
- Crypto.com websocket market data parsing
- Crypto.com authenticated order message builders/parsers
- Strategy runtime and execution sink abstraction
- Simulated execution for offline backtests
- Thread-safe order book and multi-exchange BBO coalescing primitives
- Basic risk manager for order limits and position tracking
- Unix-domain socket command protocol plus a Python client
- CMake targets and GoogleTest coverage for the core pieces

The platform is not production-ready trading infrastructure yet. Live order
testing still needs sandbox exchange credentials, conservative risk limits, and
more end-to-end soak testing.

## Deliverable Coverage

| Deliverable | Status | Notes |
| --- | --- | --- |
| Parse exchange websocket feeds | Partial | Crypto.com market trade/book parsing exists. Coinbase and other exchanges are not implemented. |
| Concurrent orderbook | Partial | Thread-safe per-symbol order book and multi-exchange BBO coalescer exist. Live market-data wiring is still basic. |
| UDS connection between C++ and Python | Partial | C++ Unix-domain command server and Python client exist. The server is not started by `main` by default yet. |
| Python commands for subscribe/snapshot | Partial | `python/hft_client.py` has `ping`, `subscribe`, and `snapshot` commands. |
| REST/order management | Partial | Crypto.com authenticated websocket order messages exist. REST HTTP client is not implemented. |
| Risk management | MVP | Order quantity/notional/net-position checks and fill-based position updates exist. |
| Move trading logic into C++ | MVP | Example strategies run in C++ through `StrategyBase`. |
| Backtester | MVP | Replay driver and simulated execution are implemented and tested offline. |

## Architecture

Live path:

```text
Crypto.com market websocket
  -> CryptoComMarketDataClient
  -> StrategyManager
  -> StrategyBase implementations
  -> RiskCheckedExecutionSink
  -> LiveOrderExecutorSink
  -> OrderExecutor
  -> CryptoComWebSocketClient
  -> Crypto.com user/order websocket
```

Offline backtest path:

```text
JSONL storage file
  -> JsonlAsyncStorage replay
  -> BacktestReplayDriver
  -> StrategyManager
  -> SimulatedExecutionSink
  -> OrderResponse callbacks
```

Python IPC path:

```text
python/hft_client.py
  -> Unix domain socket JSON line command
  -> UnixDomainCommandServer
  -> subscribe/snapshot callbacks
```

## Important Source Files

Storage:

- `include/storage/storage_interface.hpp`
- `include/storage/storage_types.hpp`
- `include/storage/in_memory_storage.hpp`
- `include/storage/jsonl_async_storage.hpp`
- `src/storage/in_memory_storage.cpp`
- `src/storage/jsonl_async_storage.cpp`
- `src/storage/storage_types.cpp`

Exchange:

- `include/exchange/auth_handler.hpp`
- `include/exchange/crypto_com_api.hpp`
- `include/exchange/market_data_client.hpp`
- `include/exchange/websocket_client.hpp`
- `src/exchange/market_data_client.cpp`

Execution and queues:

- `include/core/trade_queue.hpp`
- `include/core/order_executor.hpp`
- `include/types/trade.hpp`

Strategy runtime:

- `include/strategy/strategy_base.hpp`
- `include/strategy/example_strategies.hpp`
- `include/strategy/execution_sink.hpp`
- `include/strategy/live_order_executor_sink.hpp`
- `include/strategy/storage_replay_driver.hpp`

Backtesting:

- `include/backtest/simulated_execution_sink.hpp`
- `include/backtest/backtest_replay_driver.hpp`

Order book:

- `include/orderbook/order_book.hpp`

Risk:

- `include/risk/risk_manager.hpp`

IPC and Python:

- `include/ipc/client_protocol.hpp`
- `include/ipc/unix_domain_server.hpp`
- `python/hft_client.py`

Tests:

- `tests/test_storage.cpp`
- `tests/test_strategy_runtime.cpp`
- `tests/test_exchange_api.cpp`
- `tests/test_backtest.cpp`
- `tests/test_orderbook_risk.cpp`
- `tests/test_ipc.cpp`

## Build

Release platform build:

```bash
cmake --build build -j4
```

Test build:

```bash
cmake --build build_storage -j4
```

If you need to configure from scratch:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake -S . -B build_storage -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
```

## Test

Run the full C++ test suite:

```bash
ctest --test-dir build_storage --output-on-failure --timeout 10
```

Run only the backtest tests:

```bash
./build_storage/test_backtest
```

Run only exchange parser/signing tests:

```bash
./build_storage/test_exchange_api
```

Run only orderbook and risk tests:

```bash
./build_storage/test_orderbook_risk
```

Run only IPC tests:

```bash
./build_storage/test_ipc
```

Note: the Unix-domain socket integration test may need to run outside a sandbox
that blocks local socket bind operations. In a normal terminal, it should pass.

## Offline Backtesting

Offline backtests do not need API keys.

Run the platform backtest mode:

```bash
./build/hft_platform --backtest /path/to/storage.jsonl
```

An empty or missing JSONL file is valid but produces zero events:

```text
Backtest complete
  Events processed: 0
  Quote events:     0
  Trade events:     0
  Orders submitted: 0
  Orders filled:    0
  Orders rejected:  0
```

For a meaningful backtest, the file must contain storage events in the JSONL
format written by `JsonlAsyncStorage`. Live collection can write this file by
setting `CRYPTO_HFT_STORAGE_PATH`.

## Live Mode

Live mode currently initializes authenticated order execution, so it requires
Crypto.com credentials.

Use sandbox/UAT credentials first:

```bash
export CRYPTO_COM_API_KEY='your_key'
export CRYPTO_COM_API_SECRET='your_secret'
export CRYPTO_COM_SANDBOX=true
export CRYPTO_HFT_STORAGE_PATH=/tmp/crypto_hft_live.jsonl
export CRYPTO_HFT_MARKET_INSTRUMENTS=BTC_USDT,ETH_USDT
export CRYPTO_HFT_MAX_ORDER_QTY=0.001
export CRYPTO_HFT_MAX_ORDER_NOTIONAL=50
export CRYPTO_HFT_MAX_NET_POSITION_ABS=0.002
./build/hft_platform
```

Do not paste API secrets into chat or commit them to the repository.

Production mode is selected by:

```bash
export CRYPTO_COM_SANDBOX=false
```

Only do this after sandbox testing and after setting strict risk limits.

## Environment Variables

| Variable | Required | Description |
| --- | --- | --- |
| `CRYPTO_COM_API_KEY` | Live mode | Crypto.com API key |
| `CRYPTO_COM_API_SECRET` | Live mode | Crypto.com API secret |
| `CRYPTO_COM_SANDBOX` | No | Defaults to sandbox unless set to `false` |
| `CRYPTO_HFT_STORAGE_PATH` | No | JSONL path for live event persistence |
| `CRYPTO_HFT_MARKET_INSTRUMENTS` | No | Comma-separated market symbols |
| `CRYPTO_HFT_MAX_ORDER_QTY` | No | Risk limit per order quantity |
| `CRYPTO_HFT_MAX_ORDER_NOTIONAL` | No | Risk limit per order notional |
| `CRYPTO_HFT_MAX_NET_POSITION_ABS` | No | Risk limit for absolute net position |

## Python Client

The Python client talks to a C++ Unix-domain socket command server.

```bash
python3 python/hft_client.py --socket /tmp/crypto_hft.sock ping
python3 python/hft_client.py --socket /tmp/crypto_hft.sock subscribe BTC_USDT ETH_USDT
python3 python/hft_client.py --socket /tmp/crypto_hft.sock snapshot BTC_USDT
```

The protocol is newline-delimited JSON:

```json
{"cmd":"ping"}
{"cmd":"subscribe","tickers":["BTC_USDT","ETH_USDT"]}
{"cmd":"snapshot","ticker":"BTC_USDT"}
```

Responses use:

```json
{"ok":true,"result":{}}
{"ok":false,"error":"message"}
```

Current limitation: the command server class is implemented and tested, but
`main` does not start it by default. It needs a runtime integration step to bind
the server callbacks to the live orderbook/subscription state.

## Storage Format

Storage events are represented by:

- `TradeSubmitted`
- `OrderResponse`
- `MarketTrade`
- `QuoteUpdate`

Replay supports:

- start offset
- start timestamp
- end timestamp
- ticker filter
- max events/page size

See `docs/STORAGE_INTERFACES_V1.md` for deeper storage details.

## Risk Model

`RiskManager` checks:

- positive quantity
- optional market-order disablement
- max order quantity
- max order notional
- max absolute net position

It tracks:

- net quantity
- average entry price
- realized PnL

`RiskCheckedExecutionSink` wraps any `IExecutionSink` and rejects orders before
they reach live or simulated execution.

## Simulated Execution Model

`SimulatedExecutionSink` supports:

- market orders filled against current quote
- market-order rejection when no quote is available
- limit orders that cross current quote
- resting limit orders filled by later quote/trade events
- cancel order and cancel all
- gross notional and fee accounting

This is intentionally simple. It is enough to validate strategy plumbing and
basic fill behavior, but it is not a realistic queue-position simulator.

## Known Gaps

- Coinbase and other exchange connectors are not implemented.
- Crypto.com REST HTTP order management is not implemented.
- UDS server is implemented but not started in `main`.
- The live market-data client emits top-of-book quotes, not full-depth book
  objects into the strategy layer.
- The orderbook module is tested independently but not fully wired into live
  market-data dispatch.
- Backtest fills are simplified and do not model queue priority, partial fills,
  exchange latency, slippage, or maker/taker classification perfectly.
- No Dockerfile or deployment automation is present.
- No long-running live sandbox soak test has been run in this environment.

## Recommended Next Steps

1. Add a market-data-only live collection mode that does not require private API
   keys.
2. Wire `UnixDomainCommandServer` into `main` behind an env var or CLI flag.
3. Feed full book snapshots/deltas into `OrderBook` and expose snapshots through
   the UDS server.
4. Add a Crypto.com REST client for portfolio/funds/order management if needed.
5. Add a safer paper-trading mode where generated orders only hit
   `SimulatedExecutionSink`.
6. Add Docker and CI so the full test suite runs consistently.
7. Run sandbox exchange soak tests with restrictive risk limits before any
   production mode.

## Command Cheat Sheet

```bash
# Build live executable
cmake --build build -j4

# Build tests
cmake --build build_storage -j4

# Run all tests
ctest --test-dir build_storage --output-on-failure --timeout 10

# Run offline backtest
./build/hft_platform --backtest /path/to/storage.jsonl

# Show platform usage
./build/hft_platform --help

# Python client examples
python3 python/hft_client.py --socket /tmp/crypto_hft.sock ping
python3 python/hft_client.py --socket /tmp/crypto_hft.sock subscribe BTC_USDT
python3 python/hft_client.py --socket /tmp/crypto_hft.sock snapshot BTC_USDT
```
