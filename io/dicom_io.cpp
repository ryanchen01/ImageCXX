#include "io/image_io.h"
#include "io/io_detail.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <dcmtk/config/osconfig.h>
#include <dcmtk/dcmdata/dctk.h>
#include <dcmtk/dcmdata/dcuid.h>
#include <dcmtk/dcmdata/dcxfer.h>
#include <dcmtk/dcmdata/dcrledrg.h>
#include <dcmtk/dcmjpeg/djdecode.h>
#include <dcmtk/dcmjpls/djdecode.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numeric>
#include <mutex>
#include <stdexcept>

namespace imagecxx {
namespace {
void dicomCheck(const OFCondition &condition)
{
    if (condition.bad()) throw std::runtime_error(condition.text());
}

QString dicomTagName(const DcmTag &tag)
{
    return QStringLiteral("DICOM.%1,%2").arg(tag.getGTag(), 4, 16, QLatin1Char('0')).arg(tag.getETag(), 4, 16, QLatin1Char('0'));
}

double dicomNumber(DcmItem *dataset, const DcmTagKey &tag, double fallback, unsigned long position = 0)
{
    double value = fallback;
    if (dataset->findAndGetFloat64(tag, value, position).good()) return value;
    if (dataset->tagExists(tag)) throw std::runtime_error("Invalid DICOM numeric metadata");
    return fallback;
}
} // namespace

Image readDicom(const QString &path, std::stop_token stop, IoProgress progress)
{
    detail::checkIo(stop, progress, 0);
    DcmFileFormat file;
    dicomCheck(file.loadFile(QFile::encodeName(path).constData()));
    auto *dataset = file.getDataset();
    dicomCheck(dataset->convertToUTF8());
    if (DcmXfer(dataset->getOriginalXfer()).isPixelDataCompressed()) {
        static std::once_flag codecs;
        std::call_once(codecs, [] {
            DcmRLEDecoderRegistration::registerCodecs();
            DJDecoderRegistration::registerCodecs();
            DJLSDecoderRegistration::registerCodecs();
        });
        dicomCheck(dataset->chooseRepresentation(EXS_LittleEndianExplicit, nullptr));
        if (!dataset->canWriteXfer(EXS_LittleEndianExplicit))
            throw std::runtime_error("DICOM compression is unsupported by the installed DCMTK codecs");
    }
    if (dataset->tagExists(DCM_PerFrameFunctionalGroupsSequence))
        throw std::runtime_error("Enhanced DICOM per-frame geometry is unsupported");
    Uint16 rows = 0, columns = 0, samples = 1, bits = 0, stored = 0, highBit = 0, representation = 0;
    dicomCheck(dataset->findAndGetUint16(DCM_Rows, rows));
    dicomCheck(dataset->findAndGetUint16(DCM_Columns, columns));
    dataset->findAndGetUint16(DCM_SamplesPerPixel, samples);
    dicomCheck(dataset->findAndGetUint16(DCM_BitsAllocated, bits));
    dicomCheck(dataset->findAndGetUint16(DCM_BitsStored, stored));
    dicomCheck(dataset->findAndGetUint16(DCM_HighBit, highBit));
    dataset->findAndGetUint16(DCM_PixelRepresentation, representation);
    if (!rows || !columns || stored == 0 || stored > bits || highBit >= bits || highBit + 1 < stored)
        throw std::runtime_error("Invalid DICOM pixel encoding");
    OFString photometric;
    dicomCheck(dataset->findAndGetOFString(DCM_PhotometricInterpretation, photometric));
    PixelType type;
    if (samples == 1 && bits == 8 && representation == 0) type = PixelType::UInt8;
    else if (samples == 1 && bits == 16 && representation == 0) type = PixelType::UInt16;
    else if (samples == 1 && bits == 16 && representation == 1) type = PixelType::Int16;
    else if (samples == 3 && bits == 8 && representation == 0 && photometric == "RGB") type = PixelType::RGB8;
    else throw std::runtime_error("Unsupported DICOM pixel type");
    if (samples == 1 && photometric != "MONOCHROME1" && photometric != "MONOCHROME2")
        throw std::runtime_error("Unsupported DICOM photometric interpretation");
    Sint32 frames = 1;
    if (dataset->tagExists(DCM_NumberOfFrames)) dicomCheck(dataset->findAndGetSint32(DCM_NumberOfFrames, frames));
    if (frames <= 0) throw std::runtime_error("Invalid DICOM frame count");
    ImageMetadata metadata;
    metadata.name = QFileInfo(path).fileName();
    metadata.sourcePath = QFileInfo(path).absoluteFilePath();
    const double spacingX = dicomNumber(dataset, DCM_PixelSpacing, 1, 1);
    const double spacingY = dicomNumber(dataset, DCM_PixelSpacing, 1, 0);
    const double spacingZ = std::abs(dicomNumber(dataset, DCM_SpacingBetweenSlices, dicomNumber(dataset, DCM_SliceThickness, 1)));
    std::array<double, 3> origin{0, 0, 0};
    for (unsigned int axis = 0; axis < 3; ++axis) origin[axis] = dicomNumber(dataset, DCM_ImagePositionPatient, 0, axis);
    std::array<double, 6> orientation{1, 0, 0, 0, 1, 0};
    for (unsigned int i = 0; i < 6; ++i) orientation[i] = dicomNumber(dataset, DCM_ImageOrientationPatient, orientation[i], i);
    const std::array<double, 3> normal{
        orientation[1] * orientation[5] - orientation[2] * orientation[4],
        orientation[2] * orientation[3] - orientation[0] * orientation[5],
        orientation[0] * orientation[4] - orientation[1] * orientation[3]};
    double xLength = 0, yLength = 0, dot = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        xLength += orientation[i] * orientation[i];
        yLength += orientation[i + 3] * orientation[i + 3];
        dot += orientation[i] * orientation[i + 3];
    }
    if (!std::isfinite(xLength + yLength + dot) || std::abs(xLength - 1) > 1e-3 || std::abs(yLength - 1) > 1e-3 || std::abs(dot) > 1e-3)
        throw std::runtime_error("DICOM patient orientation is not orthonormal");
    metadata.direction = {orientation[0], orientation[3], normal[0], orientation[1], orientation[4], normal[1], orientation[2], orientation[5], normal[2]};
    metadata.axes = {{"X", spacingX, origin[0], "mm"}, {"Y", spacingY, origin[1], "mm"}, {"Z", spacingZ, origin[2], "mm"}};
    for (double spacing : {spacingX, spacingY, spacingZ})
        if (!std::isfinite(spacing) || spacing <= 0) throw std::runtime_error("Invalid DICOM spacing");
    metadata.intensityScale = dicomNumber(dataset, DCM_RescaleSlope, 1);
    metadata.intensityOffset = dicomNumber(dataset, DCM_RescaleIntercept, 0);
    if (!std::isfinite(metadata.intensityScale) || !std::isfinite(metadata.intensityOffset) || metadata.intensityScale == 0)
        throw std::runtime_error("DICOM intensity calibration is invalid");
    OFString unit;
    if (dataset->findAndGetOFString(DCM_RescaleType, unit).good()) metadata.intensityUnit = QString::fromLatin1(unit.c_str());
    for (unsigned long i = 0; i < dataset->card(); ++i) {
        auto *element = dataset->getElement(i);
        const auto &tag = element->getTag();
        if (tag.getGTag() == 0x7fe0 || element->getLength() > 65536 || element->ident() == EVR_SQ) continue;
        OFString value;
        if (element->getOFStringArray(value).good()) metadata.attributes.insert(dicomTagName(tag), QString::fromUtf8(value.c_str()));
    }
    auto image = Image::allocate(type, {columns, rows, static_cast<std::size_t>(frames)}, metadata);
    detail::checkIo(stop, progress, 0.2);
    auto *destination = image.mutableData();
    const auto count = image.pixelCount() * samples;
    if (bits == 8) {
        const Uint8 *source = nullptr;
        unsigned long length = 0;
        dicomCheck(dataset->findAndGetUint8Array(DCM_PixelData, source, &length));
        if (length < count) throw std::runtime_error("DICOM pixel data is truncated");
        Uint16 planar = 0;
        if (samples == 3) dataset->findAndGetUint16(DCM_PlanarConfiguration, planar);
        if (samples == 3 && planar == 1) {
            auto *output = reinterpret_cast<Uint8 *>(destination);
            const auto plane = image.width() * image.height();
            for (std::size_t frame = 0; frame < image.depth(); ++frame)
                for (std::size_t pixel = 0; pixel < plane; ++pixel)
                    for (std::size_t component = 0; component < 3; ++component)
                        output[(frame * plane + pixel) * 3 + component] = source[frame * plane * 3 + component * plane + pixel];
        } else {
            if (planar > 1) throw std::runtime_error("Invalid DICOM planar configuration");
            std::memcpy(destination, source, image.byteCount());
        }
        if (samples == 1 && (stored < 8 || highBit != 7)) {
            const unsigned int mask = (1u << stored) - 1;
            for (std::size_t i = 0; i < count; ++i) reinterpret_cast<Uint8 *>(destination)[i] = static_cast<Uint8>((source[i] >> (highBit + 1 - stored)) & mask);
        }
    } else {
        const Uint16 *source = nullptr;
        unsigned long length = 0;
        dicomCheck(dataset->findAndGetUint16Array(DCM_PixelData, source, &length));
        if (length < count) throw std::runtime_error("DICOM pixel data is truncated");
        auto *output = reinterpret_cast<Uint16 *>(destination);
        const unsigned int mask = (1u << stored) - 1;
        const unsigned int sign = 1u << (stored - 1);
        for (std::size_t i = 0; i < count; ++i) {
            unsigned int value = (source[i] >> (highBit + 1 - stored)) & mask;
            if (representation && (value & sign)) value |= ~mask;
            output[i] = static_cast<Uint16>(value);
        }
    }
    detail::checkIo(stop, progress, 1);
    return image;
}

