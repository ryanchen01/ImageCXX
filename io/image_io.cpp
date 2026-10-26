#include "io/image_io.h"
#include "io/io_detail.h"

#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QSysInfo>
#include <QDir>
#include <QTemporaryDir>
#include <tiffio.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>

namespace imagecxx {
namespace detail {

double spatialUnitToMillimeters(const QString &unit)
{
    const auto name = unit.trimmed().toLower();
    if (name == "m") return 1000;
    if (name == "cm") return 10;
    if (name == "mm") return 1;
    if (name == "um" || name == QString::fromUtf8("µm") || name == QString::fromUtf8("μm")) return .001;
    if (name == "nm") return .000001;
    throw std::runtime_error("Export requires calibrated spatial units: m, cm, mm, um or nm");
}

void checkIo(std::stop_token stop, const IoProgress &progress, double fraction)
{
    if (stop.stop_requested()) throw std::runtime_error("Image I/O cancelled");
    if (progress) progress(fraction);
}

void atomicCopy(const QString &source, const QString &destination,
                std::stop_token stop, const IoProgress &progress)
{
    QFile input(source);
    QSaveFile output(destination);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly))
        throw std::runtime_error("Cannot open image output");
    QByteArray buffer(4 * 1024 * 1024, Qt::Uninitialized);
    while (!input.atEnd()) {
        checkIo(stop, progress, input.size() > 0 ? static_cast<double>(input.pos()) / input.size() : 0);
        const auto count = input.read(buffer.data(), buffer.size());
        if (count < 0 || output.write(buffer.constData(), count) != count)
            throw std::runtime_error("Cannot write image output");
    }
    checkIo(stop, {}, 0);
    if (!output.commit())
        throw std::runtime_error(QString("Cannot commit image output '%1': %2").arg(destination, output.errorString()).toStdString());
    if (progress) progress(1);
}

QJsonObject imageDescription(const Image &image)
{
    const auto &metadata = image.metadata();
    if (!std::isfinite(metadata.intensityScale) || !std::isfinite(metadata.intensityOffset))
        throw std::runtime_error("Cannot preserve non-finite intensity metadata");
    for (const auto &axis : metadata.axes)
        if (!std::isfinite(axis.spacing) || !std::isfinite(axis.origin))
            throw std::runtime_error("Cannot preserve non-finite axis metadata");
    for (double value : metadata.direction)
        if (!std::isfinite(value)) throw std::runtime_error("Cannot preserve non-finite direction metadata");
    QJsonArray shape, axes, channels, direction;
    for (auto size : image.shape()) shape.append(static_cast<qint64>(size));
    for (const auto &axis : metadata.axes)
        axes.append(QJsonObject{{"name", axis.name}, {"spacing", axis.spacing},
                                {"origin", axis.origin}, {"unit", axis.unit}});
    for (const auto &channel : metadata.channels)
        channels.append(QJsonObject{{"name", channel.name}, {"color", static_cast<qint64>(channel.color)}});
    for (double value : metadata.direction) direction.append(value);
    QJsonObject attributes;
    for (auto it = metadata.attributes.cbegin(); it != metadata.attributes.cend(); ++it)
        attributes.insert(it.key(), it.value());
    return {{"version", 1}, {"pixelType", pixelTypeName(image.pixelType())}, {"shape", shape},
            {"name", metadata.name}, {"axes", axes}, {"channels", channels},
            {"direction", direction}, {"intensityScale", metadata.intensityScale},
            {"intensityOffset", metadata.intensityOffset}, {"intensityUnit", metadata.intensityUnit},
            {"attributes", attributes}, {"dicomSequences", metadata.dicomSequences},
            {"originalMetadata", metadata.originalMetadata}};
}

QJsonObject parseImageDescription(const QByteArray &bytes)
{
    const auto document = QJsonDocument::fromJson(bytes);
    const auto object = document.object();
    if (!document.isObject() || object.value("version").toInt() != 1 ||
        !object.value("attributes").isObject() || !object.value("shape").isArray())
        throw std::runtime_error("Invalid or unsupported ImageCXX metadata");
    return object;
}

