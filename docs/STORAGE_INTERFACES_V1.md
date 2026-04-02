# Storage Interfaces v1 (Sujay) - Mini Doc

## 1) What was added

A standalone storage subsystem was added to persist and replay execution + market events.

### New public API headers
- `include/storage/storage_types.hpp`
- `include/storage/storage_interface.hpp`
- `include/storage/in_memory_storage.hpp`
- `include/storage/jsonl_async_storage.hpp`

### New implementations
- `src/storage/storage_types.cpp`
- `src/storage/in_memory_storage.cpp`
- `src/storage/jsonl_async_storage.cpp`

### New tests
- `tests/test_storage.cpp`

### Build system updates
- Added `hft_storage` library target
- Replaced old broken test targets with GTest-based `test_storage`
- Added `find_package(GTest CONFIG QUIET)` with `FetchContent` fallback

---

## 2) Core concepts

### Event model
All persisted records are normalized into `StorageEvent`:
- `offset`: monotonically assigned sequence ID
- `ingest_ts_ns`: nanosecond timestamp for replay filtering
- `type`: one of:
  - `TradeSubmitted`
  - `OrderResponse`
  - `MarketTrade`
  - `QuoteUpdate`
- `payload`: `std::variant<Trade, OrderResponse, TradeEvent, QuoteUpdate>`

### Replay model
`ReplayRequest` supports:
- `start_offset`
- `start_ts_ns`
- `end_ts_ns`
- `ticker`
- `max_events` (`0` means unlimited)

`ReplayBatch` returns:
- matching `events`
- `next_offset` when pagination is needed
- `end_of_stream`

### Metrics model
`StorageMetrics` tracks:
- `accepted`
- `rejected_backpressure`
- `committed`
- `flush_count`
- `parse_errors`
- `queue_depth`

---

## 3) Interface contract (`IStorage`)

```cpp
class IStorage {
public:
  virtual ~IStorage() = default;
  virtual bool append(StorageEvent event) = 0;
  virtual ReplayBatch replay(const ReplayRequest& request) const = 0;
  virtual bool flush(std::chrono::milliseconds timeout) = 0;
  virtual void close() = 0;
  virtual StorageMetrics metrics() const = 0;
};
```

Important behavior:
- `append` is non-blocking and thread-safe
- `append` returns `false` if closed or backpressured
- offsets are monotonic for accepted writes
- replay order is strictly by offset
- async JSONL replay reads committed (written) records

---

## 4) Backend behavior

## `InMemoryStorage`
- Multi-producer safe via mutex
- Assigns offsets in append path
- Stores committed events in memory vector
- `flush()` is a no-op that increments flush counter
- Useful for fast tests and in-process simulation

## `JsonlAsyncStorage`
- Bounded queue (default `65536`)
- Background flusher thread (default enabled)
- Batch write size default `256`
- Flush interval default `25ms`
- Persists one JSON object per line
- On startup:
  - scans existing file
  - rebuilds in-memory committed replay index
  - skips malformed lines and increments `parse_errors`
- `close()` drains pending data and closes file

### Test-only convenience in config
`start_flusher_thread` can be disabled for deterministic backpressure tests.

---

## 5) JSONL format

Each line uses this shape:

```json
{
  "schema_version": 1,
  "offset": 42,
  "ingest_ts_ns": 1730000000000,
  "event_type": "quote_update",
  "ticker": "BTC_USDT",
  "payload": { ...type-specific fields... }
}
```

`event_type` values:
- `trade_submitted`
- `order_response`
- `market_trade`
- `quote_update`

---

## 6) Test coverage that was implemented

`tests/test_storage.cpp` includes:
1. `StorageInterface.InMemoryRoundTrip`
2. `StorageInterface.ReplayFilters`
3. `StorageInterface.MonotonicOffsetsMultiProducer`
4. `JsonlAsync.Backpressure`
5. `JsonlAsync.FlushAndDurability`
6. `JsonlAsync.CloseBehavior`
7. `JsonlAsync.MalformedLineRecovery`

All 7 tests are passing in the current build.

---

## 7) How to build and run tests

```bash
cmake -S . -B build_storage -DBUILD_TESTS=ON
cmake --build build_storage -j8
ctest --test-dir build_storage --output-on-failure
```

Note: if GTest is not installed locally, CMake fetches it from GitHub.

---

## 8) Current scope boundaries

- Standalone module only (not wired into `OrderExecutor` / `StrategyManager` yet)
- Sequential replay only (no indexed query engine)
- Single-process file ownership assumption
- No log rotation/compression yet

---

## 9) Suggested next integration step

Inject an `IStorage*` (or `std::shared_ptr<IStorage>`) into execution/market data paths and append:
- `TradeSubmitted` when orders enter the queue
- `OrderResponse` on exchange callbacks
- `MarketTrade` and `QuoteUpdate` in market-data dispatch

