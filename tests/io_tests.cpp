#include "io/image_io.h"

#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QColor>
#include <QDir>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <tiffio.h>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

using namespace imagecxx;

namespace {
Image fixture(PixelType type, std::vector<std::size_t> shape = {5, 3, 2})
{
    ImageMetadata metadata;
    metadata.name = "Synthetic image";
    metadata.axes = {{"X", 0.25, 10, "mm"}, {"Y", 0.5, -2, "mm"}, {"Z", 1.5, 7, "mm"}, {"C", 1, 0, ""}, {"T", 2, 0, "s"}};
    metadata.axes.resize(shape.size());
    metadata.direction = {0, -1, 0, 1, 0, 0, 0, 0, 1};
    metadata.intensityScale = 2;
    metadata.intensityOffset = -100;
    metadata.intensityUnit = "HU";
    metadata.channels = {{"Signal", 0xff00ff00}};
    metadata.attributes.insert("Experiment", "Generated test fixture");
    auto image = Image::allocate(type, shape, metadata);
    std::vector<std::size_t> index(shape.size());
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        std::size_t offset = pixel;
        for (std::size_t axis = 0; axis < shape.size(); ++axis) { index[axis] = offset % shape[axis]; offset /= shape[axis]; }
        for (std::size_t component = 0; component < componentCount(type); ++component) {
            double value = static_cast<double>((pixel * 17 + component * 53) % 251);
            if (type == PixelType::UInt16) value = pixel * 1009 % 65536;
            if (type == PixelType::Int16) value = static_cast<double>(pixel * 503 % 32768) - 16000;
            if (type == PixelType::Float32 || type == PixelType::Float64) value = pixel * 0.125 - 1.75;
            image.setValue(index, value, component);
        }
    }
    return image;
}

void expectPixels(const Image &actual, const Image &expected)
{
    ASSERT_EQ(actual.pixelType(), expected.pixelType());
    ASSERT_EQ(actual.pixelCount(), expected.pixelCount());
    ASSERT_EQ(actual.byteCount(), expected.byteCount());
    EXPECT_EQ(std::memcmp(actual.data(), expected.data(), expected.byteCount()), 0);
}
} // namespace

TEST(ImageIo, TiffPreservesEveryPixelTypeAndFiveDimensionalMetadata)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    for (const auto type : {PixelType::UInt8, PixelType::UInt16, PixelType::Int16, PixelType::Float32,
                            PixelType::Float64, PixelType::RGB8, PixelType::RGBA8}) {
        const auto original = fixture(type, {5, 3, 2, 2, 2});
        const auto path = directory.filePath(pixelTypeName(type) + ".tif");
        writeImage(original, path);
        const auto loaded = readImage(path);
        expectPixels(loaded, original);
        EXPECT_EQ(loaded.shape(), original.shape());
        EXPECT_EQ(loaded.metadata().axes[0].spacing, 0.25);
        EXPECT_EQ(loaded.metadata().axes[2].origin, 7);
        EXPECT_EQ(loaded.metadata().direction, original.metadata().direction);
        EXPECT_DOUBLE_EQ(loaded.metadata().intensityScale, 2);
        EXPECT_DOUBLE_EQ(loaded.metadata().intensityOffset, -100);
        EXPECT_EQ(loaded.metadata().intensityOffset, -100);
        EXPECT_EQ(loaded.metadata().channels[0].name, "Signal");
        EXPECT_EQ(loaded.metadata().attributes.value("Experiment"), "Generated test fixture");
    }
}

TEST(ImageIo, RawRoundTripMapsAndPreservesSidecarMetadata)
{
    QTemporaryDir directory;
    const auto original = fixture(PixelType::Float64);
    const auto path = directory.filePath("image.raw");
    writeImage(original, path);
    const auto loaded = readImage(path);
    expectPixels(loaded, original);
    EXPECT_EQ(loaded.storageKind(), "Memory mapped");
    EXPECT_EQ(loaded.metadata().intensityScale, 2);
    EXPECT_EQ(loaded.metadata().direction, original.metadata().direction);
}

