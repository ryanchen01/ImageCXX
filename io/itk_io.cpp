#include "io/image_io.h"
#include "io/io_detail.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <itkImageIOBase.h>
#include <itkImageIORegion.h>
#include <itkMetaDataObject.h>
#include <itkMetaImageIO.h>
#include <itkNiftiImageIO.h>
#include <itkNrrdImageIO.h>
#include <nifti1.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace imagecxx {
namespace {
itk::ImageIOBase::Pointer scientificAdapter(const QString &path)
{
    const auto suffix = QFileInfo(path).suffix().toLower();
    if (suffix == "mhd" || suffix == "mha") return itk::MetaImageIO::New().GetPointer();
    if (suffix == "nrrd" || suffix == "nhdr") return itk::NrrdImageIO::New().GetPointer();
    if (suffix == "nii" || path.toLower().endsWith(".nii.gz")) return itk::NiftiImageIO::New().GetPointer();
    throw std::runtime_error("Unsupported scientific image extension");
}
} // namespace

Image readScientific(const QString &path, std::stop_token stop, IoProgress progress)
{
    detail::checkIo(stop, progress, 0);
    try {
        auto io = scientificAdapter(path);
        io->SetFileName(QFile::encodeName(path).toStdString());
        io->ReadImageInformation();
        const auto &dictionary = io->GetMetaDataDictionary();
        double intensityScale = 1, intensityOffset = 0;
        if (auto *nifti = dynamic_cast<itk::NiftiImageIO *>(io.GetPointer())) {
            std::string slope, intercept, datatype;
            itk::ExposeMetaData<std::string>(dictionary, "scl_slope", slope);
            itk::ExposeMetaData<std::string>(dictionary, "scl_inter", intercept);
            itk::ExposeMetaData<std::string>(dictionary, "datatype", datatype);
            bool slopeOk = false, interceptOk = false;
            intensityScale = QString::fromStdString(slope).toDouble(&slopeOk);
            intensityOffset = QString::fromStdString(intercept).toDouble(&interceptOk);
            if (!slopeOk || !interceptOk || !std::isfinite(intensityScale) || !std::isfinite(intensityOffset))
                throw std::runtime_error("Invalid NIfTI intensity calibration");
            if (intensityScale == 0) { intensityScale = 1; intensityOffset = 0; }
            if (io->GetNumberOfComponents() > 1) { intensityScale = 1; intensityOffset = 0; }
            // ITK normally promotes and rescales integer input. Keep the stored type and
            // raw samples, with the standard calibration in our separate metadata.
            switch (QString::fromStdString(datatype).toInt()) {
            case NIFTI_TYPE_UINT8: case NIFTI_TYPE_RGB24: case NIFTI_TYPE_RGBA32: io->SetComponentType(itk::IOComponentEnum::UCHAR); break;
            case NIFTI_TYPE_UINT16: io->SetComponentType(itk::IOComponentEnum::USHORT); break;
            case NIFTI_TYPE_INT16: io->SetComponentType(itk::IOComponentEnum::SHORT); break;
            case NIFTI_TYPE_FLOAT32: io->SetComponentType(itk::IOComponentEnum::FLOAT); break;
            case NIFTI_TYPE_FLOAT64: io->SetComponentType(itk::IOComponentEnum::DOUBLE); break;
            default: throw std::runtime_error("NIfTI stored pixel type is unsupported");
            }
            nifti->SetRescaleSlope(1);
            nifti->SetRescaleIntercept(0);
        }
        const unsigned int dimensions = io->GetNumberOfDimensions();
        if (dimensions < 2 || dimensions > 5) throw std::runtime_error("Scientific images support two to five dimensions");
        const auto components = io->GetNumberOfComponents();
        PixelType type;
        const auto component = io->GetComponentType();
        if (components == 1 && component == itk::IOComponentEnum::UCHAR) type = PixelType::UInt8;
        else if (components == 1 && component == itk::IOComponentEnum::USHORT) type = PixelType::UInt16;
        else if (components == 1 && component == itk::IOComponentEnum::SHORT) type = PixelType::Int16;
        else if (components == 1 && component == itk::IOComponentEnum::FLOAT) type = PixelType::Float32;
        else if (components == 1 && component == itk::IOComponentEnum::DOUBLE) type = PixelType::Float64;
        else if (components == 3 && component == itk::IOComponentEnum::UCHAR) type = PixelType::RGB8;
        else if (components == 4 && component == itk::IOComponentEnum::UCHAR) type = PixelType::RGBA8;
        else throw std::runtime_error("Scientific image pixel type is unsupported");
        std::vector<std::size_t> shape;
        ImageMetadata metadata;
        metadata.intensityScale = intensityScale;
        metadata.intensityOffset = intensityOffset;
        const QStringList names{"X", "Y", "Z", "T", "C"};
        itk::ImageIORegion region(dimensions);
        for (unsigned int axis = 0; axis < dimensions; ++axis) {
            shape.push_back(io->GetDimensions(axis));
            metadata.axes.push_back({names[static_cast<int>(axis)], io->GetSpacing(axis), io->GetOrigin(axis), axis < 3 ? "mm" : axis == 3 ? "s" : ""});
            region.SetIndex(axis, 0);
            region.SetSize(axis, io->GetDimensions(axis));
        }
        const auto spatial = std::min<unsigned int>(dimensions, 3);
        metadata.direction.resize(spatial * spatial);
        for (unsigned int column = 0; column < spatial; ++column)
            for (unsigned int row = 0; row < spatial; ++row)
                metadata.direction[row * spatial + column] = io->GetDirection(column)[row];
        for (const auto &key : dictionary.GetKeys()) {
            std::string value;
            if (itk::ExposeMetaData<std::string>(dictionary, key, value))
                metadata.attributes.insert(QString::fromStdString(key), QString::fromStdString(value));
        }
        std::string description;
        const bool ownMetadata = itk::ExposeMetaData<std::string>(dictionary, "ImageCXX", description);
        if (ownMetadata) {
            const auto object = QJsonDocument::fromJson(QByteArray::fromStdString(description)).object();
            metadata = detail::descriptionMetadata(object);
            const auto savedShape = object.value("shape").toArray();
            std::vector<std::size_t> originalShape;
            std::size_t originalCount = 1, fileCount = 1;
            for (auto dimension : shape) fileCount *= dimension;
            for (const auto value : savedShape) {
                const auto size = value.toInteger();
                if (size <= 0 || static_cast<quint64>(size) > std::numeric_limits<std::size_t>::max() / originalCount)
                    throw std::runtime_error("Invalid scientific image metadata dimensions");
                originalShape.push_back(static_cast<std::size_t>(size));
                originalCount *= static_cast<std::size_t>(size);
            }
            if (originalShape.size() < 2 || originalShape.size() > 5 || originalCount != fileCount)
                throw std::runtime_error("Scientific image metadata does not match pixels");
            shape = std::move(originalShape);
        }
        // NIfTI and conventional scientific 4D images place time immediately after Z.
        if (dimensions == 4 && !ownMetadata) {
            shape.insert(shape.begin() + 3, 1);
            metadata.axes.insert(metadata.axes.begin() + 3, Axis{"C", 1, 0, ""});
        }
        // A 5D scientific image is X,Y,Z,T,C; model storage is X,Y,Z,C,T.
        if (dimensions == 5 && !ownMetadata) {
            std::swap(shape[3], shape[4]);
            std::swap(metadata.axes[3], metadata.axes[4]);
        }
        auto image = Image::allocate(type, shape, metadata);
        io->SetIORegion(region);
        detail::checkIo(stop, progress, 0.1);
        if (dimensions == 5 && !ownMetadata && image.channels() > 1 && image.frames() > 1) {
            std::vector<std::byte> source(image.byteCount());
            io->Read(source.data());
            auto *destination = image.mutableData();
            const auto volumeBytes = image.width() * image.height() * image.depth() * bytesPerPixel(type);
            for (std::size_t c = 0; c < image.channels(); ++c)
                for (std::size_t t = 0; t < image.frames(); ++t)
                    std::copy_n(source.data() + (t + image.frames() * c) * volumeBytes, volumeBytes,
                                destination + (c + image.channels() * t) * volumeBytes);
        } else io->Read(image.mutableData());
        image.metadata().sourcePath = QFileInfo(path).absoluteFilePath();
        if (image.metadata().name.isEmpty()) image.metadata().name = QFileInfo(path).fileName();
        detail::checkIo(stop, progress, 1);
        return image;
    } catch (const itk::ExceptionObject &error) {
        throw std::runtime_error(error.GetDescription());
    }
}

