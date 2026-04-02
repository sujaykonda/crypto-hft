#include "storage/jsonl_async_storage.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

namespace hft::storage {

namespace {

using json = nlohmann::json;

constexpr int kSchemaVersion = 1;

template <size_t N>
std::string to_string(const char (&buffer)[N]) {
    return std::string(buffer);
}

template <size_t N>
void copy_string(const std::string& source, char (&destination)[N]) {
    std::strncpy(destination, source.c_str(), N - 1);
    destination[N - 1] = '\0';
}

const char* side_to_string(OrderSide side) {
    return side == OrderSide::BUY ? "BUY" : "SELL";
}

std::optional<OrderSide> side_from_string(const std::string& side) {
    if (side == "BUY") {
        return OrderSide::BUY;
    }
    if (side == "SELL") {
        return OrderSide::SELL;
    }
    return std::nullopt;
}

const char* order_type_to_string(OrderType type) {
    switch (type) {
        case OrderType::LIMIT:
            return "LIMIT";
        case OrderType::MARKET:
            return "MARKET";
        case OrderType::STOP_LOSS:
            return "STOP_LOSS";
        case OrderType::STOP_LIMIT:
            return "STOP_LIMIT";
        case OrderType::TAKE_PROFIT:
            return "TAKE_PROFIT";
        case OrderType::TAKE_PROFIT_LIMIT:
            return "TAKE_PROFIT_LIMIT";
    }
    return "LIMIT";
}

std::optional<OrderType> order_type_from_string(const std::string& type) {
    if (type == "LIMIT") {
        return OrderType::LIMIT;
    }
    if (type == "MARKET") {
        return OrderType::MARKET;
    }
    if (type == "STOP_LOSS") {
        return OrderType::STOP_LOSS;
    }
    if (type == "STOP_LIMIT") {
        return OrderType::STOP_LIMIT;
    }
    if (type == "TAKE_PROFIT") {
        return OrderType::TAKE_PROFIT;
    }
    if (type == "TAKE_PROFIT_LIMIT") {
        return OrderType::TAKE_PROFIT_LIMIT;
    }
    return std::nullopt;
}

const char* tif_to_string(TimeInForce tif) {
    switch (tif) {
        case TimeInForce::GTC:
            return "GTC";
        case TimeInForce::IOC:
            return "IOC";
        case TimeInForce::FOK:
            return "FOK";
        case TimeInForce::GTD:
            return "GTD";
    }
    return "GTC";
}

std::optional<TimeInForce> tif_from_string(const std::string& tif) {
    if (tif == "GTC") {
        return TimeInForce::GTC;
    }
    if (tif == "IOC") {
        return TimeInForce::IOC;
    }
    if (tif == "FOK") {
        return TimeInForce::FOK;
    }
    if (tif == "GTD") {
        return TimeInForce::GTD;
    }
    return std::nullopt;
}

const char* exec_type_to_string(ExecType type) {
    return type == ExecType::POST_ONLY ? "POST_ONLY" : "DEFAULT";
}

std::optional<ExecType> exec_type_from_string(const std::string& type) {
    if (type == "POST_ONLY") {
        return ExecType::POST_ONLY;
    }
    if (type == "DEFAULT") {
        return ExecType::DEFAULT;
    }
    return std::nullopt;
}

const char* status_to_string(OrderStatus status) {
    switch (status) {
        case OrderStatus::NEW:
            return "NEW";
        case OrderStatus::FILLED:
            return "FILLED";
        case OrderStatus::PARTIALLY_FILLED:
            return "PARTIALLY_FILLED";
        case OrderStatus::CANCELED:
            return "CANCELED";
        case OrderStatus::REJECTED:
            return "REJECTED";
        case OrderStatus::EXPIRED:
            return "EXPIRED";
    }
    return "NEW";
}

std::optional<OrderStatus> status_from_string(const std::string& status) {
    if (status == "NEW") {
        return OrderStatus::NEW;
    }
    if (status == "FILLED") {
        return OrderStatus::FILLED;
    }
    if (status == "PARTIALLY_FILLED") {
        return OrderStatus::PARTIALLY_FILLED;
    }
    if (status == "CANCELED" || status == "CANCELLED") {
        return OrderStatus::CANCELED;
    }
    if (status == "REJECTED") {
        return OrderStatus::REJECTED;
    }
    if (status == "EXPIRED") {
        return OrderStatus::EXPIRED;
    }
    return std::nullopt;
}

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

json trade_to_json(const Trade& trade) {
    return json{
        {"client_order_id", trade.client_order_id},
        {"strategy_id", trade.strategy_id},
        {"ticker", to_string(trade.ticker)},
        {"side", side_to_string(trade.side)},
        {"type", order_type_to_string(trade.type)},
        {"time_in_force", tif_to_string(trade.time_in_force)},
        {"exec_type", exec_type_to_string(trade.exec_type)},
        {"quantity", trade.quantity},
        {"price", trade.price},
        {"trigger_price", trade.trigger_price},
        {"timestamp_ns", trade.timestamp_ns},
        {"priority", trade.priority},
    };
}

bool trade_from_json(const json& j, Trade& trade) {
    try {
        const std::optional<OrderSide> side = side_from_string(j.at("side").get<std::string>());
        const std::optional<OrderType> type = order_type_from_string(j.at("type").get<std::string>());
        const std::optional<TimeInForce> tif = tif_from_string(j.at("time_in_force").get<std::string>());
        const std::optional<ExecType> exec = exec_type_from_string(j.at("exec_type").get<std::string>());

        if (!side.has_value() || !type.has_value() || !tif.has_value() || !exec.has_value()) {
            return false;
        }

        trade = Trade{};
        trade.client_order_id = j.at("client_order_id").get<uint64_t>();
        trade.strategy_id = j.at("strategy_id").get<uint32_t>();
        copy_string(j.at("ticker").get<std::string>(), trade.ticker);
        trade.side = side.value();
        trade.type = type.value();
        trade.time_in_force = tif.value();
        trade.exec_type = exec.value();
        trade.quantity = j.at("quantity").get<double>();
        trade.price = j.at("price").get<double>();
        trade.trigger_price = j.at("trigger_price").get<double>();
        trade.timestamp_ns = j.at("timestamp_ns").get<int64_t>();
        trade.priority = j.at("priority").get<uint8_t>();
        return true;
    } catch (...) {
        return false;
    }
}

json response_to_json(const OrderResponse& response) {
    return json{
        {"client_order_id", response.client_order_id},
        {"exchange_order_id", response.exchange_order_id},
        {"status", status_to_string(response.status)},
        {"filled_quantity", response.filled_quantity},
        {"remaining_quantity", response.remaining_quantity},
        {"avg_price", response.avg_price},
        {"fee", response.fee},
        {"fee_currency", to_string(response.fee_currency)},
        {"exchange_timestamp_ns", response.exchange_timestamp_ns},
        {"error_code", response.error_code},
        {"error_message", to_string(response.error_message)},
    };
}

bool response_from_json(const json& j, OrderResponse& response) {
    try {
        const std::optional<OrderStatus> status = status_from_string(j.at("status").get<std::string>());
        if (!status.has_value()) {
            return false;
        }

        response = OrderResponse{};
        response.client_order_id = j.at("client_order_id").get<uint64_t>();
        response.exchange_order_id = j.at("exchange_order_id").get<uint64_t>();
        response.status = status.value();
        response.filled_quantity = j.at("filled_quantity").get<double>();
        response.remaining_quantity = j.at("remaining_quantity").get<double>();
        response.avg_price = j.at("avg_price").get<double>();
        response.fee = j.at("fee").get<double>();
        copy_string(j.at("fee_currency").get<std::string>(), response.fee_currency);
        response.exchange_timestamp_ns = j.at("exchange_timestamp_ns").get<int64_t>();
        response.error_code = j.at("error_code").get<int32_t>();
        copy_string(j.at("error_message").get<std::string>(), response.error_message);
        return true;
    } catch (...) {
        return false;
    }
}

json trade_event_to_json(const TradeEvent& trade_event) {
    return json{
        {"ticker", to_string(trade_event.ticker)},
        {"price", trade_event.price},
        {"quantity", trade_event.quantity},
        {"is_buyer_maker", trade_event.is_buyer_maker},
        {"timestamp_ns", trade_event.timestamp_ns},
        {"trade_id", trade_event.trade_id},
    };
}

bool trade_event_from_json(const json& j, TradeEvent& trade_event) {
    try {
        trade_event = TradeEvent{};
        copy_string(j.at("ticker").get<std::string>(), trade_event.ticker);
        trade_event.price = j.at("price").get<double>();
        trade_event.quantity = j.at("quantity").get<double>();
        trade_event.is_buyer_maker = j.at("is_buyer_maker").get<bool>();
        trade_event.timestamp_ns = j.at("timestamp_ns").get<int64_t>();
        trade_event.trade_id = j.at("trade_id").get<uint64_t>();
        return true;
    } catch (...) {
        return false;
    }
}

json quote_to_json(const QuoteUpdate& quote) {
    return json{
        {"ticker", to_string(quote.ticker)},
        {"bid_price", quote.bid_price},
        {"bid_size", quote.bid_size},
        {"ask_price", quote.ask_price},
        {"ask_size", quote.ask_size},
        {"timestamp_ns", quote.timestamp_ns},
    };
}

bool quote_from_json(const json& j, QuoteUpdate& quote) {
    try {
        quote = QuoteUpdate{};
        copy_string(j.at("ticker").get<std::string>(), quote.ticker);
        quote.bid_price = j.at("bid_price").get<double>();
        quote.bid_size = j.at("bid_size").get<double>();
        quote.ask_price = j.at("ask_price").get<double>();
        quote.ask_size = j.at("ask_size").get<double>();
        quote.timestamp_ns = j.at("timestamp_ns").get<int64_t>();
        return true;
    } catch (...) {
        return false;
    }
}

json storage_event_to_json(const StorageEvent& event) {
    json payload;
    switch (event.type) {
        case StorageEventType::TradeSubmitted:
            payload = trade_to_json(std::get<Trade>(event.payload));
            break;
        case StorageEventType::OrderResponse:
            payload = response_to_json(std::get<OrderResponse>(event.payload));
            break;
        case StorageEventType::MarketTrade:
            payload = trade_event_to_json(std::get<TradeEvent>(event.payload));
            break;
        case StorageEventType::QuoteUpdate:
            payload = quote_to_json(std::get<QuoteUpdate>(event.payload));
            break;
    }

    json serialized{
        {"schema_version", kSchemaVersion},
        {"offset", event.offset},
        {"ingest_ts_ns", event.ingest_ts_ns},
        {"event_type", hft::storage::to_string(event.type)},
        {"payload", std::move(payload)},
    };

    const std::optional<std::string> ticker = event_ticker(event);
    if (ticker.has_value()) {
        serialized["ticker"] = ticker.value();
    }

    return serialized;
}

bool storage_event_from_json(const json& j, StorageEvent& event) {
    try {
        if (!j.contains("offset") || !j.contains("ingest_ts_ns") || !j.contains("event_type") || !j.contains("payload")) {
            return false;
        }

        const std::optional<StorageEventType> type =
            storage_event_type_from_string(j.at("event_type").get<std::string>());
        if (!type.has_value()) {
            return false;
        }

        event = StorageEvent{};
        event.offset = j.at("offset").get<uint64_t>();
        event.ingest_ts_ns = j.at("ingest_ts_ns").get<int64_t>();
        event.type = type.value();

        const json& payload = j.at("payload");
        switch (event.type) {
            case StorageEventType::TradeSubmitted: {
                Trade trade{};
                if (!trade_from_json(payload, trade)) {
                    return false;
                }
                event.payload = trade;
                break;
            }
            case StorageEventType::OrderResponse: {
                OrderResponse response{};
                if (!response_from_json(payload, response)) {
                    return false;
                }
                event.payload = response;
                break;
            }
            case StorageEventType::MarketTrade: {
                TradeEvent trade_event{};
                if (!trade_event_from_json(payload, trade_event)) {
                    return false;
                }
                event.payload = trade_event;
                break;
            }
            case StorageEventType::QuoteUpdate: {
                QuoteUpdate quote{};
                if (!quote_from_json(payload, quote)) {
                    return false;
                }
                event.payload = quote;
                break;
            }
        }

        return true;
    } catch (...) {
        return false;
    }
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

JsonlAsyncStorage::JsonlAsyncStorage(JsonlAsyncStorageConfig config)
    : config_(std::move(config)) {
    if (config_.file_path.empty()) {
        throw std::invalid_argument("JsonlAsyncStorageConfig.file_path must be set");
    }
    if (config_.queue_capacity == 0) {
        config_.queue_capacity = 1;
    }
    if (config_.batch_size == 0) {
        config_.batch_size = 1;
    }

    load_existing_file();
    open_output_file();

    if (config_.start_flusher_thread) {
        flusher_thread_ = std::thread(&JsonlAsyncStorage::flusher_loop, this);
    }
}

JsonlAsyncStorage::~JsonlAsyncStorage() {
    close();
}

bool JsonlAsyncStorage::append(StorageEvent event) {
    if (!payload_matches_type(event)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (closed_.load(std::memory_order_acquire)) {
            return false;
        }
        if (queue_.size() >= config_.queue_capacity) {
            rejected_backpressure_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        event.offset = next_offset_++;
        if (event.ingest_ts_ns <= 0) {
            event.ingest_ts_ns = now_ns();
        }

        queue_.push_back(std::move(event));
        queue_depth_.store(static_cast<uint64_t>(queue_.size()), std::memory_order_release);
        accepted_.fetch_add(1, std::memory_order_relaxed);
    }

    queue_cv_.notify_one();
    return true;
}

ReplayBatch JsonlAsyncStorage::replay(const ReplayRequest& request) const {
    const size_t limit = request.max_events == 0 ? std::numeric_limits<size_t>::max() : request.max_events;

    ReplayBatch batch;
    batch.end_of_stream = true;

    std::lock_guard<std::mutex> lock(committed_mutex_);
    for (const StorageEvent& event : committed_events_) {
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

bool JsonlAsyncStorage::flush(std::chrono::milliseconds timeout) {
    flush_count_.fetch_add(1, std::memory_order_relaxed);

    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + timeout;

    if (!config_.start_flusher_thread) {
        return flush_without_worker(deadline);
    }

    queue_cv_.notify_one();

    std::unique_lock<std::mutex> lock(flush_mutex_);
    while (true) {
        const uint64_t accepted = accepted_.load(std::memory_order_acquire);
        const uint64_t committed = committed_.load(std::memory_order_acquire);
        const uint64_t queued = queue_depth_.load(std::memory_order_acquire);
        if (committed >= accepted && queued == 0) {
            break;
        }

        if (flush_cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
            const uint64_t accepted_after = accepted_.load(std::memory_order_acquire);
            const uint64_t committed_after = committed_.load(std::memory_order_acquire);
            const uint64_t queued_after = queue_depth_.load(std::memory_order_acquire);
            return committed_after >= accepted_after && queued_after == 0;
        }
    }

    std::lock_guard<std::mutex> file_lock(file_mutex_);
    output_.flush();
    return output_.good();
}

void JsonlAsyncStorage::close() {
    const bool was_already_closed = closed_.exchange(true, std::memory_order_acq_rel);
    if (was_already_closed) {
        return;
    }

    queue_cv_.notify_all();

    if (config_.start_flusher_thread && flusher_thread_.joinable()) {
        flusher_thread_.join();
    }

    if (!config_.start_flusher_thread) {
        (void)flush(std::chrono::seconds(5));
    }

    std::lock_guard<std::mutex> file_lock(file_mutex_);
    if (output_.is_open()) {
        output_.flush();
        output_.close();
    }

    queue_depth_.store(0, std::memory_order_release);
    flush_cv_.notify_all();
}

StorageMetrics JsonlAsyncStorage::metrics() const {
    return StorageMetrics{
        accepted_.load(std::memory_order_relaxed),
        rejected_backpressure_.load(std::memory_order_relaxed),
        committed_.load(std::memory_order_relaxed),
        flush_count_.load(std::memory_order_relaxed),
        parse_errors_.load(std::memory_order_relaxed),
        queue_depth_.load(std::memory_order_relaxed),
    };
}

bool JsonlAsyncStorage::flush_without_worker(const std::chrono::steady_clock::time_point& deadline) {
    while (true) {
        const uint64_t accepted = accepted_.load(std::memory_order_acquire);
        const uint64_t committed = committed_.load(std::memory_order_acquire);
        const uint64_t queued = queue_depth_.load(std::memory_order_acquire);
        if (committed >= accepted && queued == 0) {
            break;
        }

        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }

        std::vector<StorageEvent> batch;
        if (!drain_queue_batch(batch, config_.batch_size)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        write_batch(batch);
    }

    std::lock_guard<std::mutex> file_lock(file_mutex_);
    output_.flush();
    return output_.good();
}

bool JsonlAsyncStorage::drain_queue_batch(std::vector<StorageEvent>& batch, size_t max_items) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (queue_.empty()) {
        return false;
    }

    const size_t items = std::min(max_items, queue_.size());
    batch.reserve(items);
    for (size_t i = 0; i < items; ++i) {
        batch.push_back(std::move(queue_.front()));
        queue_.pop_front();
    }

    queue_depth_.store(static_cast<uint64_t>(queue_.size()), std::memory_order_release);
    return true;
}

void JsonlAsyncStorage::flusher_loop() {
    while (true) {
        std::vector<StorageEvent> batch;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait_for(lock, config_.flush_interval, [this] {
                return closed_.load(std::memory_order_acquire) || !queue_.empty();
            });

            if (queue_.empty()) {
                if (closed_.load(std::memory_order_acquire)) {
                    break;
                }
                continue;
            }

            const size_t items = std::min(config_.batch_size, queue_.size());
            batch.reserve(items);
            for (size_t i = 0; i < items; ++i) {
                batch.push_back(std::move(queue_.front()));
                queue_.pop_front();
            }

            queue_depth_.store(static_cast<uint64_t>(queue_.size()), std::memory_order_release);
        }

        write_batch(batch);
        flush_cv_.notify_all();
    }

    flush_cv_.notify_all();
}

void JsonlAsyncStorage::write_batch(const std::vector<StorageEvent>& batch) {
    if (batch.empty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> file_lock(file_mutex_);
        for (const StorageEvent& event : batch) {
            output_ << storage_event_to_json(event).dump() << '\n';
        }
        output_.flush();
    }

    {
        std::lock_guard<std::mutex> lock(committed_mutex_);
        committed_events_.insert(committed_events_.end(), batch.begin(), batch.end());
    }

    committed_.fetch_add(static_cast<uint64_t>(batch.size()), std::memory_order_release);
}

void JsonlAsyncStorage::load_existing_file() {
    const std::filesystem::path parent = config_.file_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    if (!std::filesystem::exists(config_.file_path)) {
        next_offset_ = 0;
        accepted_.store(0, std::memory_order_relaxed);
        committed_.store(0, std::memory_order_relaxed);
        return;
    }

    std::ifstream input(config_.file_path);
    if (!input.is_open()) {
        throw std::runtime_error("Failed to open storage file for replay scan: " + config_.file_path.string());
    }

    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }

        const json parsed = json::parse(line, nullptr, false);
        if (parsed.is_discarded()) {
            parse_errors_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        StorageEvent event;
        if (!storage_event_from_json(parsed, event)) {
            parse_errors_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        committed_events_.push_back(std::move(event));
    }

    std::sort(committed_events_.begin(), committed_events_.end(), [](const StorageEvent& lhs, const StorageEvent& rhs) {
        return lhs.offset < rhs.offset;
    });

    uint64_t initial_offset = 0;
    if (!committed_events_.empty()) {
        initial_offset = committed_events_.back().offset + 1;
    }

    next_offset_ = initial_offset;
    const uint64_t initial_committed = static_cast<uint64_t>(committed_events_.size());
    accepted_.store(initial_committed, std::memory_order_relaxed);
    committed_.store(initial_committed, std::memory_order_relaxed);
}

void JsonlAsyncStorage::open_output_file() {
    output_.open(config_.file_path, std::ios::out | std::ios::app);
    if (!output_.is_open()) {
        throw std::runtime_error("Failed to open storage file for append: " + config_.file_path.string());
    }
}

}  // namespace hft::storage
