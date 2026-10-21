#pragma once

#include <cstddef>
#include <optional>

namespace imagecxx::detail {
// Estimated available physical memory, bounded by visible process/container limits.
std::optional<std::size_t> availableMemoryBytes();
// Keep 10% available for the OS, the UI, and changes since the snapshot.
// The required amount includes output and working memory, not just the pixel buffer.
void checkMemoryBudget(std::size_t required, std::optional<std::size_t> available);
} // namespace imagecxx::detail
