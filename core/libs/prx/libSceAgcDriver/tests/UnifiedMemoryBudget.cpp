#include "prx/libSceAgcDriver/Graphics/include/UnifiedMemoryBudget.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr VkDeviceSize MiB = VkDeviceSize{1} << 20u;
constexpr VkDeviceSize DriverUsage = 1 * MiB;
constexpr VkDeviceSize Touched = 64 * MiB;
constexpr VkDeviceSize DriverBudget = 18 * 1024 * MiB;
int driverCalls = 0;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void VKAPI_PTR DriverQuery(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties2* properties) {
    ++driverCalls;
    for (auto* next = static_cast<VkBaseOutStructure*>(properties->pNext); next != nullptr; next = next->pNext) {
        if (next->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT) continue;
        auto* budget = reinterpret_cast<VkPhysicalDeviceMemoryBudgetPropertiesEXT*>(next);
        for (std::uint32_t heap = 0; heap < VK_MAX_MEMORY_HEAPS; ++heap) {
            budget->heapBudget[heap] = DriverBudget;
            budget->heapUsage[heap] = DriverUsage;
        }
    }
}

VkPhysicalDeviceMemoryProperties Heaps(std::vector<VkMemoryHeapFlags> flags) {
    VkPhysicalDeviceMemoryProperties memory{};
    memory.memoryHeapCount = static_cast<std::uint32_t>(flags.size());
    for (std::uint32_t heap = 0; heap < memory.memoryHeapCount; ++heap) memory.memoryHeaps[heap] = {DriverBudget, flags[heap]};
    return memory;
}

VkPhysicalDeviceMemoryBudgetPropertiesEXT Query(PFN_vkGetPhysicalDeviceMemoryProperties2 query) {
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &budget};
    query(VK_NULL_HANDLE, &properties);
    return budget;
}

}

int main() {
    try {
        using AgcDriver::Graphics::UnifiedMemoryBudgetQuery;
        const auto discrete = UnifiedMemoryBudgetQuery(DriverQuery, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, Heaps({VK_MEMORY_HEAP_DEVICE_LOCAL_BIT, 0}));
        Require(discrete == DriverQuery, "a discrete GPU's memory query was replaced");
        const auto split = UnifiedMemoryBudgetQuery(DriverQuery, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, Heaps({VK_MEMORY_HEAP_DEVICE_LOCAL_BIT, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT}));
        Require(split == DriverQuery, "an integrated GPU with two device-local heaps had its memory query replaced");
        Require(UnifiedMemoryBudgetQuery(nullptr, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, Heaps({0, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT})) == nullptr, "a device without VK_EXT_memory_budget gained a memory query");
        const auto unified = UnifiedMemoryBudgetQuery(DriverQuery, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, Heaps({0, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT}));
#ifdef __APPLE__
        Require(unified != DriverQuery, "the unified memory heap kept the driver's memory query");
        const auto touched = std::make_unique<unsigned char[]>(Touched);
        std::memset(touched.get(), 1, Touched);
        Require(std::memchr(touched.get(), 0, Touched) == nullptr, "the touched memory was not written");
        const auto reported = Query(unified);
        Require(driverCalls == 1, "the unified memory query did not ask the driver");
        Require(reported.heapBudget[1] == DriverBudget, "the unified memory query changed the heap budget");
        Require(reported.heapUsage[1] >= Touched, "the unified memory heap's usage does not count the memory the process has touched");
        Require(reported.heapUsage[0] == DriverUsage, "the unified memory query changed a heap that is not device-local");
        std::printf("unified memory budget tests passed (heap usage %llu MiB with the driver reporting %llu MiB)\n", static_cast<unsigned long long>(reported.heapUsage[1] / MiB), static_cast<unsigned long long>(DriverUsage / MiB));
#else
        Require(unified == DriverQuery, "an integrated GPU outside macOS had its memory query replaced");
        std::puts("unified memory budget tests passed (the driver's query is kept outside macOS)");
#endif
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "unified memory budget test failed: %s\n", error.what());
        return 1;
    }
}
