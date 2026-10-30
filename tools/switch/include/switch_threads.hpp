#pragma once

#include <cstdint>
#include <span>

namespace kz::switch_platform {

/**
 * Returns the Horizon ID of the calling thread, or zero if the query fails.
 */
std::uint32_t CurrentThreadId();

/**
 * Restricts the calling thread to the requested CPU cores when permitted by its process.
 */
bool SetCpuAffinity(std::span<const std::uint16_t> cpu_ids);

} // namespace kz::switch_platform
