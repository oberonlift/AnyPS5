#include "Async.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/include/Api.hpp"
#include <chrono>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace Http2 {

void AsyncOperations::Start(int eventType, const Http2AsyncOption* notification, std::function<int()> operation) {
    {
        std::lock_guard lock(mutex);
        if (running) NotImplemented_nid_no_patch("sceHttp2: a second asynchronous operation on a busy request");
        running = true;
    }
    std::optional<Http2AsyncOption> notify;
    if (notification) notify = *notification;
    std::thread([self = shared_from_this(), eventType, notify, operation = std::move(operation)] {
        Completion completion{eventType, 0, nullptr};
        try {
            completion.result = operation();
        } catch (...) {
            completion.failure = std::current_exception();
        }
        {
            std::lock_guard lock(self->mutex);
            self->completions.push_back(completion);
            self->running = false;
        }
        self->completed.notify_all();
        if (notify && sceKernelTriggerUserEvent(notify->equeue, notify->user_event_id, notify->user_data) != 0) throw std::runtime_error("sceHttp2: the completion's user event could not be triggered");
    }).detach();
}

int AsyncOperations::Wait(int requestId, Http2AsyncResult* result, std::uint32_t timeout) {
    std::unique_lock lock(mutex);
    if (!completed.wait_for(lock, std::chrono::microseconds(timeout), [&] { return !completions.empty(); })) return ErrorAsyncNotReady;
    const auto completion = completions.front();
    completions.pop_front();
    lock.unlock();
    if (completion.failure) std::rethrow_exception(completion.failure);
    *result = Http2AsyncResult{};
    result->event_type = completion.eventType;
    result->req_id = requestId;
    result->result = completion.result;
    return 0;
}

}