TEST(ImageIo, RawImportSwapsEndianSkipsHeaderAndRejectsTruncation)
{
    QTemporaryDir directory;
    const auto path = directory.filePath("big-endian.bin");
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    const unsigned char bytes[]{'H', 'D', 0, 1, 1, 0, 0xff, 0xfe, 0x12, 0x34};
    ASSERT_EQ(file.write(reinterpret_cast<const char *>(bytes), sizeof(bytes)), static_cast<qint64>(sizeof(bytes)));
    file.close();
    RawImportOptions options;
    options.pixelType = PixelType::UInt16;
    options.shape = {2, 2};
    options.headerBytes = 2;
    options.byteOrder = ByteOrder::BigEndian;
    const auto image = readImage(path, options);
    EXPECT_EQ(image.sample(0, 0), 1);
    EXPECT_EQ(image.sample(1, 0), 256);
    EXPECT_EQ(image.sample(0, 1), 65534);
    EXPECT_EQ(image.sample(1, 1), 0x1234);
    if (std::endian::native == std::endian::little) EXPECT_EQ(image.storageKind(), "RAM");
    options.shape = {3, 2};
    EXPECT_THROW(readImage(path, options), std::exception);
    options.byteOrder = ByteOrder::Native;
    EXPECT_THROW(readImage(path, options), std::exception);
}

TEST(ImageIo, PngPreservesGray16AndRgbaWithoutPaddingBytes)
{
    QTemporaryDir directory;
    for (const auto type : {PixelType::UInt8, PixelType::UInt16, PixelType::RGB8, PixelType::RGBA8}) {
        const auto original = fixture(type, {5, 3});
        const auto path = directory.filePath(pixelTypeName(type) + ".png");
        writeImage(original, path);
        const auto loaded = readImage(path);
        expectPixels(loaded, original);
        EXPECT_EQ(loaded.metadata().axes[0].spacing, 0.25);
        EXPECT_EQ(loaded.metadata().intensityOffset, -100);
    }
}

TEST(ImageIo, JpegLoadsAndUnsupportedRasterConversionsFail)
{
    QTemporaryDir directory;
    auto image = Image::allocate(PixelType::RGB8, {16, 16});
    auto *bytes = reinterpret_cast<unsigned char *>(image.mutableData());
    for (std::size_t i = 0; i < image.byteCount(); i += 3) { bytes[i] = 160; bytes[i + 1] = 80; bytes[i + 2] = 40; }
    const auto path = directory.filePath("image.jpg");
    writeImage(image, path);
    const auto loaded = readImage(path);
    ASSERT_EQ(loaded.pixelType(), PixelType::RGB8);
    EXPECT_NEAR(loaded.sample(5, 5, 0, 0, 0, 0), 160, 3);
    EXPECT_NEAR(loaded.sample(5, 5, 0, 0, 0, 1), 80, 3);
    EXPECT_THROW(writeImage(fixture(PixelType::Float32, {5, 3}), directory.filePath("float.png")), std::runtime_error);
    EXPECT_THROW(writeImage(fixture(PixelType::UInt16, {5, 3}), directory.filePath("gray16.jpg")), std::runtime_error);
    EXPECT_THROW(writeImage(fixture(PixelType::UInt8), directory.filePath("volume.png")), std::runtime_error);
}

