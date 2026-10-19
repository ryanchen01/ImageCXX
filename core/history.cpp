#include "core/history.h"

namespace imagecxx {

ImageHistory::ImageHistory(std::size_t byteBudget) : byteBudget_(byteBudget) {}

void ImageHistory::push(const Image &before, const QString &label) {
    redo_.clear();
    if (!before.empty()) undo_.push_back({label, before});
    trim();
}

std::optional<Image> ImageHistory::undo(const Image &current) {
    if (undo_.empty()) return {};
    auto entry = std::move(undo_.back());
    undo_.pop_back();
    redo_.push_back({entry.label, current});
    trim();
    return std::move(entry.image);
}

std::optional<Image> ImageHistory::redo(const Image &current) {
    if (redo_.empty()) return {};
    auto entry = std::move(redo_.back());
    redo_.pop_back();
    undo_.push_back({entry.label, current});
    trim();
    return std::move(entry.image);
}

QStringList ImageHistory::labels() const {
    QStringList result;
    for (const auto &entry : undo_) result.push_back(entry.label);
    return result;
}

void ImageHistory::clear() { undo_.clear(); redo_.clear(); }

std::size_t ImageHistory::retainedBytes() const {
    std::size_t bytes = 0;
    for (const auto &entry : undo_) bytes += entry.image.byteCount();
    for (const auto &entry : redo_) bytes += entry.image.byteCount();
    return bytes;
}

void ImageHistory::setByteBudget(std::size_t bytes) { byteBudget_ = bytes; trim(); }

void ImageHistory::trim() {
    auto bytes = retainedBytes();
    while (bytes > byteBudget_) {
        auto &entries = !undo_.empty() ? undo_ : redo_;
        if (entries.empty()) break;
        bytes -= entries.front().image.byteCount();
        entries.pop_front();
    }
}

} // namespace imagecxx
