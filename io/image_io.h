#pragma once

#include "core/image.h"
#include <functional>
#include <optional>
#include <stop_token>
#include <QJsonObject>

namespace imagecxx {

using IoProgress = std::function<void(double)>;
enum class ByteOrder { Native, LittleEndian, BigEndian };

struct RawImportOptions {
    PixelType pixelType = PixelType::UInt16;
    std::vector<std::size_t> shape;
    std::size_t headerBytes = 0;
    ByteOrder byteOrder = ByteOrder::Native;
    bool memoryMap = true;
    ImageMetadata metadata;
};

// Parses the validated layout and calibration stored in a raw sidecar.
RawImportOptions rawImportOptionsFromJson(const QJsonObject &description);

// Throws a standard exception for invalid, unsupported, truncated, or cancelled input.
// PNG preserves UInt8/UInt16 gray and RGB(A)8. JPEG is lossy UInt8 gray or RGB8.
// TIFF preserves all model types and ImageCXX metadata, and imports ImageJ stacks.
// ITK handles MHD/MHA, NIfTI and NRRD. DCMTK handles native, RLE, JPEG and JPEG-LS DICOM.
Image readImage(const QString &path, const std::optional<RawImportOptions> &raw = {},
                std::stop_token stop = {}, IoProgress progress = {});
void writeImage(const Image &image, const QString &path,
                std::stop_token stop = {}, IoProgress progress = {});

// All files must belong to one scalar series with consistent geometry and encoding.
// The reader orders slices by their patient-space position, not file name.
Image readDicomSeries(const QStringList &paths, std::stop_token stop = {},
                      IoProgress progress = {});
Image readDicom(const QString &path, std::stop_token stop = {}, IoProgress progress = {});
void writeDicom(const Image &image, const QString &path,
                std::stop_token stop = {}, IoProgress progress = {});
Image readScientific(const QString &path, std::stop_token stop = {}, IoProgress progress = {});
void writeScientific(const Image &image, const QString &path,
                     std::stop_token stop = {}, IoProgress progress = {});

QString imageFileFilter();
QString imageSaveFilter();

struct ImageImport {
    QStringList paths;
    std::size_t dicomBytes = 0;
};

struct ImageImportPlan {
    QList<ImageImport> images;
    QStringList errors;
};

// Recursively expands folders, removes duplicate paths and groups single-frame
// scalar DICOM images by study/series UID without decoding their pixel data.
ImageImportPlan discoverImageImports(const QStringList &paths, std::stop_token stop = {},
                                     IoProgress progress = {});
void checkDicomImportMemory(const QList<ImageImport> &images, std::optional<std::size_t> available);

} // namespace imagecxx
