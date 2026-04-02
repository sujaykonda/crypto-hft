#pragma once

#include "storage/storage_interface.hpp"

#include <atomic>
#include <mutex>
#include <vector>

namespace hft::storage {

class InMemoryStorage final : public IStorage {
public:
    InMemoryStorage() = default;
    ~InMemoryStorage() override = default;

    bool append(StorageEvent event) override;
    ReplayBatch replay(const ReplayRequest& request) const override;
    bool flush(std::chrono::milliseconds timeout) override;
    void close() override;
    StorageMetrics metrics() const override;

private:
    mutable std::mutex mutex_;
    std::vector<StorageEvent> events_;
    uint64_t next_offset_{0};
    bool closed_{false};

    std::atomic<uint64_t> accepted_{0};
    std::atomic<uint64_t> committed_{0};
    std::atomic<uint64_t> flush_count_{0};
};

}  // namespace hft::storage
