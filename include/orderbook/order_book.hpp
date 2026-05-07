#pragma once

#include "types/trade.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hft::orderbook {

struct BookLevel {
    double price{0.0};
    double quantity{0.0};
};

struct BookSnapshot {
    std::string ticker;
    std::vector<BookLevel> bids;
    std::vector<BookLevel> asks;
    int64_t timestamp_ns{0};
    uint64_t sequence{0};
};

class OrderBook {
public:
    explicit OrderBook(std::string ticker = {}) : ticker_(std::move(ticker)) {}

    void apply_snapshot(const std::vector<BookLevel>& bids,
                        const std::vector<BookLevel>& asks,
                        int64_t timestamp_ns,
                        uint64_t sequence = 0) {
        std::unique_lock lock(mutex_);
        bids_.clear();
        asks_.clear();
        for (const BookLevel& level : bids) {
            upsert_bid(level.price, level.quantity);
        }
        for (const BookLevel& level : asks) {
            upsert_ask(level.price, level.quantity);
        }
        timestamp_ns_ = timestamp_ns;
        sequence_ = sequence;
    }

    void apply_delta(const std::vector<BookLevel>& bids,
                     const std::vector<BookLevel>& asks,
                     int64_t timestamp_ns,
                     uint64_t sequence = 0) {
        std::unique_lock lock(mutex_);
        for (const BookLevel& level : bids) {
            upsert_bid(level.price, level.quantity);
        }
        for (const BookLevel& level : asks) {
            upsert_ask(level.price, level.quantity);
        }
        timestamp_ns_ = timestamp_ns;
        sequence_ = sequence;
    }

    std::optional<QuoteUpdate> best_quote() const {
        std::shared_lock lock(mutex_);
        if (bids_.empty() || asks_.empty()) {
            return std::nullopt;
        }

        QuoteUpdate quote{};
        copy_string(ticker_, quote.ticker);
        quote.bid_price = bids_.begin()->first;
        quote.bid_size = bids_.begin()->second;
        quote.ask_price = asks_.begin()->first;
        quote.ask_size = asks_.begin()->second;
        quote.timestamp_ns = timestamp_ns_;
        return quote;
    }

    BookSnapshot snapshot(size_t depth = 0) const {
        std::shared_lock lock(mutex_);
        BookSnapshot snapshot;
        snapshot.ticker = ticker_;
        snapshot.timestamp_ns = timestamp_ns_;
        snapshot.sequence = sequence_;

        const size_t bid_depth = depth == 0 ? bids_.size() : std::min(depth, bids_.size());
        const size_t ask_depth = depth == 0 ? asks_.size() : std::min(depth, asks_.size());
        snapshot.bids.reserve(bid_depth);
        snapshot.asks.reserve(ask_depth);

        size_t count = 0;
        for (const auto& [price, quantity] : bids_) {
            if (count++ >= bid_depth) {
                break;
            }
            snapshot.bids.push_back(BookLevel{price, quantity});
        }

        count = 0;
        for (const auto& [price, quantity] : asks_) {
            if (count++ >= ask_depth) {
                break;
            }
            snapshot.asks.push_back(BookLevel{price, quantity});
        }

        return snapshot;
    }

    const std::string& ticker() const { return ticker_; }

private:
    template <size_t N>
    static void copy_string(const std::string& source, char (&destination)[N]) {
        std::strncpy(destination, source.c_str(), N - 1);
        destination[N - 1] = '\0';
    }

    void upsert_bid(double price, double quantity) {
        if (price <= 0.0) {
            return;
        }
        if (quantity <= 0.0) {
            bids_.erase(price);
            return;
        }
        bids_[price] = quantity;
    }

    void upsert_ask(double price, double quantity) {
        if (price <= 0.0) {
            return;
        }
        if (quantity <= 0.0) {
            asks_.erase(price);
            return;
        }
        asks_[price] = quantity;
    }

    std::string ticker_;
    mutable std::shared_mutex mutex_;
    std::map<double, double, std::greater<double>> bids_;
    std::map<double, double, std::less<double>> asks_;
    int64_t timestamp_ns_{0};
    uint64_t sequence_{0};
};

struct ConsolidatedQuote {
    QuoteUpdate quote{};
    std::string bid_exchange;
    std::string ask_exchange;
};

class MultiExchangeOrderBook {
public:
    OrderBook& book_for(const std::string& exchange, const std::string& ticker) {
        std::lock_guard lock(mutex_);
        const std::string key = exchange + ":" + ticker;
        auto it = books_.find(key);
        if (it == books_.end()) {
            it = books_.emplace(key, std::make_unique<OrderBook>(ticker)).first;
            book_index_[ticker].push_back(BookRef{exchange, it->second.get()});
        }
        return *it->second;
    }

    std::optional<ConsolidatedQuote> best_quote(const std::string& ticker) const {
        std::lock_guard lock(mutex_);
        const auto index_it = book_index_.find(ticker);
        if (index_it == book_index_.end()) {
            return std::nullopt;
        }

        std::optional<ConsolidatedQuote> best;
        for (const BookRef& ref : index_it->second) {
            const auto quote = ref.book->best_quote();
            if (!quote.has_value()) {
                continue;
            }

            if (!best.has_value()) {
                best = ConsolidatedQuote{*quote, ref.exchange, ref.exchange};
                continue;
            }

            if (quote->bid_price > best->quote.bid_price) {
                best->quote.bid_price = quote->bid_price;
                best->quote.bid_size = quote->bid_size;
                best->bid_exchange = ref.exchange;
            }
            if (quote->ask_price < best->quote.ask_price) {
                best->quote.ask_price = quote->ask_price;
                best->quote.ask_size = quote->ask_size;
                best->ask_exchange = ref.exchange;
            }
            best->quote.timestamp_ns = std::max(best->quote.timestamp_ns, quote->timestamp_ns);
        }

        return best;
    }

private:
    struct BookRef {
        std::string exchange;
        OrderBook* book{nullptr};
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<OrderBook>> books_;
    std::unordered_map<std::string, std::vector<BookRef>> book_index_;
};

}  // namespace hft::orderbook