QMap<QString, QString> imageDescriptionText(const Image &image)
{
    const auto encoded = QJsonDocument(imageDescription(image)).toJson(QJsonDocument::Compact).toBase64();
    // Short ASCII records also fit MetaImage's header-line limit and JPEG COM.
    constexpr qsizetype chunkSize = 3000;
    const auto count = (encoded.size() + chunkSize - 1) / chunkSize;
    QMap<QString, QString> result{{"ImageCXX", QString("base64-chunks:%1").arg(count)}};
    for (qsizetype index = 0; index < count; ++index)
        result.insert(QString("ImageCXX.%1").arg(index, 8, 10, QLatin1Char('0')),
            QString::fromLatin1(encoded.mid(index * chunkSize, chunkSize)));
    return result;
}

QJsonObject parseImageDescriptionText(const QMap<QString, QString> &values)
{
    const auto text = values.value("ImageCXX");
    if (text.isEmpty()) return {};
    if (!text.startsWith("base64-chunks:")) return parseImageDescription(text.toUtf8());
    bool ok = false;
    const auto count = text.mid(14).toInt(&ok);
    if (!ok || count <= 0 || count > values.size())
        throw std::runtime_error("Invalid ImageCXX metadata chunks");
    QByteArray bytes;
    for (int index = 0; index < count; ++index) {
        const auto chunk = values.value(QString("ImageCXX.%1").arg(index, 8, 10, QLatin1Char('0')));
        if (chunk.isEmpty()) throw std::runtime_error("Missing ImageCXX metadata chunk");
        bytes.append(chunk.toLatin1());
    }
    const auto decoded = QByteArray::fromBase64Encoding(bytes, QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded) throw std::runtime_error("Invalid ImageCXX metadata encoding");
    return parseImageDescription(decoded.decoded);
}

QJsonObject rasterImageDescription(const QImage &image)
{
    QMap<QString, QString> values;
    for (const auto &key : image.textKeys()) values.insert(key, image.text(key));
    return parseImageDescriptionText(values);
}

ImageMetadata descriptionMetadata(const QJsonObject &object)
{
    ImageMetadata metadata;
    metadata.name = object.value("name").toString();
    for (const auto value : object.value("axes").toArray()) {
        const auto axis = value.toObject();
        metadata.axes.push_back({axis.value("name").toString(), axis.value("spacing").toDouble(1),
                                 axis.value("origin").toDouble(), axis.value("unit").toString("px")});
    }
    for (const auto value : object.value("channels").toArray()) {
        const auto channel = value.toObject();
        metadata.channels.push_back({channel.value("name").toString(),
                                     static_cast<std::uint32_t>(channel.value("color").toInteger(0xffffffff))});
    }
    for (const auto value : object.value("direction").toArray()) metadata.direction.push_back(value.toDouble());
    metadata.intensityScale = object.value("intensityScale").toDouble(1);
    metadata.intensityOffset = object.value("intensityOffset").toDouble();
    metadata.intensityUnit = object.value("intensityUnit").toString();
    const auto attributes = object.value("attributes").toObject();
    for (auto it = attributes.begin(); it != attributes.end(); ++it)
        metadata.attributes.insert(it.key(), it.value().toString());
    metadata.dicomSequences = object.value("dicomSequences").toObject();
    metadata.originalMetadata = object.value("originalMetadata").toObject();
    return metadata;
}
} // namespace detail

QString imageFileFilter()
{
    return QStringLiteral("Images (*.png *.jpg *.jpeg *.tif *.tiff *.mhd *.mha *.nii *.nii.gz *.nrrd *.dcm *.dicom *.raw *.bin);;All files (*)");
}

QString imageSaveFilter()
{
    return QStringLiteral("TIFF (*.tif);;PNG (*.png);;JPEG (*.jpg);;MetaImage (*.mha *.mhd);;NIfTI (*.nii *.nii.gz);;NRRD (*.nrrd);;DICOM (*.dcm);;Raw binary (*.raw)");
}