Image readDicomSeries(const QStringList &paths, std::stop_token stop, IoProgress progress)
{
    if (paths.isEmpty()) throw std::runtime_error("DICOM series is empty");
    std::vector<Image> slices;
    for (int i = 0; i < paths.size(); ++i) {
        detail::checkIo(stop, progress, 0.8 * i / paths.size());
        auto slice = readDicom(paths[i], stop);
        if (slice.depth() != 1 || componentCount(slice.pixelType()) != 1)
            throw std::runtime_error("Series import requires single-frame scalar DICOM files");
        slices.push_back(std::move(slice));
    }
    const auto &reference = slices.front();
    const auto &metadata = reference.metadata();
    const auto &direction = metadata.direction;
    if (metadata.attributes.value("DICOM.0020,000e").isEmpty())
        throw std::runtime_error("DICOM series requires a Series Instance UID");
    const std::array<double, 3> normal{direction[2], direction[5], direction[8]};
    std::vector<double> positions;
    for (const auto &slice : slices) {
        const auto &current = slice.metadata();
        if (slice.width() != reference.width() || slice.height() != reference.height() || slice.pixelType() != reference.pixelType() ||
            current.attributes.value("DICOM.0020,000e") != metadata.attributes.value("DICOM.0020,000e") ||
            current.attributes.value("DICOM.0020,000d") != metadata.attributes.value("DICOM.0020,000d") ||
            current.attributes.value("DICOM.0020,0052") != metadata.attributes.value("DICOM.0020,0052") ||
            current.intensityScale != metadata.intensityScale || current.intensityOffset != metadata.intensityOffset)
            throw std::runtime_error("DICOM files have inconsistent series or pixel encoding");
        if (!current.attributes.contains("DICOM.0020,0032") || !current.attributes.contains("DICOM.0020,0037"))
            throw std::runtime_error("DICOM series requires patient position and orientation on every slice");
        for (std::size_t i = 0; i < 9; ++i)
            if (std::abs(current.direction[i] - direction[i]) > 1e-5) throw std::runtime_error("DICOM series has inconsistent orientation");
        for (std::size_t axis = 0; axis < 2; ++axis)
            if (std::abs(current.axes[axis].spacing - metadata.axes[axis].spacing) > 1e-5)
                throw std::runtime_error("DICOM series has inconsistent pixel spacing");
        double position = 0;
        for (std::size_t axis = 0; axis < 3; ++axis) position += current.axes[axis].origin * normal[axis];
        positions.push_back(position);
    }
    std::vector<std::size_t> order(slices.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&positions](auto a, auto b) { return positions[a] < positions[b]; });
    auto outputMetadata = slices[order.front()].metadata();
    if (slices.size() > 1) {
        const auto spacing = positions[order[1]] - positions[order[0]];
        if (spacing < 1e-6) throw std::runtime_error("DICOM series contains duplicate slice positions");
        outputMetadata.axes[2].spacing = spacing;
        for (std::size_t i = 1; i < order.size(); ++i) {
            if (std::abs(positions[order[i]] - positions[order[i - 1]] - spacing) > std::max(1e-4, spacing * 1e-3))
                throw std::runtime_error("DICOM series has nonuniform slice spacing");
            // Reject gantry tilt rather than silently assigning incorrect orthogonal geometry.
            for (std::size_t axis = 0; axis < 3; ++axis)
                if (std::abs(slices[order[i]].metadata().axes[axis].origin - outputMetadata.axes[axis].origin - normal[axis] * spacing * i) > 1e-3)
                    throw std::runtime_error("DICOM series has a shifted slice grid");
        }
    }
    auto image = Image::allocate(reference.pixelType(), {reference.width(), reference.height(), slices.size()}, outputMetadata);
    auto *destination = image.mutableData();
    for (std::size_t i = 0; i < order.size(); ++i) {
        detail::checkIo(stop, progress, 0.8 + 0.2 * i / order.size());
        std::memcpy(destination + i * reference.byteCount(), slices[order[i]].data(), reference.byteCount());
    }
    detail::checkIo(stop, progress, 1);
    return image;
}