TEST(ImageIo, ScientificFormatsPreserveNumericVolumeAndGeometry)
{
    QTemporaryDir directory;
    for (const auto extension : {"mha", "mhd", "nii", "nii.gz", "nrrd"}) {
        SCOPED_TRACE(extension);
        const auto original = fixture(PixelType::Int16);
        const auto path = directory.filePath(QString("image.") + extension);
        writeImage(original, path);
        const auto loaded = readImage(path);
        expectPixels(loaded, original);
        EXPECT_EQ(loaded.shape(), original.shape());
        EXPECT_NEAR(loaded.metadata().axes[0].spacing, 0.25, 1e-6);
        EXPECT_NEAR(loaded.metadata().axes[1].spacing, 0.5, 1e-6);
        EXPECT_NEAR(loaded.metadata().axes[2].spacing, 1.5, 1e-6);
        EXPECT_NEAR(loaded.metadata().axes[0].origin, 10, 1e-6);
        EXPECT_EQ(loaded.metadata().direction, original.metadata().direction);
    }
}

TEST(ImageIo, MetaImagePreservesCalibrationAndAxes)
{
    QTemporaryDir directory;
    const auto original = fixture(PixelType::Float32, {5, 3, 2, 2, 1});
    const auto path = directory.filePath("image.mha");
    writeImage(original, path);
    const auto loaded = readImage(path);
    expectPixels(loaded, original);
    EXPECT_EQ(loaded.shape(), original.shape());
    EXPECT_EQ(loaded.metadata().intensityScale, 2);
    EXPECT_EQ(loaded.metadata().intensityOffset, -100);
    EXPECT_EQ(loaded.metadata().axes[3].name, "C");
}

TEST(ImageIo, DicomPreservesSignedValuesCalibrationAndPatientGeometry)
{
    QTemporaryDir directory;
    const auto original = fixture(PixelType::Int16, {5, 3, 1});
    const auto path = directory.filePath("image.dcm");
    writeImage(original, path);
    const auto loaded = readImage(path);
    expectPixels(loaded, original);
    EXPECT_EQ(loaded.metadata().axes[0].spacing, 0.25);
    EXPECT_EQ(loaded.metadata().axes[1].spacing, 0.5);
    EXPECT_EQ(loaded.metadata().axes[0].origin, 10);
    EXPECT_EQ(loaded.metadata().axes[2].origin, 7);
    EXPECT_EQ(loaded.metadata().direction, original.metadata().direction);
    EXPECT_EQ(loaded.metadata().intensityScale, 2);
    EXPECT_EQ(loaded.metadata().intensityOffset, -100);
}

TEST(ImageIo, DicomSeriesOrdersPhysicalPositionsAndRejectsMixedSeries)
{
    QTemporaryDir directory;
    QStringList paths;
    for (int z : {2, 0, 1}) {
        auto slice = fixture(PixelType::UInt16, {5, 3, 1});
        slice.metadata().axes[2].origin = 7 + 1.5 * z;
        slice.metadata().attributes.insert("DICOM.0020,000e", "1.2.826.0.1.3680043.10.543.1");
        slice.metadata().attributes.insert("DICOM.0020,000d", "1.2.826.0.1.3680043.10.543.10");
        slice.setValue(std::array<std::size_t, 3>{0, 0, 0}, z * 100);
        const auto path = directory.filePath(QString("slice-%1.dcm").arg(z));
        writeImage(slice, path);
        paths << path;
    }
    const auto image = readDicomSeries(paths);
    EXPECT_EQ(image.depth(), 3);
    EXPECT_EQ(image.sample(0, 0, 0), 0);
    EXPECT_EQ(image.sample(0, 0, 1), 100);
    EXPECT_EQ(image.sample(0, 0, 2), 200);
    EXPECT_EQ(image.metadata().axes[2].spacing, 1.5);
    auto different = fixture(PixelType::UInt16, {5, 3});
    const auto path = directory.filePath("other-series.dcm");
    writeImage(different, path);
    paths << path;
    EXPECT_THROW(readDicomSeries(paths), std::runtime_error);
    EXPECT_THROW(readDicomSeries({paths[0], paths[0]}), std::runtime_error);
}

