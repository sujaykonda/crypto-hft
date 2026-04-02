#pragma once

#include "storage/storage_types.hpp"

#include <chrono>

namespace hft::storage {

class IStorage {
public:
    virtual ~IStorage() = default;

    // Thread-safe and non-blocking append.
    virtual bool append(StorageEvent event) = 0;
    virtual ReplayBatch replay(const ReplayRequest& request) const = 0;
    virtual bool flush(std::chrono::milliseconds timeout) = 0;
    virtual void close() = 0;
    virtual StorageMetrics metrics() const = 0;
};

}  // namespace hft::storage
