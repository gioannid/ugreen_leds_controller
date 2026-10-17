#include "hardware_worker.h"
#include "logger.h"

#include <sys/eventfd.h>
#include <unistd.h>

namespace ugreen {

HardwareWorker::HardwareWorker(std::unique_ptr<LedBackend> backend)
    : backend_(std::move(backend)) {
    wakeup_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
}

HardwareWorker::~HardwareWorker() {
    stop();
    if (wakeup_fd_ >= 0) {
        close(wakeup_fd_);
        wakeup_fd_ = -1;
    }
}

void HardwareWorker::start() {
    if (running_.exchange(true)) {
        return;
    }
    log::debug("hardware worker thread started");
    thread_ = std::thread(&HardwareWorker::worker_loop, this);
}

void HardwareWorker::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        cv_.notify_all();
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    log::debug("hardware worker thread stopped");
}

bool HardwareWorker::submit_batch(HardwareBatch batch) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    // Replace pending batch if already one waiting (per Section 7)
    pending_batch_ = std::move(batch);
    cv_.notify_one();
    return true;
}

bool HardwareWorker::submit_read(HardwareReadRequest req) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    pending_reads_.push(req);
    cv_.notify_one();
    return true;
}

std::vector<HardwareBatchResult> HardwareWorker::pop_completed_batches() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    std::vector<HardwareBatchResult> res;
    res.swap(completed_batches_);
    return res;
}

std::vector<HardwareReadResult> HardwareWorker::pop_completed_reads() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    std::vector<HardwareReadResult> res;
    res.swap(completed_reads_);
    return res;
}

void HardwareWorker::worker_loop() {
    while (running_) {
        std::optional<HardwareBatch> current_batch;
        std::optional<HardwareReadRequest> current_read;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            cv_.wait(lock, [this]() {
                return !running_ || pending_batch_.has_value() || !pending_reads_.empty();
            });

            if (!running_) break;

            if (pending_batch_.has_value()) {
                current_batch = std::move(pending_batch_);
                pending_batch_.reset();
            } else if (!pending_reads_.empty()) {
                current_read = pending_reads_.front();
                pending_reads_.pop();
            }
        }

        bool notified = false;

        if (current_batch) {
            HardwareBatchResult res;
            res.batch_id = current_batch->batch_id;
            res.max_generation = current_batch->max_generation;
            res.success = true;

            for (const auto& op : current_batch->operations) {
                try {
                    backend_->apply(op.id, op.patch);
                    res.applied_states.push_back({op.id, op.target_state});
                } catch (const std::exception& ex) {
                    res.success = false;
                    res.failed_led = op.id;
                    res.error_message = ex.what();
                    break;
                }
            }

            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                completed_batches_.push_back(std::move(res));
            }
            notified = true;
        } else if (current_read) {
            HardwareReadResult res;
            res.client_id = current_read->client_id;
            res.request_id = current_read->request_id;
            res.id = current_read->id;

            try {
                res.state = backend_->read(current_read->id);
                res.success = true;
            } catch (const std::exception& ex) {
                res.success = false;
                res.error_message = ex.what();
            }

            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                completed_reads_.push_back(std::move(res));
            }
            notified = true;
        }

        if (notified && wakeup_fd_ >= 0) {
            uint64_t val = 1;
            (void)write(wakeup_fd_, &val, sizeof(val));
        }
    }
}

} // namespace ugreen