TEST(ImageIo, ImageJHyperstackUsesChannelFastestPageOrder)
{
    QTemporaryDir directory;
    const auto path = directory.filePath("imagej.tif");
    TIFF *tiff = TIFFOpen(path.toLocal8Bit().constData(), "w");
    ASSERT_NE(tiff, nullptr);
    for (int page = 0; page < 8; ++page) {
        TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, 2);
        TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, 2);
        TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, 16);
        TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, 1);
        TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
        TIFFSetField(tiff, TIFFTAG_ROWSPERSTRIP, 2);
        TIFFSetField(tiff, TIFFTAG_IMAGEDESCRIPTION, "ImageJ=1.54\nimages=8\nchannels=2\nslices=2\nframes=2\nunit=um\nspacing=2.5\n");
        const std::array<std::uint16_t, 2> row{static_cast<std::uint16_t>(page * 10), static_cast<std::uint16_t>(page * 10 + 1)};
        for (int y = 0; y < 2; ++y) ASSERT_GE(TIFFWriteScanline(tiff, const_cast<std::uint16_t *>(row.data()), y, 0), 0);
        ASSERT_TRUE(TIFFWriteDirectory(tiff));
    }
    TIFFClose(tiff);
    const auto image = readImage(path);
    EXPECT_EQ(image.shape(), (std::vector<std::size_t>{2, 2, 2, 2, 2}));
    EXPECT_EQ(image.sample(0, 0, 0, 1, 0), 10);
    EXPECT_EQ(image.sample(0, 0, 1, 0, 0), 20);
    EXPECT_EQ(image.sample(0, 0, 1, 1, 1), 70);
    EXPECT_EQ(image.metadata().axes[2].spacing, 2.5);
}

TEST(ImageIo, CancellationAndCorruptFilesReportErrors)
{
    QTemporaryDir directory;
    std::stop_source cancellation;
    cancellation.request_stop();
    EXPECT_THROW(readImage(directory.filePath("missing.tif"), {}, cancellation.get_token()), std::runtime_error);
    EXPECT_THROW(writeImage(fixture(PixelType::UInt8), directory.filePath("cancel.tif"), cancellation.get_token()), std::runtime_error);
    const auto path = directory.filePath("corrupt.png");
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("This is not an image");
    file.close();
    EXPECT_THROW(readImage(path), std::runtime_error);
}

TEST(ImageIo, CancelledTiffExportPreservesThePreviousFile)
{
    QTemporaryDir directory;
    const auto path = directory.filePath("existing.tif");
    const auto original = fixture(PixelType::UInt8, {5, 3, 2});
    writeImage(original, path);
    std::stop_source cancellation;
    const auto replacement = fixture(PixelType::Float32, {5, 3, 4});
    EXPECT_THROW(writeImage(replacement, path, cancellation.get_token(), [&cancellation](double progress) {
        if (progress > 0.1) cancellation.request_stop();
    }), std::runtime_error);
    expectPixels(readImage(path), original);
}

TEST(ImageIo, HighBitDepthColorIsRejectedWithoutSilentConversion)
{
    QTemporaryDir directory;
    const auto path = directory.filePath("color16.png");
    QImage image(2, 2, QImage::Format_RGBA64);
    image.fill(QColor::fromRgba64(65535, 12345, 54321, 65535));
    ASSERT_TRUE(image.save(path));
    EXPECT_THROW(readImage(path), std::runtime_error);
}

TEST(ImageIo, CancelledExportsPreserveExistingScientificRasterAndDicomFiles)
{
    QTemporaryDir directory;
    for (const auto extension : {"png", "jpg", "mha", "mhd", "nii", "nii.gz", "nrrd", "nhdr", "dcm"}) {
        SCOPED_TRACE(extension);
        const auto path = directory.filePath(QString("existing.") + extension);
        const bool twoDimensional = QString(extension) == "png" || QString(extension) == "jpg" || QString(extension) == "dcm";
        const auto original = fixture(PixelType::UInt8, twoDimensional ? std::vector<std::size_t>{5, 3} : std::vector<std::size_t>{5, 3, 2});
        writeImage(original, path);
        QFile beforeFile(path);
        ASSERT_TRUE(beforeFile.open(QIODevice::ReadOnly));
        const auto before = beforeFile.readAll();
        beforeFile.close();
        const auto baseline = readImage(path);
        std::stop_source cancellation;
        EXPECT_THROW(writeImage(original, path, cancellation.get_token(), [&cancellation](double progress) {
            if (progress > .8) cancellation.request_stop();
        }), std::runtime_error);
        QFile afterFile(path);
        ASSERT_TRUE(afterFile.open(QIODevice::ReadOnly));
        EXPECT_EQ(afterFile.readAll(), before);
        expectPixels(readImage(path), baseline);
    }
}