void writeScientific(const Image &input, const QString &path, std::stop_token stop, IoProgress progress)
{
    if (input.empty()) throw std::runtime_error("Cannot save an empty scientific image");
    if (input.shape().size() == 2 && input.metadata().direction.size() == 9)
        throw std::runtime_error("This 2D slice has a 3D spatial orientation; save the volume, or use TIFF or DICOM to preserve its plane");
    detail::checkIo(stop, progress, 0);
    try {
        const Image image = input.materialized();
        auto io = scientificAdapter(path);
        const auto parent = QFileInfo(path).absolutePath();
        QTemporaryDir staging(QDir(parent).filePath(".imagecxx-XXXXXX"));
        if (!staging.isValid()) throw std::runtime_error("Cannot create temporary image output");
        const auto suffix = path.toLower().endsWith(".nii.gz") ? QString("nii.gz") : QFileInfo(path).suffix();
        const auto stagedPath = staging.filePath("image." + suffix);
        io->SetFileName(QFile::encodeName(stagedPath).toStdString());
        auto shape = image.shape();
        auto axes = image.metadata().axes;
        const bool nifti = path.toLower().endsWith(".nii") || path.toLower().endsWith(".nii.gz");
        // MetaImage/NRRD retain our explicit axes in ImageCXX metadata. NIfTI uses its standard T,C axes.
        if (nifti && shape.size() > 3) {
            while (shape.size() < 5) shape.push_back(1);
            while (axes.size() < 5) axes.push_back({axes.size() == 3 ? "C" : "T", 1, 0, ""});
            std::swap(shape[3], shape[4]);
            std::swap(axes[3], axes[4]);
        }
        const std::size_t spatialDimensions = std::min<std::size_t>(3, image.shape().size());
        while (shape.size() > spatialDimensions && shape.back() == 1) shape.pop_back();
        const auto dimensions = static_cast<unsigned int>(shape.size());
        io->SetNumberOfDimensions(dimensions);
        itk::ImageIORegion region(dimensions);
        for (unsigned int axis = 0; axis < dimensions; ++axis) {
            io->SetDimensions(axis, shape[axis]);
            Axis calibration = axis < axes.size() ? axes[axis] : Axis{};
            if (nifti && axis < 4) {
                const auto unit = calibration.unit.trimmed().toLower();
                double factor = 1;
                if (axis < 3) {
                    factor = detail::spatialUnitToMillimeters(unit);
                } else {
                    if (unit == "s" || (shape[axis] == 1 && unit.isEmpty())) factor = 1;
                    else if (unit == "ms") factor = .001;
                    else if (unit == "us" || unit == QString::fromUtf8("µs") || unit == QString::fromUtf8("μs")) factor = .000001;
                    else if (unit == "min") factor = 60;
                    else throw std::runtime_error("NIfTI export requires time units: s, ms, us or min");
                }
                calibration.spacing *= factor;
                calibration.origin *= factor;
            }
            if (!std::isfinite(calibration.spacing) || calibration.spacing <= 0 || !std::isfinite(calibration.origin))
                throw std::runtime_error("Scientific image geometry is invalid");
            io->SetSpacing(axis, calibration.spacing);
            io->SetOrigin(axis, calibration.origin);
            std::vector<double> direction(dimensions, 0);
            direction[axis] = 1;
            const auto spatial = std::min<unsigned int>(dimensions, 3);
            if (axis < spatial && image.metadata().direction.size() == spatial * spatial)
                for (unsigned int row = 0; row < spatial; ++row) direction[row] = image.metadata().direction[row * spatial + axis];
            io->SetDirection(axis, direction);
            region.SetIndex(axis, 0);
            region.SetSize(axis, shape[axis]);
        }
        io->SetIORegion(region);
        const auto components = componentCount(image.pixelType());
        io->SetNumberOfComponents(static_cast<unsigned int>(components));
        io->SetPixelType(components == 3 ? itk::IOPixelEnum::RGB : components == 4 ? itk::IOPixelEnum::RGBA : itk::IOPixelEnum::SCALAR);
        switch (image.pixelType()) {
        case PixelType::UInt8: case PixelType::RGB8: case PixelType::RGBA8: io->SetComponentType(itk::IOComponentEnum::UCHAR); break;
        case PixelType::UInt16: io->SetComponentType(itk::IOComponentEnum::USHORT); break;
        case PixelType::Int16: io->SetComponentType(itk::IOComponentEnum::SHORT); break;
        case PixelType::Float32: io->SetComponentType(itk::IOComponentEnum::FLOAT); break;
        case PixelType::Float64: io->SetComponentType(itk::IOComponentEnum::DOUBLE); break;
        }
        auto &dictionary = io->GetMetaDataDictionary();
        for (auto it = image.metadata().attributes.cbegin(); it != image.metadata().attributes.cend(); ++it)
            itk::EncapsulateMetaData<std::string>(dictionary, it.key().toStdString(), it.value().toStdString());
        if (!nifti)
            itk::EncapsulateMetaData<std::string>(dictionary, "ImageCXX", QJsonDocument(detail::imageDescription(image)).toJson(QJsonDocument::Compact).toStdString());
        else {
            const auto scale = image.metadata().intensityScale, offset = image.metadata().intensityOffset;
            if (components > 1 && (scale != 1 || offset != 0))
                throw std::runtime_error("NIfTI color pixels require identity intensity calibration; use TIFF to retain color calibration");
            if (!std::isfinite(scale) || !std::isfinite(offset) || scale == 0 ||
                !std::isfinite(static_cast<float>(scale)) || !std::isfinite(static_cast<float>(offset)) || static_cast<float>(scale) == 0)
                throw std::runtime_error("NIfTI intensity calibration must fit finite nonzero float32 slope and float32 offset");
            auto *niftiIo = dynamic_cast<itk::NiftiImageIO *>(io.GetPointer());
            niftiIo->SetRescaleSlope(scale);
            niftiIo->SetRescaleIntercept(offset);
        }
        io->SetUseCompression(true);
        io->WriteImageInformation();
        detail::checkIo(stop, progress, 0.1);
        if (nifti && image.channels() > 1 && image.frames() > 1) {
            std::vector<std::byte> ordered(image.byteCount());
            const auto volumeBytes = image.width() * image.height() * image.depth() * bytesPerPixel(image.pixelType());
            for (std::size_t c = 0; c < image.channels(); ++c)
                for (std::size_t t = 0; t < image.frames(); ++t)
                    std::copy_n(image.data() + (c + image.channels() * t) * volumeBytes, volumeBytes,
                                ordered.data() + (t + image.frames() * c) * volumeBytes);
            io->Write(ordered.data());
        } else io->Write(image.data());
        detail::checkIo(stop, progress, 0.85);
        if (suffix.compare("mhd", Qt::CaseInsensitive) == 0 || suffix.compare("nhdr", Qt::CaseInsensitive) == 0) {
            QFile header(stagedPath);
            if (!header.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read scientific image header");
            auto text = QString::fromUtf8(header.readAll());
            const bool metaImage = suffix.compare("mhd", Qt::CaseInsensitive) == 0;
            const QRegularExpression expression(metaImage ? "(?m)^ElementDataFile\\s*=\\s*([^\\r\\n]+)" : "(?m)^data file:\\s*([^\\r\\n]+)");
            const auto match = expression.match(text);
            const auto payloadName = match.captured(1).trimmed();
            if (!match.hasMatch() || payloadName.isEmpty() || QFileInfo(payloadName).fileName() != payloadName)
                throw std::runtime_error("Unsupported detached scientific image layout");
            QTemporaryFile payload(QDir(parent).filePath(QFileInfo(path).completeBaseName() + ".imagecxx-XXXXXX." + QFileInfo(payloadName).completeSuffix()));
            if (!payload.open())
                throw std::runtime_error(QString("Cannot create scientific image payload '%1': %2").arg(payload.fileTemplate(), payload.errorString()).toStdString());
            const auto payloadPath = payload.fileName();
            if (payloadPath.isEmpty())
                throw std::runtime_error(QString("Cannot name scientific image payload '%1': %2").arg(payload.fileTemplate(), payload.errorString()).toStdString());
            // QTemporaryFile keeps its native handle even after close(). Replacing
            // that file with QSaveFile fails on Windows, so fill the unique payload
            // directly and publish only its header once all bytes are flushed.
            QFile source(staging.filePath(payloadName));
            if (!source.open(QIODevice::ReadOnly))
                throw std::runtime_error(QString("Cannot read scientific image payload '%1': %2").arg(source.fileName(), source.errorString()).toStdString());
            QByteArray buffer(4 * 1024 * 1024, Qt::Uninitialized);
            while (!source.atEnd()) {
                detail::checkIo(stop, {}, 0);
                const auto count = source.read(buffer.data(), buffer.size());
                if (count < 0)
                    throw std::runtime_error(QString("Cannot read scientific image payload '%1': %2").arg(source.fileName(), source.errorString()).toStdString());
                if (payload.write(buffer.constData(), count) != count)
                    throw std::runtime_error(QString("Cannot write scientific image payload '%1': %2").arg(payloadPath, payload.errorString()).toStdString());
            }
            detail::checkIo(stop, {}, 0);
            if (!payload.flush())
                throw std::runtime_error(QString("Cannot flush scientific image payload '%1': %2").arg(payloadPath, payload.errorString()).toStdString());
            payload.close();
            text.replace(match.capturedStart(1), match.capturedLength(1), QFileInfo(payloadPath).fileName());
            QSaveFile destination(path);
            const auto bytes = text.toUtf8();
            if (!destination.open(QIODevice::WriteOnly) || destination.write(bytes) != bytes.size())
                throw std::runtime_error(QString("Cannot write scientific image header '%1': %2").arg(path, destination.errorString()).toStdString());
            detail::checkIo(stop, {}, 0);
            if (!destination.commit())
                throw std::runtime_error(QString("Cannot commit scientific image header '%1': %2").arg(path, destination.errorString()).toStdString());
            payload.setAutoRemove(false);
        } else detail::atomicCopy(stagedPath, path, stop);
        if (progress) progress(1);
    } catch (const itk::ExceptionObject &error) {
        throw std::runtime_error(error.GetDescription());
    }
}

} // namespace imagecxx
