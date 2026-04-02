#pragma once

#include "storage/storage_interface.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace hft::storage {

struct JsonlAsyncStorageConfig {
    std::filesystem::path file_path;
    size_t queue_capacity{65536};
    size_t batch_size{256};
    std::chrono::milliseconds flush_interval{25};
    bool start_flusher_thread{true};
};

class JsonlAsyncStorage final : public IStorage {
public:
    explicit JsonlAsyncStorage(JsonlAsyncStorageConfig config);
    ~JsonlAsyncStorage() override;

    JsonlAsyncStorage(const JsonlAsyncStorage&) = delete;
    JsonlAsyncStorage& operator=(const JsonlAsyncStorage&) = delete;
    JsonlAsyncStorage(JsonlAsyncStorage&&) = delete;
    JsonlAsyncStorage& operator=(JsonlAsyncStorage&&) = delete;

    bool append(StorageEvent event) override;
    ReplayBatch replay(const ReplayRequest& request) const override;
    bool flush(std::chrono::milliseconds timeout) override;
    void close() override;
    StorageMetrics metrics() const override;

private:
    bool flush_without_worker(const std::chrono::steady_clock::time_point& deadline);
    bool drain_queue_batch(std::vector<StorageEvent>& batch, size_t max_items);
    void flusher_loop();
    void write_batch(const std::vector<StorageEvent>& batch);
    void load_existing_file();
    void open_output_file();

    JsonlAsyncStorageConfig config_;

    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<StorageEvent> queue_;
    std::atomic<uint64_t> queue_depth_{0};

    mutable std::mutex committed_mutex_;
    std::vector<StorageEvent> committed_events_;

    mutable std::mutex file_mutex_;
    std::ofstream output_;

    std::atomic<bool> closed_{false};
    std::thread flusher_thread_;

    mutable std::mutex flush_mutex_;
    std::condition_variable flush_cv_;

    uint64_t next_offset_{0};

    std::atomic<uint64_t> accepted_{0};
    std::atomic<uint64_t> rejected_backpressure_{0};
    std::atomic<uint64_t> committed_{0};
    std::atomic<uint64_t> flush_count_{0};
    std::atomic<uint64_t> parse_errors_{0};
};

}  // namespace hft::storage