TEST(ImageIo, RawSaveRefusesToOverwriteEitherPartAndCleansCancelledFreshPair)
{
    QTemporaryDir directory;
    const auto path = directory.filePath("original.raw");
    const auto original = fixture(PixelType::UInt16);
    writeImage(original, path);
    QFile sidecar(path + ".json");
    ASSERT_TRUE(sidecar.open(QIODevice::ReadOnly));
    const auto metadata = sidecar.readAll();
    sidecar.close();
    EXPECT_THROW(writeImage(fixture(PixelType::Float64), path), std::runtime_error);
    expectPixels(readImage(path), original);
    ASSERT_TRUE(sidecar.open(QIODevice::ReadOnly));
    EXPECT_EQ(sidecar.readAll(), metadata);
    sidecar.close();
    ASSERT_TRUE(QFile::remove(path));
    EXPECT_THROW(writeImage(original, path), std::runtime_error);
    ASSERT_TRUE(sidecar.open(QIODevice::ReadOnly));
    EXPECT_EQ(sidecar.readAll(), metadata);
    const auto cancelled = directory.filePath("cancelled.raw");
    std::stop_source cancellation;
    EXPECT_THROW(writeImage(original, cancelled, cancellation.get_token(), [&cancellation](double progress) {
        if (progress > .8) cancellation.request_stop();
    }), std::runtime_error);
    EXPECT_FALSE(QFile::exists(cancelled));
    EXPECT_FALSE(QFile::exists(cancelled + ".json"));
}

TEST(ImageIo, DetachedScientificReplacementRetainsReferencedPayloads)
{
    QTemporaryDir directory;
    for (const auto extension : {"mhd", "nhdr"}) {
        SCOPED_TRACE(extension);
        const auto path = directory.filePath(QString("image.") + extension);
        const auto backup = directory.filePath(QString("previous.") + extension);
        const auto original = fixture(PixelType::UInt16);
        writeImage(original, path);
        ASSERT_TRUE(QFile::copy(path, backup));
        const auto replacement = fixture(PixelType::Float64);
        writeImage(replacement, path);
        expectPixels(readImage(path), replacement);
        expectPixels(readImage(backup), original);
        const auto before = QDir(directory.path()).entryList(QDir::Files);
        std::stop_source cancellation;
        EXPECT_THROW(writeImage(original, path, cancellation.get_token(), [&cancellation](double progress) {
            if (progress > .8) cancellation.request_stop();
        }), std::runtime_error);
        EXPECT_EQ(QDir(directory.path()).entryList(QDir::Files), before);
    }
}

TEST(ImageIo, DetachedScientificExportsCopyMultiplePayloadChunks)
{
    auto image = Image::allocate(PixelType::UInt8, {1024, 1024, 5});
    auto *pixels = image.mutableData();
    // Incompressible bytes keep the on-disk payload larger than a copy chunk.
    std::uint32_t random = 123456789;
    for (std::size_t i = 0; i < image.byteCount(); ++i) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        pixels[i] = static_cast<std::byte>(random & 0xff);
    }
    for (const auto extension : {"mhd", "nhdr"}) {
        SCOPED_TRACE(extension);
        QTemporaryDir directory;
        ASSERT_TRUE(directory.isValid());
        const auto path = directory.filePath(QString("image.") + extension);
        ASSERT_NO_THROW(writeImage(image, path));
        const auto payloads = QDir(directory.path()).entryList({"*.imagecxx-*"}, QDir::Files);
        ASSERT_EQ(payloads.size(), 1);
        EXPECT_GT(QFileInfo(directory.filePath(payloads.front())).size(), 4 * 1024 * 1024);
        expectPixels(readImage(path), image);
    }
}

