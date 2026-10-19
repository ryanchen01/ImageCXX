#pragma once

#include <QMap>
#include <QString>
#include <QStringList>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace imagecxx {

enum class PixelType { UInt8, UInt16, Int16, Float32, Float64, RGB8, RGBA8 };
std::size_t bytesPerPixel(PixelType type);
QString pixelTypeName(PixelType type);
PixelType pixelTypeFromName(const QString &name);
std::size_t componentCount(PixelType type);

struct Axis {
    QString name;
    double spacing = 1.0;
    double origin = 0.0;
    QString unit = QStringLiteral("px");
};

struct Channel {
    QString name;
    std::uint32_t color = 0xffffffff;
};

struct ImageMetadata {
    QString name;
    QString sourcePath;
    std::vector<Axis> axes;
    // Row-major spatial direction matrix. Empty means identity. A 2D view may
    // retain its 3D plane orientation, with its complete world origin stored as
    // a JSON triple in attributes["ImageCXX.EmbeddedPlaneOrigin"].
    std::vector<double> direction;
    std::vector<Channel> channels;
    double intensityScale = 1.0;
    double intensityOffset = 0.0;
    QString intensityUnit;
    QMap<QString, QString> attributes;
};

// Storage owns bytes independently from image views. Providers may load regions lazily.
class Storage {
public:
    virtual ~Storage() = default;
    virtual std::size_t size() const = 0;
    virtual const std::byte *data() const = 0;
    virtual std::byte *mutableData() = 0;
    virtual bool readOnly() const = 0;
    virtual QString kind() const = 0;
    virtual void read(std::size_t offset, std::span<std::byte> destination) const;
};

// X is the fastest axis. Copies share storage; mutation detaches snapshots and views.
class Image {
public:
    Image() = default;
    static Image allocate(PixelType type, std::vector<std::size_t> shape,
                          ImageMetadata metadata = {});
    static Image map(const QString &path, PixelType type, std::vector<std::size_t> shape,
                     std::size_t fileOffset = 0, ImageMetadata metadata = {});
    static Image fromStorage(PixelType type, std::vector<std::size_t> shape,
                             std::shared_ptr<Storage> storage, ImageMetadata metadata = {});

    bool empty() const { return !storage_; }
    PixelType pixelType() const { return type_; }
    const std::vector<std::size_t> &shape() const { return shape_; }
    const std::vector<std::size_t> &strides() const { return strides_; }
    std::size_t dimension(std::size_t axis) const;
    std::size_t width() const { return dimension(0); }
    std::size_t height() const { return dimension(1); }
    std::size_t depth() const { return dimension(2); }
    std::size_t channels() const { return dimension(3); }
    std::size_t frames() const { return dimension(4); }
    std::size_t pixelCount() const;
    std::size_t byteCount() const;
    bool contiguous() const;
    const ImageMetadata &metadata() const { return metadata_; }
    ImageMetadata &metadata() { return metadata_; }
    const std::byte *data() const;
    std::byte *mutableData();
    QString storageKind() const;
    double value(std::span<const std::size_t> index, std::size_t component = 0) const;
    void setValue(std::span<const std::size_t> index, double value, std::size_t component = 0);
    double sample(std::size_t x, std::size_t y, std::size_t z = 0,
                  std::size_t channel = 0, std::size_t time = 0,
                  std::size_t component = 0) const;
    Image slice(std::size_t z = 0, std::size_t channel = 0, std::size_t time = 0) const;
    Image region(std::span<const std::size_t> start, std::span<const std::size_t> extent) const;
    Image clone() const;
    Image materialized() const;

private:
    std::size_t byteOffset(std::span<const std::size_t> index) const;
    PixelType type_ = PixelType::UInt8;
    std::vector<std::size_t> shape_;
    std::vector<std::size_t> strides_;
    std::size_t offset_ = 0;
    std::shared_ptr<Storage> storage_;
    ImageMetadata metadata_;
};

} // namespace imagecxx
