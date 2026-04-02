#include "storage/in_memory_storage.hpp"

#include <limits>

namespace hft::storage {

namespace {

bool payload_matches_type(const StorageEvent& event) {
    switch (event.type) {
        case StorageEventType::TradeSubmitted:
            return std::holds_alternative<Trade>(event.payload);
        case StorageEventType::OrderResponse:
            return std::holds_alternative<OrderResponse>(event.payload);
        case StorageEventType::MarketTrade:
            return std::holds_alternative<TradeEvent>(event.payload);
        case StorageEventType::QuoteUpdate:
            return std::holds_alternative<QuoteUpdate>(event.payload);
    }
    return false;
}

bool matches_replay_filter(const StorageEvent& event, const ReplayRequest& request) {
    if (request.start_offset.has_value() && event.offset < request.start_offset.value()) {
        return false;
    }
    if (request.start_ts_ns.has_value() && event.ingest_ts_ns < request.start_ts_ns.value()) {
        return false;
    }
    if (request.end_ts_ns.has_value() && event.ingest_ts_ns > request.end_ts_ns.value()) {
        return false;
    }
    if (request.ticker.has_value()) {
        const std::optional<std::string> ticker = event_ticker(event);
        if (!ticker.has_value() || ticker.value() != request.ticker.value()) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool InMemoryStorage::append(StorageEvent event) {
    if (!payload_matches_type(event)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
        return false;
    }

    event.offset = next_offset_++;
    if (event.ingest_ts_ns <= 0) {
        event.ingest_ts_ns = now_ns();
    }

    events_.push_back(std::move(event));
    accepted_.fetch_add(1, std::memory_order_relaxed);
    committed_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

ReplayBatch InMemoryStorage::replay(const ReplayRequest& request) const {
    const size_t limit = request.max_events == 0 ? std::numeric_limits<size_t>::max() : request.max_events;

    ReplayBatch batch;
    batch.end_of_stream = true;

    std::lock_guard<std::mutex> lock(mutex_);
    for (const StorageEvent& event : events_) {
        if (!matches_replay_filter(event, request)) {
            continue;
        }

        if (batch.events.size() >= limit) {
            batch.end_of_stream = false;
            if (!batch.events.empty()) {
                batch.next_offset = batch.events.back().offset + 1;
            }
            break;
        }

        batch.events.push_back(event);
    }

    return batch;
}

bool InMemoryStorage::flush(std::chrono::milliseconds) {
    flush_count_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void InMemoryStorage::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
}

StorageMetrics InMemoryStorage::metrics() const {
    return StorageMetrics{
        accepted_.load(std::memory_order_relaxed),
        0,
        committed_.load(std::memory_order_relaxed),
        flush_count_.load(std::memory_order_relaxed),
        0,
        0,
    };
}

}  // namespace hft::storage