TEST(ImageIo, DetachedScientificFailuresCleanUnpublishedPayloads)
{
    const auto image = fixture(PixelType::UInt16);
    for (const auto extension : {"mhd", "nhdr"}) {
        SCOPED_TRACE(extension);
        QTemporaryDir directory;
        ASSERT_TRUE(directory.isValid());
        QDir parent(directory.path());
        const auto path = directory.filePath(QString("image.") + extension);
        std::stop_source cancellation;
        EXPECT_THROW(writeImage(image, path, cancellation.get_token(), [&cancellation](double progress) {
            if (progress > .8) cancellation.request_stop();
        }), std::runtime_error);
        EXPECT_TRUE(parent.entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());

        // A directory at the header path rejects publication after payload copy.
        ASSERT_TRUE(parent.mkdir(QFileInfo(path).fileName()));
        QFile sentinel(QDir(path).filePath("keep.txt"));
        ASSERT_TRUE(sentinel.open(QIODevice::WriteOnly));
        ASSERT_EQ(sentinel.write("keep"), 4);
        sentinel.close();
        const auto before = parent.entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
        try {
            writeImage(image, path);
            FAIL() << "Expected scientific header publication to fail";
        } catch (const std::runtime_error &error) {
            const auto message = QString::fromUtf8(error.what());
            EXPECT_TRUE(message.contains("Cannot write scientific image header")) << error.what();
            EXPECT_TRUE(message.contains(path)) << error.what();
        }
        EXPECT_EQ(parent.entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot), before);
        ASSERT_TRUE(sentinel.open(QIODevice::ReadOnly));
        EXPECT_EQ(sentinel.readAll(), "keep");
    }
}

TEST(ImageIo, ObliqueSliceRetainsPatientPlaneThroughDicomAndTiff)
{
    QTemporaryDir directory;
    auto volume = fixture(PixelType::Int16);
    volume.metadata().direction = {1, 0, 0, 0, 0, -1, 0, 1, 0};
    const auto slice = volume.slice(1);
    ASSERT_TRUE(slice.metadata().attributes.contains("ImageCXX.EmbeddedPlaneOrigin"));
    const auto dicomPath = directory.filePath("slice.dcm");
    writeImage(slice, dicomPath);
    const auto dicom = readImage(dicomPath);
    expectPixels(dicom, slice.clone());
    EXPECT_EQ(dicom.metadata().direction, volume.metadata().direction);
    EXPECT_DOUBLE_EQ(dicom.metadata().axes[0].origin, 10);
    EXPECT_DOUBLE_EQ(dicom.metadata().axes[1].origin, -3.5);
    EXPECT_DOUBLE_EQ(dicom.metadata().axes[2].origin, 7);
    const auto tiffPath = directory.filePath("slice.tif");
    writeImage(slice, tiffPath);
    const auto tiff = readImage(tiffPath);
    expectPixels(tiff, slice.clone());
    EXPECT_EQ(tiff.metadata().direction, slice.metadata().direction);
    EXPECT_EQ(tiff.metadata().attributes.value("ImageCXX.EmbeddedPlaneOrigin"), slice.metadata().attributes.value("ImageCXX.EmbeddedPlaneOrigin"));
    const auto scientificPath = directory.filePath("slice.mha");
    EXPECT_THROW(writeImage(slice, scientificPath), std::runtime_error);
    EXPECT_FALSE(QFile::exists(scientificPath));
}

