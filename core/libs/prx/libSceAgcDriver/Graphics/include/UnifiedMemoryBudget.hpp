#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_UNIFIEDMEMORYBUDGET_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_UNIFIEDMEMORYBUDGET_HPP

#include <vulkan/vulkan.h>

namespace AgcDriver::Graphics {

// The memory query the budgets read. On an integrated GPU with one device-local heap on macOS the GPU allocates from
// the process's own memory, while VK_EXT_memory_budget counts only the GPU's allocations as the heap's usage; the
// returned query reports the process's whole footprint as that heap's usage instead, so the caches sized from the
// budget leave the guest's own memory room. Any other device keeps the driver's query.
PFN_vkGetPhysicalDeviceMemoryProperties2 UnifiedMemoryBudgetQuery(PFN_vkGetPhysicalDeviceMemoryProperties2 query, VkPhysicalDeviceType type, const VkPhysicalDeviceMemoryProperties& memory);

}

#endif
