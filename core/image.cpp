#include "core/image.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace imagecxx {
namespace {

class RamStorage final : public Storage {
public:
    explicit RamStorage(std::size_t bytes) : bytes_(bytes) {}
    std::size_t size() const override { return bytes_.size(); }
    const std::byte *data() const override { return bytes_.data(); }
    std::byte *mutableData() override { return bytes_.data(); }
    bool readOnly() const override { return false; }
    QString kind() const override { return QStringLiteral("RAM"); }
private:
    std::vector<std::byte> bytes_;
};

class MappedStorage final : public Storage {
public:
    MappedStorage(const QString &path, std::size_t offset, std::size_t bytes)
        : file_(path), bytes_(bytes) {
        if (offset > static_cast<std::size_t>(std::numeric_limits<qint64>::max())
            || bytes > static_cast<std::size_t>(std::numeric_limits<qint64>::max()) - offset)
            throw std::overflow_error("Mapped region exceeds file address range");
        if (!file_.open(QIODevice::ReadOnly))
            throw std::runtime_error(file_.errorString().toStdString());
        if (static_cast<quint64>(file_.size()) < offset + bytes)
            throw std::invalid_argument("File is smaller than the requested image");
        mapped_ = file_.map(static_cast<qint64>(offset), static_cast<qint64>(bytes));
        if (!mapped_)
            throw std::runtime_error(file_.errorString().toStdString());
    }
    ~MappedStorage() override { if (mapped_) file_.unmap(mapped_); }
    std::size_t size() const override { return bytes_; }
    const std::byte *data() const override { return reinterpret_cast<const std::byte *>(mapped_); }
    std::byte *mutableData() override { return nullptr; }
    bool readOnly() const override { return true; }
    QString kind() const override { return QStringLiteral("Memory mapped"); }
private:
    QFile file_;
    uchar *mapped_ = nullptr;
    std::size_t bytes_;
};

std::size_t checkedProduct(std::span<const std::size_t> values, std::size_t initial = 1) {
    for (auto value : values) {
        if (value == 0) throw std::invalid_argument("Image axes cannot have zero length");
        if (initial > std::numeric_limits<std::size_t>::max() / value)
            throw std::overflow_error("Image dimensions exceed the addressable size");
        initial *= value;
    }
    return initial;
}

template <typename T> double readNumber(const std::byte *source) {
    T value;
    std::memcpy(&value, source, sizeof(T));
    return static_cast<double>(value);
}

template <typename T> void writeNumber(std::byte *destination, double value) {
    if constexpr (std::is_integral_v<T>) {
        value = std::isnan(value) ? 0.0 : std::clamp(value,
            static_cast<double>(std::numeric_limits<T>::lowest()),
            static_cast<double>(std::numeric_limits<T>::max()));
        value = std::round(value);
    }
    const auto converted = static_cast<T>(value);
    std::memcpy(destination, &converted, sizeof(T));
}

} // namespace

std::size_t bytesPerPixel(PixelType type) {
    switch (type) {
    case PixelType::UInt8: return 1;
    case PixelType::UInt16: case PixelType::Int16: return 2;
    case PixelType::Float32: case PixelType::RGBA8: return 4;
    case PixelType::Float64: return 8;
    case PixelType::RGB8: return 3;
    }
    throw std::invalid_argument("Unknown pixel type");
}

QString pixelTypeName(PixelType type) {
    switch (type) {
    case PixelType::UInt8: return QStringLiteral("uint8");
    case PixelType::UInt16: return QStringLiteral("uint16");
    case PixelType::Int16: return QStringLiteral("int16");
    case PixelType::Float32: return QStringLiteral("float32");
    case PixelType::Float64: return QStringLiteral("float64");
    case PixelType::RGB8: return QStringLiteral("rgb8");
    case PixelType::RGBA8: return QStringLiteral("rgba8");
    }
    throw std::invalid_argument("Unknown pixel type");
}

PixelType pixelTypeFromName(const QString &name) {
    for (auto type : {PixelType::UInt8, PixelType::UInt16, PixelType::Int16,
                      PixelType::Float32, PixelType::Float64, PixelType::RGB8, PixelType::RGBA8})
        if (pixelTypeName(type).compare(name, Qt::CaseInsensitive) == 0) return type;
    throw std::invalid_argument("Unsupported pixel type: " + name.toStdString());
}