TEST(ImageIo, NiftiPreservesStoredTypesAndStandardIntensityCalibration)
{
    QTemporaryDir directory;
    for (const auto type : {PixelType::UInt8, PixelType::UInt16, PixelType::Int16, PixelType::Float32, PixelType::Float64}) {
        for (const auto extension : {"nii", "nii.gz"}) {
            const auto original = fixture(type);
            const auto path = directory.filePath(pixelTypeName(type) + "." + extension);
            writeImage(original, path);
            const auto loaded = readImage(path);
            expectPixels(loaded, original);
            EXPECT_DOUBLE_EQ(loaded.metadata().intensityScale, 2);
            EXPECT_DOUBLE_EQ(loaded.metadata().intensityOffset, -100);
            EXPECT_DOUBLE_EQ(loaded.sample(1, 1) * loaded.metadata().intensityScale + loaded.metadata().intensityOffset,
                             original.sample(1, 1) * original.metadata().intensityScale + original.metadata().intensityOffset);
        }
    }
}

TEST(ImageIo, NiftiConvertsSpatialAndTimeUnitsWithoutChangingPixels)
{
    QTemporaryDir directory;
    auto original = fixture(PixelType::UInt16, {5, 3, 2, 1, 2});
    for (std::size_t axis = 0; axis < 3; ++axis) original.metadata().axes[axis].unit = QString::fromUtf8("µm");
    original.metadata().axes[4].unit = "ms";
    const auto path = directory.filePath("microscopy.nii.gz");
    writeImage(original, path);
    const auto loaded = readImage(path);
    expectPixels(loaded, original);
    EXPECT_EQ(loaded.shape(), original.shape());
    EXPECT_EQ(loaded.metadata().axes[0].unit, "mm");
    EXPECT_NEAR(loaded.metadata().axes[0].spacing, .00025, 1e-10);
    EXPECT_NEAR(loaded.metadata().axes[1].spacing, .0005, 1e-10);
    EXPECT_NEAR(loaded.metadata().axes[2].spacing, .0015, 1e-9);
    EXPECT_NEAR(loaded.metadata().axes[0].origin, .01, 1e-9);
    EXPECT_EQ(loaded.metadata().axes[4].unit, "s");
    EXPECT_NEAR(loaded.metadata().axes[4].spacing, .002, 1e-9);
    auto invalid = original;
    invalid.metadata().axes[0].unit = "px";
    EXPECT_THROW(writeImage(invalid, path), std::runtime_error);
    expectPixels(readImage(path), loaded);
    invalid = original;
    invalid.metadata().intensityScale = 0;
    EXPECT_THROW(writeImage(invalid, directory.filePath("zero-slope.nii")), std::runtime_error);
    EXPECT_FALSE(QFile::exists(directory.filePath("zero-slope.nii")));
    auto color = fixture(PixelType::RGB8);
    EXPECT_THROW(writeImage(color, directory.filePath("calibrated-color.nii")), std::runtime_error);
    color.metadata().intensityScale = 1;
    color.metadata().intensityOffset = 0;
    const auto colorPath = directory.filePath("color.nii");
    writeImage(color, colorPath);
    expectPixels(readImage(colorPath), color);
}

TEST(ImageIo, DicomReexportPreservesMonochromeOneAndStoredSamples)
{
    QTemporaryDir directory;
    auto original = fixture(PixelType::UInt16, {5, 3, 1});
    original.metadata().attributes.insert("DICOM.0028,0004", "MONOCHROME1");
    const auto firstPath = directory.filePath("first.dcm");
    writeImage(original, firstPath);
    const auto first = readImage(firstPath);
    EXPECT_EQ(first.metadata().attributes.value("DICOM.0028,0004"), "MONOCHROME1");
    expectPixels(first, original);
    const auto secondPath = directory.filePath("second.dcm");
    writeImage(first, secondPath);
    const auto second = readImage(secondPath);
    EXPECT_EQ(second.metadata().attributes.value("DICOM.0028,0004"), "MONOCHROME1");
    expectPixels(second, original);
}
