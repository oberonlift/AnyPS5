#include "prx/libSceAgcDriver/Graphics/include/UnifiedMemoryBudget.hpp"
#ifdef __APPLE__
#include <mach/mach.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <optional>
#include <stdexcept>
#endif

namespace AgcDriver::Graphics {

#ifdef __APPLE__
namespace {

std::atomic<PFN_vkGetPhysicalDeviceMemoryProperties2> driverQuery{nullptr};
std::atomic<std::uint32_t> sharedHeap{0};

// Metal's allocations count towards the process's physical footprint, with the guest's memory and everything else
// the process has dirtied.
std::uint64_t ProcessFootprint() {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) throw std::runtime_error("AGC graphics: cannot read the process's memory footprint for the unified memory budget");
    return info.phys_footprint;
}

void VKAPI_PTR UnifiedQuery(VkPhysicalDevice physical, VkPhysicalDeviceMemoryProperties2* properties) {
    driverQuery.load(std::memory_order_acquire)(physical, properties);
    for (auto* next = static_cast<VkBaseOutStructure*>(properties->pNext); next != nullptr; next = next->pNext) {
        if (next->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT) continue;
        auto& usage = reinterpret_cast<VkPhysicalDeviceMemoryBudgetPropertiesEXT*>(next)->heapUsage[sharedHeap.load(std::memory_order_relaxed)];
        usage = std::max<VkDeviceSize>(usage, ProcessFootprint());
    }
}

std::optional<std::uint32_t> OnlyDeviceLocalHeap(const VkPhysicalDeviceMemoryProperties& memory) {
    std::optional<std::uint32_t> found;
    for (std::uint32_t heap = 0; heap < memory.memoryHeapCount; ++heap) {
        if ((memory.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0) continue;
        if (found) return std::nullopt;
        found = heap;
    }
    return found;
}

}
#endif

PFN_vkGetPhysicalDeviceMemoryProperties2 UnifiedMemoryBudgetQuery(PFN_vkGetPhysicalDeviceMemoryProperties2 query, VkPhysicalDeviceType type, const VkPhysicalDeviceMemoryProperties& memory) {
#ifdef __APPLE__
    const auto heap = OnlyDeviceLocalHeap(memory);
    if (query == nullptr || type != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU || !heap) return query;
    const auto installed = driverQuery.exchange(query, std::memory_order_acq_rel);
    if (installed != nullptr && installed != query) throw std::runtime_error("AGC graphics: the unified memory budget wraps one driver's memory query per process");
    sharedHeap.store(*heap, std::memory_order_relaxed);
    return UnifiedQuery;
#else
    static_cast<void>(type);
    static_cast<void>(memory);
    return query;
#endif
}

}