std::size_t componentCount(PixelType type) {
    return type == PixelType::RGB8 ? 3 : type == PixelType::RGBA8 ? 4 : 1;
}

void Storage::read(std::size_t offset, std::span<std::byte> destination) const {
    if (offset > size() || destination.size() > size() - offset)
        throw std::out_of_range("Storage region is outside the buffer");
    if (!data()) throw std::runtime_error("Storage provider must implement region reads");
    std::memcpy(destination.data(), data() + offset, destination.size());
}

Image Image::allocate(PixelType type, std::vector<std::size_t> shape, ImageMetadata metadata) {
    const auto bytes = checkedProduct(shape, bytesPerPixel(type));
    return fromStorage(type, std::move(shape), std::make_shared<RamStorage>(bytes), std::move(metadata));
}

Image Image::map(const QString &path, PixelType type, std::vector<std::size_t> shape,
                 std::size_t fileOffset, ImageMetadata metadata) {
    const auto bytes = checkedProduct(shape, bytesPerPixel(type));
    return fromStorage(type, std::move(shape),
                       std::make_shared<MappedStorage>(path, fileOffset, bytes), std::move(metadata));
}

Image Image::fromStorage(PixelType type, std::vector<std::size_t> shape,
                        std::shared_ptr<Storage> storage, ImageMetadata metadata) {
    if (shape.size() < 2) throw std::invalid_argument("Images need at least X and Y axes");
    const auto bytes = checkedProduct(shape, bytesPerPixel(type));
    if (!storage || storage->size() < bytes) throw std::invalid_argument("Image storage is too small");
    if (metadata.axes.size() > shape.size()) throw std::invalid_argument("Metadata has too many axes");
    const QStringList names{QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z"),
                            QStringLiteral("C"), QStringLiteral("T")};
    while (metadata.axes.size() < shape.size()) {
        const auto axis = metadata.axes.size();
        metadata.axes.push_back({axis < 5 ? names[static_cast<int>(axis)]
                                        : QStringLiteral("Axis%1").arg(axis)});
    }
    for (const auto &axis : metadata.axes) {
        if (!std::isfinite(axis.spacing) || axis.spacing <= 0 || !std::isfinite(axis.origin))
            throw std::invalid_argument("Axis spacing must be positive and origin must be finite");
    }
    if (!metadata.direction.empty()) {
        const auto dimension = static_cast<std::size_t>(std::sqrt(metadata.direction.size()));
        if (dimension < 2 || dimension * dimension != metadata.direction.size()
            || !std::all_of(metadata.direction.begin(), metadata.direction.end(), [](double value) { return std::isfinite(value); }))
            throw std::invalid_argument("Image direction must be a finite square matrix");
    }
    if (!std::isfinite(metadata.intensityScale) || !std::isfinite(metadata.intensityOffset))
        throw std::invalid_argument("Intensity calibration must be finite");
    Image image;
    image.type_ = type;
    image.shape_ = std::move(shape);
    image.storage_ = std::move(storage);
    image.metadata_ = std::move(metadata);
    std::size_t stride = bytesPerPixel(type);
    for (auto length : image.shape_) {
        image.strides_.push_back(stride);
        stride *= length;
    }
    return image;
}

std::size_t Image::dimension(std::size_t axis) const {
    return empty() ? 0 : axis < shape_.size() ? shape_[axis] : 1;
}

std::size_t Image::pixelCount() const { return empty() ? 0 : checkedProduct(shape_); }
std::size_t Image::byteCount() const { return pixelCount() * bytesPerPixel(type_); }

bool Image::contiguous() const {
    std::size_t stride = bytesPerPixel(type_);
    for (std::size_t axis = 0; axis < shape_.size(); ++axis) {
        if (shape_[axis] > 1 && strides_[axis] != stride) return false;
        stride *= shape_[axis];
    }
    return !empty();
}

const std::byte *Image::data() const {
    if (empty()) return nullptr;
    const auto *buffer = storage_->data();
    return buffer ? buffer + offset_ : nullptr;
}

