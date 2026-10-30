#pragma once

#include "SceTypes.hpp"
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>

namespace Http2 {

inline constexpr int AsyncSendEvent = 0;
inline constexpr int AsyncReadEvent = 1;
inline constexpr int ErrorAsyncNotReady = static_cast<int>(0x817B1079);

class AsyncOperations : public std::enable_shared_from_this<AsyncOperations> {
public:
    void Start(int eventType, const Http2AsyncOption* notification, std::function<int()> operation);
    int Wait(int requestId, Http2AsyncResult* result, std::uint32_t timeout);

private:
    struct Completion {
        int eventType;
        int result;
        std::exception_ptr failure;
    };
    std::mutex mutex;
    std::condition_variable completed;
    std::deque<Completion> completions;
    bool running = false;
};

}
