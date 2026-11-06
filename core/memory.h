#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <mutex>
#include <stop_token>
#include "core/image.h"

namespace imagecxx::detail {
// Estimated available physical memory, bounded by visible process/container limits.
std::optional<std::size_t> availableMemoryBytes();
std::size_t checkedImageBytes(PixelType type, std::span<const std::size_t> shape);
std::unique_lock<std::recursive_timed_mutex> lockMemoryAllocation(std::stop_token stop);
// Keep 10% available for the OS, the UI, and changes since the snapshot.
// The required amount includes output and working memory, not just the pixel buffer.
void checkMemoryBudget(std::size_t required, std::optional<std::size_t> available);
} // namespace imagecxx::detail