std::byte *Image::mutableData() {
    if (empty()) throw std::logic_error("Cannot edit an empty image");
    if (storage_.use_count() > 1 || storage_->readOnly() || offset_ != 0 || !contiguous())
        *this = clone();
    auto *buffer = storage_->mutableData();
    if (!buffer) { *this = clone(); buffer = storage_->mutableData(); }
    return buffer;
}

QString Image::storageKind() const { return empty() ? QString() : storage_->kind(); }

std::size_t Image::byteOffset(std::span<const std::size_t> index) const {
    if (empty() || index.size() != shape_.size()) throw std::invalid_argument("Index dimensionality mismatch");
    auto offset = offset_;
    for (std::size_t axis = 0; axis < index.size(); ++axis) {
        if (index[axis] >= shape_[axis]) throw std::out_of_range("Pixel is outside the image");
        offset += index[axis] * strides_[axis];
    }
    return offset;
}

double Image::value(std::span<const std::size_t> index, std::size_t component) const {
    if (component >= componentCount(type_)) throw std::out_of_range("Invalid pixel component");
    const auto offset = byteOffset(index) + (componentCount(type_) > 1 ? component : 0);
    std::byte bytes[8];
    storage_->read(offset, std::span(bytes, bytesPerPixel(type_) / componentCount(type_)));
    switch (type_) {
    case PixelType::UInt8: case PixelType::RGB8: case PixelType::RGBA8: return readNumber<std::uint8_t>(bytes);
    case PixelType::UInt16: return readNumber<std::uint16_t>(bytes);
    case PixelType::Int16: return readNumber<std::int16_t>(bytes);
    case PixelType::Float32: return readNumber<float>(bytes);
    case PixelType::Float64: return readNumber<double>(bytes);
    }
    throw std::invalid_argument("Unknown pixel type");
}

void Image::setValue(std::span<const std::size_t> index, double number, std::size_t component) {
    if (component >= componentCount(type_)) throw std::out_of_range("Invalid pixel component");
    // Validate before detaching, then recompute the offset after strides change.
    (void)byteOffset(index);
    auto *buffer = mutableData();
    auto *destination = buffer + byteOffset(index) + (componentCount(type_) > 1 ? component : 0);
    switch (type_) {
    case PixelType::UInt8: case PixelType::RGB8: case PixelType::RGBA8: writeNumber<std::uint8_t>(destination, number); break;
    case PixelType::UInt16: writeNumber<std::uint16_t>(destination, number); break;
    case PixelType::Int16: writeNumber<std::int16_t>(destination, number); break;
    case PixelType::Float32: writeNumber<float>(destination, number); break;
    case PixelType::Float64: writeNumber<double>(destination, number); break;
    }
}

double Image::sample(std::size_t x, std::size_t y, std::size_t z, std::size_t channel,
                     std::size_t time, std::size_t component) const {
    const std::array<std::size_t, 5> coordinates{x, y, z, channel, time};
    if (shape_.size() <= coordinates.size()) {
        for (std::size_t axis = shape_.size(); axis < coordinates.size(); ++axis)
            if (coordinates[axis] != 0) throw std::out_of_range("Axis is outside the image");
        return value(std::span(coordinates.data(), shape_.size()), component);
    }
    std::vector<std::size_t> index(shape_.size(), 0);
    for (std::size_t axis = 0; axis < 5; ++axis) {
        if (axis < index.size()) index[axis] = coordinates[axis];
        else if (coordinates[axis] != 0) throw std::out_of_range("Axis is outside the image");
    }
    return value(index, component);
}