RawImportOptions rawImportOptionsFromJson(const QJsonObject &object)
{
    RawImportOptions options;
    options.pixelType = pixelTypeFromName(object.value("pixelType").toString());
    const auto dimensions = object.value("shape").toArray();
    if (dimensions.size() < 2 || dimensions.size() > 5) throw std::runtime_error("Invalid raw dimensions");
    std::size_t bytes = bytesPerPixel(options.pixelType);
    for (const auto value : dimensions) {
        const auto size = value.toInteger();
        if (size <= 0 || static_cast<quint64>(size) > std::numeric_limits<std::size_t>::max() / bytes)
            throw std::runtime_error("Invalid raw dimensions");
        bytes *= static_cast<std::size_t>(size);
        options.shape.push_back(static_cast<std::size_t>(size));
    }
    const auto offset = object.value("headerBytes").toInteger(0);
    if (offset < 0 || static_cast<quint64>(offset) > std::numeric_limits<std::size_t>::max() - bytes)
        throw std::runtime_error("Invalid raw header offset");
    options.headerBytes = static_cast<std::size_t>(offset);
    const auto order = object.value("byteOrder").toString();
    if (order == "native") options.byteOrder = ByteOrder::Native;
    else if (order == "little") options.byteOrder = ByteOrder::LittleEndian;
    else if (order == "big") options.byteOrder = ByteOrder::BigEndian;
    else throw std::runtime_error("Invalid raw byte order");
    options.memoryMap = object.value("memoryMap").toBool(true);
    options.metadata = detail::descriptionMetadata(object);
    return options;
}

