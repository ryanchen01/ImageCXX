#pragma once

#include "core/image.h"
#include <deque>
#include <optional>

namespace imagecxx {

struct HistoryEntry {
    QString label;
    Image image;
};

// Snapshots share immutable buffers. Each retained entry counts against the budget
// conservatively, including a shared buffer, so retained pixel memory stays bounded.
class ImageHistory {
public:
    explicit ImageHistory(std::size_t byteBudget = 256 * 1024 * 1024);
    void push(const Image &before, const QString &label);
    std::optional<Image> undo(const Image &current);
    std::optional<Image> redo(const Image &current);
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    QStringList labels() const;
    void clear();
    std::size_t retainedBytes() const;
    std::size_t byteBudget() const { return byteBudget_; }
    void setByteBudget(std::size_t bytes);

private:
    void trim();
    std::size_t byteBudget_;
    std::deque<HistoryEntry> undo_;
    std::deque<HistoryEntry> redo_;
};

} // namespace imagecxx
