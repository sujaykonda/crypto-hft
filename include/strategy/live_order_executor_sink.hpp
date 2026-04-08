#pragma once

#include "storage/storage_interface.hpp"
#include "strategy/execution_sink.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace hft {

class LiveOrderExecutorSink final : public IExecutionSink {
public:
    struct Stats {
        uint64_t submitted_events_seen{0};
        uint64_t response_events_seen{0};
        uint64_t storage_append_failures{0};
    };

    using SubmitFn = std::function<bool(const Trade&, OrderCallback)>;
    using CancelFn = std::function<void(const std::string&, uint64_t, uint64_t)>;
    using CancelAllFn = std::function<void(const std::string&)>;

    LiveOrderExecutorSink(SubmitFn submit_fn,
                          CancelFn cancel_fn,
                          CancelAllFn cancel_all_fn,
                          std::shared_ptr<storage::IStorage> storage = nullptr)
        : submit_fn_(std::move(submit_fn)),
          cancel_fn_(std::move(cancel_fn)),
          cancel_all_fn_(std::move(cancel_all_fn)),
          storage_(std::move(storage)) {}

    bool submit_trade(const Trade& trade,
                      OrderCallback response_callback,
                      StorageFailureCallback storage_failure_callback) override {
        const bool submitted = submit_fn_(
            trade,
            [this,
             response_callback = std::move(response_callback),
             storage_failure_callback = std::move(storage_failure_callback)](
                const OrderResponse& response) mutable {
                append_storage_event(storage::StorageEvent{
                                         0,
                                         storage::now_ns(),
                                         storage::StorageEventType::OrderResponse,
                                         response},
                                     storage_failure_callback);
                response_events_seen_.fetch_add(1, std::memory_order_relaxed);

                if (response_callback) {
                    response_callback(response);
                }
            });

        if (!submitted) {
            return false;
        }

        append_storage_event(storage::StorageEvent{
                                 0,
                                 storage::now_ns(),
                                 storage::StorageEventType::TradeSubmitted,
                                 trade},
                             storage_failure_callback);
        submitted_events_seen_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void cancel_order(const std::string& instrument,
                      uint64_t order_id,
                      uint64_t client_oid = 0) override {
        cancel_fn_(instrument, order_id, client_oid);
    }

    void cancel_all_orders(const std::string& instrument = "") override {
        cancel_all_fn_(instrument);
    }

    Stats stats() const {
        return Stats{
            submitted_events_seen_.load(std::memory_order_relaxed),
            response_events_seen_.load(std::memory_order_relaxed),
            storage_append_failures_.load(std::memory_order_relaxed),
        };
    }

private:
    bool append_storage_event(storage::StorageEvent event,
                              const StorageFailureCallback& storage_failure_callback) {
        if (!storage_) {
            return true;
        }

        const bool appended = storage_->append(std::move(event));
        if (!appended) {
            storage_append_failures_.fetch_add(1, std::memory_order_relaxed);
            if (storage_failure_callback) {
                storage_failure_callback();
            }
        }

        return appended;
    }

    SubmitFn submit_fn_;
    CancelFn cancel_fn_;
    CancelAllFn cancel_all_fn_;
    std::shared_ptr<storage::IStorage> storage_;

    std::atomic<uint64_t> submitted_events_seen_{0};
    std::atomic<uint64_t> response_events_seen_{0};
    std::atomic<uint64_t> storage_append_failures_{0};
};

}  // namespace hft