Image Image::slice(std::size_t z, std::size_t channel, std::size_t time) const {
    if (empty()) throw std::invalid_argument("Cannot slice an empty image");
    std::vector<std::size_t> start(shape_.size(), 0), extent(shape_.size(), 1);
    extent[0] = width(); extent[1] = height();
    const std::size_t coordinates[]{z, channel, time};
    for (std::size_t axis = 2; axis < 5; ++axis) {
        if (axis < shape_.size()) start[axis] = coordinates[axis - 2];
        else if (coordinates[axis - 2] != 0) throw std::out_of_range("Slice is outside the image");
    }
    Image result = region(start, extent);
    if (result.metadata_.axes.size() >= 3 && result.metadata_.direction.empty())
        result.metadata_.direction = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    if (result.metadata_.direction.size() == 9 && result.metadata_.axes.size() >= 3) {
        const QJsonArray origin{result.metadata_.axes[0].origin, result.metadata_.axes[1].origin,
                                result.metadata_.axes[2].origin};
        result.metadata_.attributes.insert(QStringLiteral("ImageCXX.EmbeddedPlaneOrigin"),
            QString::fromUtf8(QJsonDocument(origin).toJson(QJsonDocument::Compact)));
        result.metadata_.attributes.insert(QStringLiteral("ImageCXX.EmbeddedPlaneZUnit"),
            result.metadata_.axes[2].unit);
    }
    result.shape_.resize(2);
    result.strides_.resize(2);
    result.metadata_.axes.resize(2);
    // Retain the 3D orientation in metadata for viewers and spatial I/O.
    return result;
}

Image Image::region(std::span<const std::size_t> start, std::span<const std::size_t> extent) const {
    if (start.size() != shape_.size() || extent.size() != shape_.size())
        throw std::invalid_argument("Region dimensionality mismatch");
    Image result = *this;
    result.offset_ = byteOffset(start);
    for (std::size_t axis = 0; axis < shape_.size(); ++axis) {
        if (extent[axis] == 0 || extent[axis] > shape_[axis] - start[axis])
            throw std::out_of_range("Region is outside the image");
        result.shape_[axis] = extent[axis];
        result.metadata_.axes[axis].origin += static_cast<double>(start[axis]) * metadata_.axes[axis].spacing;
    }
    const auto directionDimension = static_cast<std::size_t>(std::sqrt(metadata_.direction.size()));
    if (directionDimension * directionDimension == metadata_.direction.size() && directionDimension > 0) {
        for (std::size_t row = 0; row < std::min(directionDimension, shape_.size()); ++row) {
            double shift = 0;
            for (std::size_t col = 0; col < std::min(directionDimension, shape_.size()); ++col)
                shift += metadata_.direction[row * directionDimension + col]
                         * static_cast<double>(start[col]) * metadata_.axes[col].spacing;
            result.metadata_.axes[row].origin = metadata_.axes[row].origin + shift;
        }
        if (directionDimension == 3 && shape_.size() == 2
            && metadata_.attributes.contains(QStringLiteral("ImageCXX.EmbeddedPlaneOrigin"))) {
            const auto embedded = QJsonDocument::fromJson(metadata_.attributes.value(
                QStringLiteral("ImageCXX.EmbeddedPlaneOrigin")).toUtf8()).array();
            if (embedded.size() != 3 || !embedded[2].isDouble() || !std::isfinite(embedded[2].toDouble()))
                throw std::invalid_argument("Embedded image plane origin is invalid");
            double thirdOrigin = embedded[2].toDouble();
            for (std::size_t col = 0; col < 2; ++col)
                thirdOrigin += metadata_.direction[6 + col] * static_cast<double>(start[col])
                               * metadata_.axes[col].spacing;
            const QJsonArray origin{result.metadata_.axes[0].origin, result.metadata_.axes[1].origin, thirdOrigin};
            result.metadata_.attributes.insert(QStringLiteral("ImageCXX.EmbeddedPlaneOrigin"),
                QString::fromUtf8(QJsonDocument(origin).toJson(QJsonDocument::Compact)));
        }
    }
    return result;
}

Image Image::clone() const {
    if (empty()) return {};
    auto result = allocate(type_, shape_, metadata_);
    auto *destination = result.mutableData();
    if (contiguous()) storage_->read(offset_, std::span(destination, byteCount()));
    else {
        std::vector<std::size_t> index(shape_.size(), 0);
        for (std::size_t pixel = 0; pixel < pixelCount(); ++pixel) {
            storage_->read(byteOffset(index), std::span(destination + pixel * bytesPerPixel(type_), bytesPerPixel(type_)));
            for (std::size_t axis = 0; axis < index.size(); ++axis) {
                if (++index[axis] < shape_[axis]) break;
                index[axis] = 0;
            }
        }
    }
    return result;
}

Image Image::materialized() const {
    return contiguous() && data() ? *this : clone();
}

} // namespace imagecxx