Image readImage(const QString &path, const std::optional<RawImportOptions> &raw,
                std::stop_token stop, IoProgress progress)
{
    detail::checkIo(stop, progress, 0);
    const QFileInfo info(path);
    const QString suffix = info.suffix().toLower();
    Image image;
    if (raw || suffix == "raw" || suffix == "bin") {
        RawImportOptions options;
        if (raw) options = *raw;
        else {
            QFile sidecar(path + ".json");
            if (!sidecar.open(QIODevice::ReadOnly))
                throw std::runtime_error("Raw import requires dimensions and a pixel type");
            options = rawImportOptionsFromJson(QJsonDocument::fromJson(sidecar.readAll()).object());
        }
        if (options.shape.size() < 2 || options.shape.size() > 5)
            throw std::runtime_error("Raw images need between two and five dimensions");
        const bool swap = bytesPerPixel(options.pixelType) > 1 && componentCount(options.pixelType) == 1 &&
            ((options.byteOrder == ByteOrder::LittleEndian && std::endian::native == std::endian::big) ||
             (options.byteOrder == ByteOrder::BigEndian && std::endian::native == std::endian::little));
        if (options.memoryMap && !swap) image = Image::map(path, options.pixelType, options.shape, options.headerBytes, options.metadata);
        else {
            image = Image::allocate(options.pixelType, options.shape, options.metadata);
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly) || options.headerBytes > static_cast<std::size_t>(file.size()) ||
                !file.seek(static_cast<qint64>(options.headerBytes)))
                throw std::runtime_error("Cannot open raw image");
            auto *destination = reinterpret_cast<char *>(image.mutableData());
            std::size_t remaining = image.byteCount();
            while (remaining) {
                detail::checkIo(stop, progress, 1.0 - static_cast<double>(remaining) / image.byteCount());
                const auto count = std::min<std::size_t>(remaining, 4 * 1024 * 1024);
                if (file.read(destination, static_cast<qint64>(count)) != static_cast<qint64>(count))
                    throw std::runtime_error("Raw image is truncated");
                destination += count;
                remaining -= count;
            }
            if (swap) {
                auto *bytes = image.mutableData();
                const auto size = bytesPerPixel(image.pixelType());
                for (std::size_t offset = 0; offset < image.byteCount(); offset += size)
                    std::reverse(bytes + offset, bytes + offset + size);
            }
        }
    } else if (suffix == "mhd" || suffix == "mha" || suffix == "nii" || suffix == "gz" || suffix == "nrrd" || suffix == "nhdr") {
        image = readScientific(path, stop, progress);
    } else if (suffix == "dcm" || suffix == "dicom" || suffix.isEmpty()) {
        image = readDicom(path, stop, progress);
    } else if (suffix == "tif" || suffix == "tiff") {
        std::unique_ptr<TIFF, decltype(&TIFFClose)> tiff(TIFFOpen(QFile::encodeName(path).constData(), "r"), TIFFClose);
        if (!tiff) throw std::runtime_error("Cannot open TIFF");
        uint32_t width = 0, height = 0;
        uint16_t bits = 0, samples = 1, format = SAMPLEFORMAT_UINT, planar = PLANARCONFIG_CONTIG, photometric = PHOTOMETRIC_MINISBLACK;
        TIFFGetField(tiff.get(), TIFFTAG_IMAGEWIDTH, &width);
        TIFFGetField(tiff.get(), TIFFTAG_IMAGELENGTH, &height);
        TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_BITSPERSAMPLE, &bits);
        TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLESPERPIXEL, &samples);
        TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLEFORMAT, &format);
        TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_PLANARCONFIG, &planar);
        TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_PHOTOMETRIC, &photometric);
        PixelType type;
        if (samples == 1 && format == SAMPLEFORMAT_UINT && bits == 8) type = PixelType::UInt8;
        else if (samples == 1 && format == SAMPLEFORMAT_UINT && bits == 16) type = PixelType::UInt16;
        else if (samples == 1 && format == SAMPLEFORMAT_INT && bits == 16) type = PixelType::Int16;
        else if (samples == 1 && format == SAMPLEFORMAT_IEEEFP && bits == 32) type = PixelType::Float32;
        else if (samples == 1 && format == SAMPLEFORMAT_IEEEFP && bits == 64) type = PixelType::Float64;
        else if (bits == 8 && format == SAMPLEFORMAT_UINT && samples == 3 && photometric == PHOTOMETRIC_RGB) type = PixelType::RGB8;
        else if (bits == 8 && format == SAMPLEFORMAT_UINT && samples == 4 && photometric == PHOTOMETRIC_RGB) type = PixelType::RGBA8;
        else throw std::runtime_error("Unsupported TIFF pixel type");
        if (planar != PLANARCONFIG_CONTIG || (samples == 1
            && photometric != PHOTOMETRIC_MINISBLACK && photometric != PHOTOMETRIC_MINISWHITE))
            throw std::runtime_error("TIFF planar or photometric encoding is unsupported");
        std::size_t pages = 0;
        do { ++pages; } while (TIFFReadDirectory(tiff.get()));
        TIFFSetDirectory(tiff.get(), 0);
        std::vector<std::size_t> shape{width, height};
        if (pages > 1) shape.push_back(pages);
        ImageMetadata metadata;
        char *description = nullptr;
        TIFFGetField(tiff.get(), TIFFTAG_IMAGEDESCRIPTION, &description);
        if (description) {
            const QByteArray text(description);
            if (text.startsWith("ImageCXX\n")) {
                const auto object = detail::parseImageDescription(text.mid(QByteArrayLiteral("ImageCXX\n").size()));
                std::vector<std::size_t> savedShape;
                std::size_t savedPages = 1;
                const auto dimensions = object.value("shape").toArray();
                if (dimensions.size() < 2 || dimensions.size() > 5) throw std::runtime_error("Invalid TIFF dimensions");
                for (const auto value : dimensions) {
                    const auto size = value.toInteger();
                    if (size <= 0 || static_cast<quint64>(size) > std::numeric_limits<std::size_t>::max() / savedPages)
                        throw std::runtime_error("Invalid TIFF dimensions");
                    savedShape.push_back(static_cast<std::size_t>(size));
                    if (savedShape.size() > 2) savedPages *= static_cast<std::size_t>(size);
                }
                if (savedShape[0] != width || savedShape[1] != height || savedPages != pages)
                    throw std::runtime_error("TIFF metadata does not match pages");
                shape = std::move(savedShape);
                metadata = detail::descriptionMetadata(object);
            } else if (text.startsWith("ImageJ=")) {
                QMap<QByteArray, QByteArray> fields;
                for (const auto &line : text.split('\n')) {
                    const auto split = line.indexOf('=');
                    if (split > 0) fields[line.left(split)] = line.mid(split + 1).trimmed();
                }
                const auto channels = std::max(1, fields.value("channels", "1").toInt());
                const auto slices = std::max(1, fields.value("slices", "1").toInt());
                const auto frames = std::max(1, fields.value("frames", "1").toInt());
                if (static_cast<std::size_t>(channels) * slices * frames != pages)
                    throw std::runtime_error("ImageJ stack dimensions do not match TIFF pages");
                shape = {width, height, static_cast<std::size_t>(slices), static_cast<std::size_t>(channels), static_cast<std::size_t>(frames)};
                metadata.axes = {{"X", 1, 0, QString::fromUtf8(fields.value("unit", "px"))},
                                 {"Y", 1, 0, QString::fromUtf8(fields.value("unit", "px"))},
                                 {"Z", fields.value("spacing", "1").toDouble(), 0, QString::fromUtf8(fields.value("unit", "px"))},
                                 {"C", 1, 0, ""}, {"T", fields.value("finterval", "1").toDouble(), 0, "s"}};
                metadata.attributes.insert("TIFF.ImageJ", QString::fromUtf8(text));
            }
        }
        if (metadata.axes.empty()) metadata.axes = {{"X", 1, 0, "px"}, {"Y", 1, 0, "px"}};
        float xResolution = 0, yResolution = 0;
        uint16_t resolutionUnit = RESUNIT_NONE;
        TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_RESOLUTIONUNIT, &resolutionUnit);
        TIFFGetField(tiff.get(), TIFFTAG_XRESOLUTION, &xResolution);
        TIFFGetField(tiff.get(), TIFFTAG_YRESOLUTION, &yResolution);
        if (xResolution > 0 && yResolution > 0 && (!description || !QByteArray(description).startsWith("ImageCXX\n"))) {
            const double factor = resolutionUnit == RESUNIT_INCH ? 25.4 : resolutionUnit == RESUNIT_CENTIMETER ? 10 : 1;
            metadata.axes[0].spacing = factor / xResolution;
            metadata.axes[1].spacing = factor / yResolution;
            if (resolutionUnit != RESUNIT_NONE) metadata.axes[0].unit = metadata.axes[1].unit = "mm";
        }
        image = Image::allocate(type, shape, metadata);
        const std::size_t rowBytes = width * bytesPerPixel(type), pageBytes = rowBytes * height;
        const bool imageJ = metadata.attributes.contains("TIFF.ImageJ");
        auto *destination = image.mutableData();
        for (std::size_t page = 0; page < pages; ++page) {
            detail::checkIo(stop, progress, static_cast<double>(page) / pages);
            if (page > std::numeric_limits<tdir_t>::max() || !TIFFSetDirectory(tiff.get(), static_cast<tdir_t>(page)))
                throw std::runtime_error("Cannot read TIFF directory");
            uint32_t pageWidth = 0, pageHeight = 0;
            uint16_t pageBits = 0, pageSamples = 1, pageFormat = SAMPLEFORMAT_UINT, pageOrientation = ORIENTATION_TOPLEFT;
            uint16_t pagePlanar = PLANARCONFIG_CONTIG, pagePhotometric = PHOTOMETRIC_MINISBLACK;
            TIFFGetField(tiff.get(), TIFFTAG_IMAGEWIDTH, &pageWidth);
            TIFFGetField(tiff.get(), TIFFTAG_IMAGELENGTH, &pageHeight);
            TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_BITSPERSAMPLE, &pageBits);
            TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLESPERPIXEL, &pageSamples);
            TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLEFORMAT, &pageFormat);
            TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_ORIENTATION, &pageOrientation);
            TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_PLANARCONFIG, &pagePlanar);
            TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_PHOTOMETRIC, &pagePhotometric);
            if (pageWidth != width || pageHeight != height || pageBits != bits || pageSamples != samples
                || pageFormat != format || pageOrientation != ORIENTATION_TOPLEFT
                || pagePlanar != PLANARCONFIG_CONTIG || pagePhotometric != photometric)
                throw std::runtime_error("TIFF pages have inconsistent dimensions, encoding, or unsupported orientation");
            std::size_t targetPage = page;
            if (imageJ) {
                const auto channel = page % image.channels();
                const auto z = (page / image.channels()) % image.depth();
                const auto time = page / (image.channels() * image.depth());
                targetPage = z + image.depth() * (channel + image.channels() * time);
            }
            if (TIFFIsTiled(tiff.get())) {
                uint32_t tileWidth = 0, tileHeight = 0;
                TIFFGetField(tiff.get(), TIFFTAG_TILEWIDTH, &tileWidth);
                TIFFGetField(tiff.get(), TIFFTAG_TILELENGTH, &tileHeight);
                const auto tileSize = TIFFTileSize(tiff.get());
                if (!tileWidth || !tileHeight || tileSize <= 0) throw std::runtime_error("Invalid TIFF tile layout");
                if (tileWidth > static_cast<std::size_t>(tileSize) / bytesPerPixel(type) / tileHeight
                    || static_cast<std::size_t>(tileWidth) * bytesPerPixel(type) * tileHeight != static_cast<std::size_t>(tileSize))
                    throw std::runtime_error("Invalid TIFF tile buffer size");
                std::vector<std::byte> tile(static_cast<std::size_t>(tileSize));
                for (std::size_t y = 0; y < height; y += tileHeight) {
                    detail::checkIo(stop, {}, 0);
                    for (std::size_t x = 0; x < width; x += tileWidth) {
                        if (TIFFReadTile(tiff.get(), tile.data(), static_cast<uint32_t>(x), static_cast<uint32_t>(y), 0, 0) != tileSize)
                            throw std::runtime_error("TIFF tile is truncated or corrupt");
                        for (std::size_t row = 0; row < std::min<std::size_t>(tileHeight, height - y); ++row)
                            std::memcpy(destination + targetPage * pageBytes + (y + row) * rowBytes + x * bytesPerPixel(type),
                                        tile.data() + row * tileWidth * bytesPerPixel(type), std::min<std::size_t>(tileWidth, width - x) * bytesPerPixel(type));
                    }
                }
            } else {
                const auto scanline = TIFFScanlineSize(tiff.get());
                if (scanline < 0 || static_cast<std::size_t>(scanline) != rowBytes) throw std::runtime_error("Invalid TIFF scanline layout");
                for (uint32_t y = 0; y < height; ++y) {
                    detail::checkIo(stop, {}, 0);
                    if (TIFFReadScanline(tiff.get(), destination + targetPage * pageBytes + y * rowBytes, y, 0) < 0)
                        throw std::runtime_error("TIFF image is truncated or corrupt");
                }
            }
        }
        if (photometric == PHOTOMETRIC_MINISWHITE && samples == 1) {
            if (type != PixelType::UInt8 && type != PixelType::UInt16) throw std::runtime_error("Unsupported min-is-white scientific TIFF");
            auto *bytes = image.mutableData();
            if (type == PixelType::UInt8)
                for (std::size_t i = 0; i < image.byteCount(); ++i) bytes[i] = std::byte{255} ^ bytes[i];
            else {
                auto *values = reinterpret_cast<std::uint16_t *>(bytes);
                for (std::size_t i = 0; i < image.pixelCount(); ++i) values[i] = static_cast<std::uint16_t>(65535 - values[i]);
            }
        }
    } else {
        QImageReader reader(path);
        // Orientation is retained in pixel order; scientific coordinates are never transformed implicitly.
        reader.setAutoTransform(false);
        QImage raster = reader.read();
        if (raster.isNull() && reader.error() == QImageReader::UnsupportedFormatError)
            return readDicom(path, stop, progress);
        if (raster.isNull()) throw std::runtime_error(reader.errorString().toStdString());
        if (raster.depth() > 32)
            throw std::runtime_error("High-bit-depth color raster pixels are unsupported; no conversion was applied");
        PixelType type;
        if (raster.format() == QImage::Format_Grayscale16) type = PixelType::UInt16;
        else if (raster.format() == QImage::Format_Grayscale8 || (raster.format() == QImage::Format_Indexed8 && raster.isGrayscale())) { type = PixelType::UInt8; raster = raster.convertToFormat(QImage::Format_Grayscale8); }
        else if (raster.hasAlphaChannel()) { type = PixelType::RGBA8; raster = raster.convertToFormat(QImage::Format_RGBA8888); }
        else { type = PixelType::RGB8; raster = raster.convertToFormat(QImage::Format_RGB888); }
        const auto object = detail::rasterImageDescription(raster);
        auto metadata = detail::descriptionMetadata(object);
        if (metadata.axes.empty()) {
            metadata.axes = {{"X", raster.dotsPerMeterX() > 0 ? 1000.0 / raster.dotsPerMeterX() : 1, 0, raster.dotsPerMeterX() > 0 ? "mm" : "px"},
                             {"Y", raster.dotsPerMeterY() > 0 ? 1000.0 / raster.dotsPerMeterY() : 1, 0, raster.dotsPerMeterY() > 0 ? "mm" : "px"}};
        }
        image = Image::allocate(type, {static_cast<std::size_t>(raster.width()), static_cast<std::size_t>(raster.height())}, metadata);
        const auto rowBytes = image.width() * bytesPerPixel(type);
        auto *destination = image.mutableData();
        for (int y = 0; y < raster.height(); ++y) {
            detail::checkIo(stop, {}, 0);
            std::memcpy(destination + static_cast<std::size_t>(y) * rowBytes, raster.constScanLine(y), rowBytes);
        }
    }
    image.metadata().sourcePath = info.absoluteFilePath();
    if (image.metadata().name.isEmpty()) image.metadata().name = info.fileName();
    detail::checkIo(stop, progress, 1);
    return image;
}

