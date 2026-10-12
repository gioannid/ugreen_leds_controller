#ifndef UGREEN_SERVER_HARDWARE_WORKER_H
#define UGREEN_SERVER_HARDWARE_WORKER_H

#include "led_backend.h"
#include "scheduler.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <vector>

namespace ugreen {

struct HardwareReadRequest {
    uint64_t client_id = 0;
    uint64_t request_id = 0;
    LedId id = LedId::Power;
};

struct HardwareReadResult {
    uint64_t client_id = 0;
    uint64_t request_id = 0;
    LedId id = LedId::Power;
    bool success = true;
    LedState state;
    std::string error_message;
};

class HardwareWorker {
public:
    explicit HardwareWorker(std::unique_ptr<LedBackend> backend);
    ~HardwareWorker();

    void start();
    void stop();

    // The wakeup descriptor to monitor in reactor's poll()
    int wakeup_fd() const { return wakeup_fd_; }

    // Submits work from reactor thread
    bool submit_batch(HardwareBatch batch);
    bool submit_read(HardwareReadRequest req);

    // Pops completed items (called by reactor thread when wakeup_fd is readable)
    std::vector<HardwareBatchResult> pop_completed_batches();
    std::vector<HardwareReadResult> pop_completed_reads();

    // Backend query (read-only capabilities)
    Capabilities capabilities() const { return backend_->capabilities(); }

private:
    void worker_loop();

    std::unique_ptr<LedBackend> backend_;
    int wakeup_fd_ = -1;
    std::thread thread_;
    std::atomic<bool> running_{false};

    mutable std::mutex queue_mutex_;
    std::condition_variable cv_;

    std::optional<HardwareBatch> pending_batch_;
    std::queue<HardwareReadRequest> pending_reads_;

    std::vector<HardwareBatchResult> completed_batches_;
    std::vector<HardwareReadResult> completed_reads_;
};

} // namespace ugreen

#endif // UGREEN_SERVER_HARDWARE_WORKER_H