void writeDicom(const Image &input, const QString &path, std::stop_token stop, IoProgress progress)
{
    detail::checkIo(stop, progress, 0);
    if (input.empty() || input.width() > 65535 || input.height() > 65535 || input.channels() != 1 || input.frames() != 1 || input.depth() > 1 || input.byteCount() > UINT32_MAX - 1)
        throw std::runtime_error("DICOM export supports one 2D image; save volumes as TIFF or a scientific format");
    if (input.pixelType() != PixelType::UInt8 && input.pixelType() != PixelType::UInt16 && input.pixelType() != PixelType::Int16 && input.pixelType() != PixelType::RGB8)
        throw std::runtime_error("DICOM export requires UInt8, UInt16, Int16 or RGB8 pixels");
    const Image image = input.contiguous() ? input : input.clone();
    const auto &metadata = image.metadata();
    DcmFileFormat file;
    auto *dataset = file.getDataset();
    for (auto it = metadata.attributes.cbegin(); it != metadata.attributes.cend(); ++it) {
        if (!it.key().startsWith("DICOM.")) continue;
        const auto parts = it.key().mid(6).split(',');
        bool groupOk = false, elementOk = false;
        if (parts.size() != 2) continue;
        const auto group = parts[0].toUShort(&groupOk, 16), element = parts[1].toUShort(&elementOk, 16);
        if (!groupOk || !elementOk || group < 8 || group == 0x7fe0 || element == 0) continue;
        dataset->putAndInsertString(DcmTag(DcmTagKey(group, element)), it.value().toUtf8().constData());
    }
    char uid[100];
    dicomCheck(dataset->putAndInsertString(DCM_SOPClassUID, UID_SecondaryCaptureImageStorage));
    dicomCheck(dataset->putAndInsertString(DCM_SOPInstanceUID, dcmGenerateUniqueIdentifier(uid, SITE_INSTANCE_UID_ROOT)));
    if (!dataset->tagExists(DCM_StudyInstanceUID)) dicomCheck(dataset->putAndInsertString(DCM_StudyInstanceUID, dcmGenerateUniqueIdentifier(uid, SITE_STUDY_UID_ROOT)));
    if (!dataset->tagExists(DCM_SeriesInstanceUID)) dicomCheck(dataset->putAndInsertString(DCM_SeriesInstanceUID, dcmGenerateUniqueIdentifier(uid, SITE_SERIES_UID_ROOT)));
    if (!dataset->tagExists(DCM_PatientName)) dataset->putAndInsertString(DCM_PatientName, "");
    if (!dataset->tagExists(DCM_PatientID)) dataset->putAndInsertString(DCM_PatientID, "");
    if (!dataset->tagExists(DCM_Modality)) dataset->putAndInsertString(DCM_Modality, "OT");
    dicomCheck(dataset->putAndInsertString(DCM_SpecificCharacterSet, "ISO_IR 192"));
    dicomCheck(dataset->putAndInsertString(DCM_ConversionType, "WSD"));
    dicomCheck(dataset->putAndInsertString(DCM_ImageType, "DERIVED\\SECONDARY"));
    const Uint16 bits = image.pixelType() == PixelType::UInt16 || image.pixelType() == PixelType::Int16 ? 16 : 8;
    const Uint16 samples = image.pixelType() == PixelType::RGB8 ? 3 : 1;
    dicomCheck(dataset->putAndInsertUint16(DCM_Rows, static_cast<Uint16>(image.height())));
    dicomCheck(dataset->putAndInsertUint16(DCM_Columns, static_cast<Uint16>(image.width())));
    dicomCheck(dataset->putAndInsertUint16(DCM_SamplesPerPixel, samples));
    dicomCheck(dataset->putAndInsertUint16(DCM_BitsAllocated, bits));
    dicomCheck(dataset->putAndInsertUint16(DCM_BitsStored, bits));
    dicomCheck(dataset->putAndInsertUint16(DCM_HighBit, bits - 1));
    dicomCheck(dataset->putAndInsertUint16(DCM_PixelRepresentation, image.pixelType() == PixelType::Int16 ? 1 : 0));
    const bool monochromeOne = metadata.attributes.value("DICOM.0028,0004").trimmed() == "MONOCHROME1";
    dicomCheck(dataset->putAndInsertString(DCM_PhotometricInterpretation, samples == 3 ? "RGB" : monochromeOne ? "MONOCHROME1" : "MONOCHROME2"));
    dataset->findAndDeleteElement(DCM_NumberOfFrames);
    if (samples == 3) dicomCheck(dataset->putAndInsertUint16(DCM_PlanarConfiguration, 0));
    if (metadata.axes.size() >= 2) {
        const auto spacing = QString::number(metadata.axes[1].spacing, 'g', 12) + "\\" + QString::number(metadata.axes[0].spacing, 'g', 12);
        dicomCheck(dataset->putAndInsertString(DCM_PixelSpacing, spacing.toLatin1().constData()));
        std::array<double, 3> origin{metadata.axes[0].origin, metadata.axes[1].origin, metadata.axes.size() > 2 ? metadata.axes[2].origin : 0};
        if (metadata.attributes.contains("ImageCXX.EmbeddedPlaneOrigin")) {
            const auto embedded = QJsonDocument::fromJson(metadata.attributes.value("ImageCXX.EmbeddedPlaneOrigin").toUtf8()).array();
            if (embedded.size() != 3) throw std::runtime_error("Invalid embedded DICOM plane origin");
            for (int axis = 0; axis < 3; ++axis) {
                if (!embedded[axis].isDouble() || !std::isfinite(embedded[axis].toDouble())) throw std::runtime_error("Invalid embedded DICOM plane origin");
                origin[axis] = embedded[axis].toDouble();
            }
        }
        const auto position = QString::number(origin[0], 'g', 12) + "\\" + QString::number(origin[1], 'g', 12) + "\\" + QString::number(origin[2], 'g', 12);
        dicomCheck(dataset->putAndInsertString(DCM_ImagePositionPatient, position.toLatin1().constData()));
    }
    const auto &direction = metadata.direction;
    QStringList orientation;
    if (direction.size() == 9) {
        for (std::size_t column = 0; column < 2; ++column)
            for (std::size_t row = 0; row < 3; ++row) orientation << QString::number(direction[row * 3 + column], 'g', 12);
    } else orientation = {"1", "0", "0", "0", "1", "0"};
    dicomCheck(dataset->putAndInsertString(DCM_ImageOrientationPatient, orientation.join('\\').toLatin1().constData()));
    dicomCheck(dataset->putAndInsertString(DCM_RescaleSlope, QString::number(metadata.intensityScale, 'g', 12).toLatin1().constData()));
    dicomCheck(dataset->putAndInsertString(DCM_RescaleIntercept, QString::number(metadata.intensityOffset, 'g', 12).toLatin1().constData()));
    if (!metadata.intensityUnit.isEmpty()) dicomCheck(dataset->putAndInsertString(DCM_RescaleType, metadata.intensityUnit.toLatin1().constData()));
    detail::checkIo(stop, progress, 0.3);
    if (bits == 8) dicomCheck(dataset->putAndInsertUint8Array(DCM_PixelData, reinterpret_cast<const Uint8 *>(image.data()), static_cast<unsigned long>(image.byteCount())));
    else dicomCheck(dataset->putAndInsertUint16Array(DCM_PixelData, reinterpret_cast<const Uint16 *>(image.data()), static_cast<unsigned long>(image.pixelCount())));
    QTemporaryDir staging(QDir(QFileInfo(path).absolutePath()).filePath(".imagecxx-XXXXXX"));
    if (!staging.isValid()) throw std::runtime_error("Cannot create temporary DICOM output");
    const auto stagedPath = staging.filePath("image.dcm");
    dicomCheck(file.saveFile(QFile::encodeName(stagedPath).constData(), EXS_LittleEndianExplicit));
    detail::checkIo(stop, progress, 0.85);
    detail::atomicCopy(stagedPath, path, stop);
    if (progress) progress(1);
}

} // namespace imagecxx