void writeImage(const Image &input, const QString &path, std::stop_token stop, IoProgress progress)
{
    if (input.empty()) throw std::runtime_error("Cannot save an empty image");
    detail::checkIo(stop, progress, 0);
    const QString suffix = QFileInfo(path).suffix().toLower();
    const Image image = input.materialized();
    if (suffix == "mhd" || suffix == "mha" || suffix == "nii" || suffix == "gz" || suffix == "nrrd" || suffix == "nhdr") {
        writeScientific(image, path, stop, progress);
    } else if (suffix == "dcm" || suffix == "dicom") {
        writeDicom(image, path, stop, progress);
    } else if (suffix == "raw" || suffix == "bin") {
        QLockFile lock(path + ".imagecxx-save.lock");
        if (!lock.tryLock(0)) throw std::runtime_error("Another writer is saving this raw image");
        if (QFileInfo::exists(path) || QFileInfo::exists(path + ".json"))
            throw std::runtime_error("Raw data and metadata cannot be atomically replaced together; choose a new filename");
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot create raw image");
        const auto *source = reinterpret_cast<const char *>(image.data());
        std::size_t remaining = image.byteCount();
        while (remaining) {
            detail::checkIo(stop, progress, 1.0 - static_cast<double>(remaining) / image.byteCount());
            const auto count = std::min<std::size_t>(remaining, 4 * 1024 * 1024);
            if (file.write(source, static_cast<qint64>(count)) != static_cast<qint64>(count)) throw std::runtime_error("Cannot write raw image");
            source += count;
            remaining -= count;
        }
        auto object = detail::imageDescription(image);
        object.insert("byteOrder", std::endian::native == std::endian::big ? "big" : "little");
        detail::checkIo(stop, progress, 0.95);
        bool metadataPublished = false;
        try {
            detail::checkIo(stop, {}, 0);
            QSaveFile sidecar(path + ".json");
            const auto bytes = QJsonDocument(object).toJson();
            if (!sidecar.open(QIODevice::WriteOnly) || sidecar.write(bytes) != bytes.size() || !sidecar.commit())
                throw std::runtime_error("Cannot write raw image metadata");
            metadataPublished = true;
            detail::checkIo(stop, {}, 0);
            if (!file.commit()) throw std::runtime_error("Cannot commit raw image");
        } catch (...) {
            if (metadataPublished) QFile::remove(path + ".json");
            throw;
        }
    } else if (suffix == "tif" || suffix == "tiff") {
        const bool bigTiff = image.byteCount() > 3500ULL * 1024 * 1024;
        QTemporaryDir staging(QDir(QFileInfo(path).absolutePath()).filePath(".imagecxx-XXXXXX"));
        if (!staging.isValid()) throw std::runtime_error("Cannot create temporary TIFF output");
        const auto stagedPath = staging.filePath("image.tif");
        std::unique_ptr<TIFF, decltype(&TIFFClose)> tiff(TIFFOpen(QFile::encodeName(stagedPath).constData(), bigTiff ? "w8" : "w"), TIFFClose);
        if (!tiff) throw std::runtime_error("Cannot create TIFF");
        if (image.width() > UINT32_MAX || image.height() > UINT32_MAX) throw std::runtime_error("TIFF dimensions exceed format limits");
        const auto samples = static_cast<uint16_t>(componentCount(image.pixelType()));
        const auto bits = static_cast<uint16_t>(8 * bytesPerPixel(image.pixelType()) / samples);
        const uint16_t format = image.pixelType() == PixelType::Int16 ? SAMPLEFORMAT_INT :
            (image.pixelType() == PixelType::Float32 || image.pixelType() == PixelType::Float64) ? SAMPLEFORMAT_IEEEFP : SAMPLEFORMAT_UINT;
        const auto description = QByteArray("ImageCXX\n") + QJsonDocument(detail::imageDescription(image)).toJson(QJsonDocument::Compact);
        const auto rowBytes = image.width() * bytesPerPixel(image.pixelType());
        const auto pages = image.pixelCount() / (image.width() * image.height());
        const auto *source = image.data();
        for (std::size_t page = 0; page < pages; ++page) {
            detail::checkIo(stop, progress, static_cast<double>(page) / pages);
            TIFFSetField(tiff.get(), TIFFTAG_IMAGEWIDTH, static_cast<uint32_t>(image.width()));
            TIFFSetField(tiff.get(), TIFFTAG_IMAGELENGTH, static_cast<uint32_t>(image.height()));
            TIFFSetField(tiff.get(), TIFFTAG_SAMPLESPERPIXEL, samples);
            TIFFSetField(tiff.get(), TIFFTAG_BITSPERSAMPLE, bits);
            TIFFSetField(tiff.get(), TIFFTAG_SAMPLEFORMAT, format);
            TIFFSetField(tiff.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
            TIFFSetField(tiff.get(), TIFFTAG_PHOTOMETRIC, samples >= 3 ? PHOTOMETRIC_RGB : PHOTOMETRIC_MINISBLACK);
            TIFFSetField(tiff.get(), TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
            TIFFSetField(tiff.get(), TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
            TIFFSetField(tiff.get(), TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tiff.get(), 0));
            if (!TIFFSetField(tiff.get(), TIFFTAG_IMAGEDESCRIPTION, description.constData()))
                throw std::runtime_error("Cannot preserve TIFF metadata");
            if (samples == 4) { uint16_t extra = EXTRASAMPLE_UNASSALPHA; TIFFSetField(tiff.get(), TIFFTAG_EXTRASAMPLES, 1, &extra); }
            for (std::size_t y = 0; y < image.height(); ++y) {
                detail::checkIo(stop, {}, 0);
                if (TIFFWriteScanline(tiff.get(), const_cast<std::byte *>(source + (page * image.height() + y) * rowBytes), static_cast<uint32_t>(y), 0) < 0)
                    throw std::runtime_error("Cannot write TIFF pixels");
            }
            if (!TIFFWriteDirectory(tiff.get())) throw std::runtime_error("Cannot write TIFF directory");
        }
        tiff.reset();
        detail::checkIo(stop, progress, 0.95);
        detail::atomicCopy(stagedPath, path, stop);
    } else {
        if (image.depth() != 1 || image.channels() != 1 || image.frames() != 1)
            throw std::runtime_error("This raster format saves a single 2D image; select a slice or use TIFF");
        if (image.width() > INT_MAX || image.height() > INT_MAX) throw std::runtime_error("Raster image dimensions exceed format limits");
        const bool jpeg = suffix == "jpg" || suffix == "jpeg";
        QImage::Format format;
        if (image.pixelType() == PixelType::UInt8) format = QImage::Format_Grayscale8;
        else if (image.pixelType() == PixelType::UInt16 && !jpeg) format = QImage::Format_Grayscale16;
        else if (image.pixelType() == PixelType::RGB8) format = QImage::Format_RGB888;
        else if (image.pixelType() == PixelType::RGBA8 && !jpeg) format = QImage::Format_RGBA8888;
        else throw std::runtime_error("Raster format cannot preserve this pixel type; use TIFF or a scientific format");
        QImage raster(static_cast<int>(image.width()), static_cast<int>(image.height()), format);
        if (raster.isNull()) throw std::runtime_error("Cannot allocate raster export");
        const auto rowBytes = image.width() * bytesPerPixel(image.pixelType());
        for (int y = 0; y < raster.height(); ++y) {
            detail::checkIo(stop, {}, 0);
            std::memcpy(raster.scanLine(y), image.data() + static_cast<std::size_t>(y) * rowBytes, rowBytes);
        }
        auto plane = image.slice();
        if (plane.metadata().originalMetadata.isEmpty()) {
            plane.metadata().originalMetadata = detail::imageDescription(image);
            plane.metadata().originalMetadata.remove("originalMetadata");
        }
        const auto description = detail::imageDescription(plane);
        const auto text = detail::imageDescriptionText(plane);
        for (auto it = text.cbegin(); it != text.cend(); ++it) raster.setText(it.key(), it.value());
        QTemporaryDir staging(QDir(QFileInfo(path).absolutePath()).filePath(".imagecxx-XXXXXX"));
        if (!staging.isValid()) throw std::runtime_error("Cannot create temporary raster output");
        const auto stagedPath = staging.filePath("image." + suffix);
        QImageWriter writer(stagedPath, suffix.toLatin1());
        if (jpeg) writer.setQuality(95);
        if (!writer.write(raster)) throw std::runtime_error(writer.errorString().toStdString());
        raster = {}; // Release export pixels before decoding the verification image.
        QImageReader verification(stagedPath);
        const auto stored = verification.read();
        if (stored.isNull() || detail::rasterImageDescription(stored) != description)
            throw std::runtime_error("Raster image codec did not preserve ImageCXX metadata");
        detail::checkIo(stop, progress, 0.95);
        detail::checkIo(stop, {}, 0);
        detail::atomicCopy(stagedPath, path, stop);
    }
    if (progress) progress(1);
}

} // namespace imagecxx
