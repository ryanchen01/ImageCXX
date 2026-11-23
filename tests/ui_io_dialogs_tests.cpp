#include "tests/desktop_test_helpers.h"
#include "core/memory.h"
#include "io/image_io.h"
#include "mainwindow.h"
#include "ui/theme.h"
#include <gtest/gtest.h>
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QImage>
#include <QInputDialog>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLineSeries>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPrintPreviewDialog>
#include <QPrintPreviewWidget>
#include <QPrinter>
#include <QPushButton>
#include <QQuickItem>
#include <QQuickWidget>
#include <QScatterSeries>
#include <QScreen>
#include <QSet>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTranslator>
#include <QTreeWidget>
#include <QUrl>
#include <QValueAxis>
#include <QWindow>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <dcmtk/dcmdata/dctk.h>

using namespace imagecxx;
using namespace imagecxx::test;

namespace {
QString saveImage(MainWindow &window, const QString &path, bool includeFullMetadata = false)
{
    QString error;
    bool selected = false;
    QTimer picker;
    QObject::connect(&picker, &QTimer::timeout, &window, [&] {
        auto *modal = QApplication::activeModalWidget();
        if (auto *dialog = qobject_cast<QFileDialog *>(modal)) {
            if (selected) return;
            selected = true;
            auto *checkbox = dialog->findChild<QCheckBox *>("includeFullMetadata");
            EXPECT_NE(checkbox, nullptr);
            if (checkbox) {
                EXPECT_FALSE(checkbox->isChecked());
                checkbox->setChecked(includeFullMetadata);
            }
            dialog->setDirectory(QFileInfo(path).absolutePath());
            dialog->selectFile(QFileInfo(path).fileName());
            if (auto *name = dialog->findChild<QLineEdit *>("fileNameEdit")) name->setText(QFileInfo(path).fileName());
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        } else if (auto *message = qobject_cast<QMessageBox *>(modal)) {
            if (message->standardButtons().testFlag(QMessageBox::Yes)) message->button(QMessageBox::Yes)->click();
            else { error = message->text(); message->button(QMessageBox::Ok)->click(); }
        }
    });
    picker.start(1);
    window.commands().execute("file.save");
    return error;
}

QStringList writeImportSeries(const QString &directory, int series)
{
    QStringList paths;
    for (int z : {1, 0}) {
        ImageMetadata metadata;
        metadata.axes = {{"X", 1, 0, "mm"}, {"Y", 1, 0, "mm"}, {"Z", 2, double(z * 2), "mm"}};
        metadata.attributes.insert("DICOM.0020,000d", "1.2.826.0.1.3680043.10.543.10");
        metadata.attributes.insert("DICOM.0020,000e", QString("1.2.826.0.1.3680043.10.543.%1").arg(series));
        auto slice = Image::allocate(PixelType::UInt16, {4, 4, 1}, metadata);
        slice.setValue(std::array<std::size_t, 3>{0, 0, 0}, series * 100 + z);
        const auto path = QDir(directory).filePath(QString("series-%1-%2.dcm").arg(series).arg(z));
        writeDicom(slice, path);
        paths.append(path);
    }
    return paths;
}

void dropPaths(QWidget *target, const QStringList &paths)
{
    QMimeData mime;
    QList<QUrl> urls;
    for (const auto &path : paths) urls.append(QUrl::fromLocalFile(path));
    mime.setUrls(urls);
    QDragEnterEvent enter(QPointF(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &enter);
    EXPECT_TRUE(enter.isAccepted());
    QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &drop);
    EXPECT_TRUE(drop.isAccepted());
}
} // namespace

TEST(Desktop, OverlappingSavesAreRejectedAndReservationsReleaseAfterCancellation)
{
    for (bool cancel : {false, true}) {
        QTemporaryDir directory;
#ifdef Q_OS_WIN
        const QString fileName = "saved.tif";
#else
        const QString fileName = QString(230, 'x') + ".tif"; // Lock filenames must also fit the directory entry limit.
#endif
        const auto path = directory.filePath(fileName);
        std::atomic<bool> release{false};
        MainWindow window;
        for (int i = 0; i < 2; ++i) window.jobs().submit("Hold worker", [&](auto stop, auto) {
            while (!release.load() && !stop.stop_requested()) QThread::msleep(1);
        }, {});
        auto original = Image::allocate(PixelType::UInt16, {4, 4});
        original.mutableData()[0] = std::byte{11};
        auto *doc = window.addDocument(original);
        EXPECT_TRUE(saveImage(window, path, true).isEmpty());
        auto newer = Image::allocate(PixelType::UInt16, {2, 2});
        newer.mutableData()[0] = std::byte{22};
        doc->replaceImage(newer, "New revision");
        EXPECT_TRUE(saveImage(window, directory.filePath("./" + fileName), true).contains("still being saved"));
        if (cancel) window.jobs().cancelAll(); else release.store(true);
        ASSERT_TRUE(finishJobs(window));
        EXPECT_TRUE(doc->modified);
        if (cancel) EXPECT_FALSE(QFileInfo::exists(path));
        else EXPECT_DOUBLE_EQ(readImage(path).sample(0, 0), 11);
        EXPECT_TRUE(saveImage(window, path, true).isEmpty());
        ASSERT_TRUE(finishJobs(window));
        EXPECT_FALSE(doc->modified);
        const auto saved = readImage(path);
        EXPECT_EQ(saved.shape(), newer.shape());
        EXPECT_DOUBLE_EQ(saved.sample(0, 0), 22);
    }
}

TEST(Desktop, DicomSaveRetainsNestedMetadataAfterDocumentIsMarkedClean)
{
    QTemporaryDir directory;
    ImageMetadata metadata;
    metadata.axes = {{"X", 1, 0, "mm"}, {"Y", 1, 0, "mm"}};
    metadata.attributes.insert("Experiment", "Preserve this too");
    metadata.dicomSequences = {{"DICOM.0008,1115", QJsonArray{QJsonObject{
        {"DICOM.0008,1140", QJsonArray{QJsonObject{{"DICOM.0008,1155", "1.2.3"}}}}}}}};
    MainWindow window;
    auto *document = window.addDocument(Image::allocate(PixelType::UInt16, {4, 4}, metadata));
    document->modified = true;
    const auto path = directory.filePath("saved.dcm");
    ASSERT_TRUE(saveImage(window, path, true).isEmpty());
    ASSERT_TRUE(finishJobs(window));
    EXPECT_FALSE(document->modified);
    EXPECT_EQ(document->image.metadata().sourcePath, path);
    const auto saved = readImage(path);
    EXPECT_EQ(saved.metadata().dicomSequences, metadata.dicomSequences);
    EXPECT_EQ(saved.metadata().attributes.value("Experiment"), "Preserve this too");
}

TEST(Desktop, SaveImageOmitsPatientMetadataByDefault)
{
    QTemporaryDir directory;
    ImageMetadata metadata;
    metadata.name = "Patient Name";
    metadata.axes = {{"X", 1, 0, "mm"}, {"Y", 1, 0, "mm"}};
    metadata.attributes.insert("DICOM.0010,0010", "Patient Name");
    MainWindow window;
    auto *document = window.addDocument(Image::allocate(PixelType::UInt16, {4, 4}, metadata));
    document->modified = true;
    const auto path = directory.filePath("safe.tif");
    ASSERT_TRUE(saveImage(window, path).isEmpty());
    ASSERT_TRUE(finishJobs(window));
    const auto saved = readImage(path);
    EXPECT_FALSE(saved.metadata().attributes.contains("DICOM.0010,0010"));
    EXPECT_NE(saved.metadata().name, metadata.name);
    EXPECT_TRUE(document->modified);
}

TEST(Desktop, WorkerOpensImageAndPublishesOnUiThread)
{
    QTemporaryDir directory;
    auto image = Image::allocate(PixelType::Float32, {12, 10, 3});
    reinterpret_cast<float *>(image.mutableData())[2 * 12 * 10 + 7 * 12 + 4] = -17.25f;
    writeImageWithMetadata(image, directory.filePath("input.tif"));
    MainWindow window; window.openPaths({directory.filePath("input.tif")});
    QElapsedTimer timer; timer.start();
    while ((window.jobs().busy() || !window.activeDocument()) && timer.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(2); }
    ASSERT_NE(window.activeDocument(), nullptr);
    EXPECT_EQ(window.activeDocument()->image.pixelType(), PixelType::Float32);
    EXPECT_EQ(window.activeDocument()->image.depth(), 3u);
    EXPECT_FLOAT_EQ(window.activeDocument()->image.sample(4, 7, 2), -17.25f);
    EXPECT_EQ(window.activeDocument()->thread(), QCoreApplication::instance()->thread());
}

TEST(Desktop, ImportGroupsOneSeriesFromSelectedFilesOrDroppedFolder)
{
    QTemporaryDir directory;
    const auto paths = writeImportSeries(directory.path(), 1);
    for (bool folder : {false, true}) {
        MainWindow window; window.show();
        if (folder) dropPaths(&window, {directory.path()});
        else window.openPaths(paths);
        ASSERT_TRUE(finishJobs(window));
        ASSERT_EQ(window.documents().size(), 1);
        const auto &image = window.activeDocument()->image;
        EXPECT_EQ(image.depth(), 2);
        EXPECT_EQ(image.sample(0, 0, 0), 100);
        EXPECT_EQ(image.sample(0, 0, 1), 101);
        EXPECT_EQ(image.metadata().axes[2].spacing, 2);
        EXPECT_TRUE(image.metadata().sourcePath.isEmpty());
    }
}

TEST(Desktop, ImportMultipleSeriesAndOrdinaryImagesThroughLauncherAndImageWindows)
{
    QTemporaryDir directory;
    ASSERT_TRUE(QDir(directory.path()).mkpath("nested"));
    auto paths = writeImportSeries(directory.path(), 1);
    paths.append(writeImportSeries(directory.filePath("nested"), 2));
    const auto raster = directory.filePath("ordinary.tif");
    writeImageWithMetadata(Image::allocate(PixelType::UInt8, {4, 4}), raster);
    paths.append(raster);
    for (int mode : {0, 1, 2}) {
        MainWindow window; window.show();
        if (mode == 0) dropPaths(&window, paths);
        else if (mode == 1) dropPaths(&window, {directory.path(), paths.front()});
        else {
            auto *existing = window.addDocument(Image::allocate(PixelType::UInt8, {4, 4}));
            dropPaths(existing, {directory.path()});
        }
        ASSERT_TRUE(finishJobs(window));
        ASSERT_EQ(window.documents().size(), mode == 2 ? 4 : 3);
        QSet<int> values;
        int stackCount = 0;
        for (const auto *document : window.documents()) if (document->image.depth() == 2) {
            ++stackCount;
            values.insert(int(document->image.sample(0, 0, 0)));
        }
        EXPECT_EQ(stackCount, 2);
        EXPECT_EQ(values, (QSet<int>{100, 200}));
    }
}

TEST(Desktop, InvalidDicomSeriesDoesNotBlockOtherImports)
{
    QTemporaryDir directory;
    auto paths = writeImportSeries(directory.path(), 1);
    // A different file at the same physical position must still be rejected.
    const auto duplicate = directory.filePath("duplicate.dcm");
    ASSERT_TRUE(QFile::copy(paths.back(), duplicate));
    paths.append(duplicate);
    paths.append(writeImportSeries(directory.path(), 2));
    const auto raster = directory.filePath("ordinary.tif");
    writeImageWithMetadata(Image::allocate(PixelType::UInt8, {4, 4}), raster);
    paths.append(raster);
    MainWindow window;
    QStringList errors;
    QTimer dismiss;
    QObject::connect(&dismiss, &QTimer::timeout, &window, [&] {
        if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            errors.append(message->text()); message->accept();
        }
    });
    dismiss.start(1);
    window.openPaths(paths);
    ASSERT_TRUE(finishJobs(window));
    ASSERT_EQ(window.documents().size(), 2);
    ASSERT_EQ(errors.size(), 1);
    EXPECT_TRUE(errors.front().contains("duplicate slice positions"));
    EXPECT_TRUE(errors.front().contains(paths.front()));
}

TEST(Desktop, HoverShowsRescaledDicomIntensity)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath("ct.dcm");
    ImageMetadata metadata;
    metadata.axes = {{"X", 1, 0, "mm"}, {"Y", 1, 0, "mm"}};
    metadata.intensityScale = 1.5;
    metadata.intensityOffset = -1024;
    metadata.intensityUnit = "HU";
    auto image = Image::allocate(PixelType::UInt16, {4, 4}, metadata);
    image.setValue(std::array<std::size_t, 2>{1, 2}, 201);
    image.setValue(std::array<std::size_t, 2>{2, 2}, 2048);
    writeDicom(image, path);

    MainWindow window;
    window.openPaths({path});
    ASSERT_TRUE(finishJobs(window));
    auto *document = window.activeDocument();
    ASSERT_NE(document, nullptr);
    ASSERT_TRUE(waitFor([&] { return document->isVisible(); }));
    EXPECT_DOUBLE_EQ(document->image.sample(1, 2), 201);

    auto *viewer = document->viewer;
    for (const auto &[x, expected] : {std::pair{1, "X 1   Y 2   Z 0   V -722.5 HU (201)"},
                                      std::pair{2, "X 2   Y 2   Z 0   V 2048 HU"}}) {
        const auto position = viewer->imageToWidget({x + 0.5, 2.5});
        QMouseEvent move(QEvent::MouseMove, position, position, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(viewer, &move);
        QStringList statusLabels;
        for (auto *label : window.statusBar()->findChildren<QLabel *>()) statusLabels.append(label->text());
        EXPECT_TRUE(statusLabels.contains(expected)) << statusLabels.join(" | ").toStdString();
    }
}

TEST(Desktop, RevertReloadsContentDetectedDicom)
{
    QTemporaryDir directory;
    const auto original = writeImportSeries(directory.path(), 1).front();
    const auto path = directory.filePath("single.IMA");
    ASSERT_TRUE(QFile::rename(original, path));
    MainWindow window;
    window.openPaths({path});
    ASSERT_TRUE(finishJobs(window));
    auto *document = window.activeDocument(); ASSERT_NE(document, nullptr);
    auto edited = document->image.clone();
    edited.setValue(std::array<std::size_t, 3>{0, 0, 0}, 999);
    document->replaceImage(edited, "Edit");
    QString error;
    QTimer dismiss;
    QObject::connect(&dismiss, &QTimer::timeout, &window, [&] {
        if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            if (message->standardButtons().testFlag(QMessageBox::Yes)) message->button(QMessageBox::Yes)->click();
            else { error = message->text(); message->accept(); }
        }
    });
    dismiss.start(1);
    window.executeCommand("file.revert");
    ASSERT_TRUE(finishJobs(window));
    EXPECT_TRUE(error.isEmpty()) << error.toStdString();
    EXPECT_EQ(document->image.sample(0, 0), 101);
    EXPECT_FALSE(document->modified);
    EXPECT_EQ(document->image.metadata().sourcePath, path);
}

TEST(Desktop, FolderImportsDetachedImageWithoutPromptingForItsPayload)
{
    QTemporaryDir directory;
    QFile header(directory.filePath("volume.mhd"));
    ASSERT_TRUE(header.open(QIODevice::WriteOnly));
    header.write("ObjectType = Image\nNDims = 3\nBinaryData = True\nBinaryDataByteOrderMSB = False\n"
                 "ElementSpacing = 1 1 1\nDimSize = 4 4 1\nElementType = MET_USHORT\nElementDataFile = volume.raw\n");
    header.close();
    QFile payload(directory.filePath("volume.raw"));
    ASSERT_TRUE(payload.open(QIODevice::WriteOnly)); payload.write(QByteArray(32, '\0')); payload.close();
    MainWindow window;
    int prompts = 0;
    QTimer dismiss;
    QObject::connect(&dismiss, &QTimer::timeout, &window, [&] {
        if (auto *dialog = window.findChild<QDialog *>("rawImportDialog")) { ++prompts; dialog->reject(); }
    });
    dismiss.start(1);
    window.openPaths({directory.path()});
    ASSERT_TRUE(finishJobs(window));
    EXPECT_EQ(prompts, 0);
    ASSERT_EQ(window.documents().size(), 1);
    EXPECT_EQ(window.activeDocument()->image.metadata().sourcePath, header.fileName());
}

TEST(Desktop, DroppedMetaImagePayloadOpensOnlyThroughItsHeader)
{
    QTemporaryDir directory;
    ASSERT_TRUE(QDir(directory.path()).mkpath("payloads"));
    const auto dicom = writeImportSeries(directory.path(), 1).front();
    const auto raw = directory.filePath("payloads/scan data.raw");
    writeImageWithMetadata(Image::allocate(PixelType::UInt16, {4, 4, 1}), raw);
    QFile header(directory.filePath("different-name.MHD"));
    ASSERT_TRUE(header.open(QIODevice::WriteOnly));
    header.write("ObjectType = Image\nNDims = 3\nBinaryData = True\nBinaryDataByteOrderMSB = False\n"
                 "ElementSpacing = 1 1 1\nDimSize = 4 4 1\nElementType = MET_USHORT\n"
                 "ElementDataFile = payloads/../payloads/scan data.raw\n");
    header.close();
    // A same-named raw file in another directory is still a separate image.
    const auto unrelated = directory.filePath("scan data.raw");
    writeImageWithMetadata(Image::allocate(PixelType::UInt16, {4, 4}), unrelated);
    for (bool sidecar : {true, false}) {
        if (!sidecar) ASSERT_TRUE(QFile::remove(raw + ".json"));
        for (int mode : {0, 1, 2}) {
            MainWindow window; window.show();
            int prompts = 0;
            QTimer dismiss;
            QObject::connect(&dismiss, &QTimer::timeout, &window, [&] {
                if (auto *dialog = window.findChild<QDialog *>("rawImportDialog")) { ++prompts; dialog->reject(); }
            });
            dismiss.start(1);
            auto paths = mode == 0 ? QStringList{raw, dicom, header.fileName()}
                                   : QStringList{header.fileName(), dicom, raw};
            QSet<QString> expected{dicom, header.fileName()};
            if (mode == 2) { paths.append(unrelated); expected.insert(unrelated); }
            dropPaths(&window, paths);
            ASSERT_TRUE(finishJobs(window));
            EXPECT_EQ(prompts, 0);
            ASSERT_EQ(window.documents().size(), expected.size());
            QSet<QString> sources;
            for (const auto *document : window.documents()) sources.insert(document->image.metadata().sourcePath);
            EXPECT_EQ(sources, expected);
        }
    }
}

TEST(Desktop, ExcessiveDicomMemoryDoesNotBlockOrdinaryImages)
{
    QTemporaryDir directory;
    const auto path = writeImportSeries(directory.path(), 1).front();
    DcmFileFormat file;
    ASSERT_TRUE(file.loadFile(QFile::encodeName(path).constData()).good());
    auto *dataset = file.getDataset();
    ASSERT_TRUE(dataset->putAndInsertUint16(DCM_Rows, 65535).good());
    ASSERT_TRUE(dataset->putAndInsertUint16(DCM_Columns, 65535).good());
    ASSERT_TRUE(dataset->putAndInsertUint16(DCM_BitsAllocated, 8).good());
    ASSERT_TRUE(dataset->putAndInsertUint16(DCM_BitsStored, 8).good());
    ASSERT_TRUE(dataset->putAndInsertUint16(DCM_HighBit, 7).good());
    ASSERT_TRUE(dataset->putAndInsertString(DCM_NumberOfFrames, "2147483647").good());
    ASSERT_TRUE(file.saveFile(QFile::encodeName(path).constData(), EXS_LittleEndianExplicit).good());
    EXPECT_THROW(readDicom(path), std::length_error);
    const auto raster = directory.filePath("ordinary.tif");
    writeImageWithMetadata(Image::allocate(PixelType::UInt8, {4, 4}), raster);
    MainWindow window;
    QString error;
    QTimer dismiss;
    QObject::connect(&dismiss, &QTimer::timeout, &window, [&] {
        if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            error = message->text(); message->accept();
        }
    });
    dismiss.start(1);
    window.openPaths({path, raster});
    ASSERT_TRUE(finishJobs(window));
    EXPECT_TRUE(error.contains("memory")) << error.toStdString();
    ASSERT_EQ(window.documents().size(), 1);
    EXPECT_EQ(window.activeDocument()->image.metadata().sourcePath, raster);
}

TEST(Desktop, ExplicitRawAndOverlappingFolderImportOnlyOnce)
{
    QTemporaryDir directory;
    const auto raw = directory.filePath("image.raw");
    writeImageWithMetadata(Image::allocate(PixelType::UInt16, {4, 4}), raw);
    writeImageWithMetadata(Image::allocate(PixelType::UInt8, {4, 4}), directory.filePath("ordinary.tif"));
    MainWindow window;
    window.openPaths({raw, directory.path(), raw});
    ASSERT_TRUE(finishJobs(window));
    ASSERT_EQ(window.documents().size(), 2);
    int rawCount = 0;
    for (const auto *document : window.documents())
        if (document->image.metadata().sourcePath == raw) ++rawCount;
    EXPECT_EQ(rawCount, 1);
}

TEST(Desktop, DroppedRawStackPromptsAndKeepsInvalidLayoutEditable)
{
    QSettings settings; settings.remove("rawImport");
    QTemporaryDir directory;
    const auto path = directory.filePath("stack.RAW");
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    const unsigned char bytes[]{'H', 'D', 0, 1, 1, 0, 0xff, 0xfe, 0x12, 0x34,
                               0, 2, 2, 0, 0xff, 0xfd, 0x43, 0x21};
    ASSERT_EQ(file.write(reinterpret_cast<const char *>(bytes), sizeof(bytes)), qint64(sizeof(bytes)));
    file.close();
    MainWindow window; window.show();
    bool prompted = false;
    QTimer::singleShot(0, &window, [&] {
        auto *dialog = window.findChild<QDialog *>("rawImportDialog");
        EXPECT_NE(dialog, nullptr);
        if (!dialog) return;
        prompted = true;
        auto *type = dialog->findChild<QComboBox *>("rawPixelType");
        auto *width = dialog->findChild<QSpinBox *>("rawWidth");
        auto *height = dialog->findChild<QSpinBox *>("rawHeight");
        auto *count = dialog->findChild<QSpinBox *>("rawImages");
        auto *offset = dialog->findChild<QLineEdit *>("rawOffset");
        auto *endian = dialog->findChild<QComboBox *>("rawByteOrder");
        auto *error = dialog->findChild<QLabel *>("rawImportError");
        auto *buttons = dialog->findChild<QDialogButtonBox *>();
        type->setCurrentIndex(type->findData("uint16"));
        width->setValue(5); height->setValue(2); count->setValue(9999);
        offset->setText("2"); endian->setCurrentIndex(2);
        const auto capture = qEnvironmentVariable("IMAGECXX_RAW_CAPTURE_PATH");
        if (!capture.isEmpty()) EXPECT_TRUE(dialog->grab().save(capture));
        buttons->button(QDialogButtonBox::Ok)->click();
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_TRUE(error->isVisible());
        EXPECT_TRUE(error->text().contains("too small"));
        EXPECT_FALSE(window.jobs().busy());
        EXPECT_TRUE(window.documents().empty());
        width->setValue(2);
        buttons->button(QDialogButtonBox::Ok)->click();
    });
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(path)});
    QDragEnterEvent enter(QPointF(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &enter);
    ASSERT_TRUE(enter.isAccepted());
    QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &drop);
    EXPECT_TRUE(drop.isAccepted());
    ASSERT_TRUE(prompted);
    ASSERT_TRUE(finishJobs(window));
    auto *document = window.activeDocument();
    ASSERT_NE(document, nullptr);
    EXPECT_EQ(document->image.pixelType(), PixelType::UInt16);
    EXPECT_EQ(document->image.shape(), (std::vector<std::size_t>{2, 2, 2}));
    EXPECT_EQ(document->image.sample(1, 0), 256);
    EXPECT_EQ(document->image.sample(0, 1), 65534);
    EXPECT_EQ(document->image.sample(1, 1, 1), 0x4321);
    EXPECT_EQ(document->image.metadata().sourcePath, path);
    EXPECT_FALSE(QFileInfo::exists(path + ".json"));

}

TEST(Desktop, RawImageCountCapsCompleteImagesAndIgnoresPartialTail)
{
    QSettings settings; settings.remove("rawImport");
    QTemporaryDir directory;
    const auto path = directory.filePath("partial.bin");
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    const std::array<std::uint16_t, 13> pixels{65535, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    ASSERT_EQ(file.write(reinterpret_cast<const char *>(pixels.data()), sizeof(pixels)), qint64(sizeof(pixels)));
    ASSERT_EQ(file.write("xyz", 3), 3);
    file.close();
    for (int maximum : {1, 2, 9999, std::numeric_limits<int>::max()}) {
        MainWindow window;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<QDialog *>("rawImportDialog");
            EXPECT_NE(dialog, nullptr);
            if (!dialog) return;
            dialog->findChild<QSpinBox *>("rawWidth")->setValue(2);
            dialog->findChild<QSpinBox *>("rawHeight")->setValue(2);
            dialog->findChild<QSpinBox *>("rawImages")->setValue(maximum);
            dialog->findChild<QLineEdit *>("rawOffset")->setText("2");
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            if (dialog->isVisible()) {
                ADD_FAILURE() << "A maximum count must accept fewer complete images";
                dialog->reject();
            }
        });
        window.openPaths({path});
        ASSERT_TRUE(finishJobs(window));
        ASSERT_NE(window.activeDocument(), nullptr);
        const auto &image = window.activeDocument()->image;
        EXPECT_EQ(image.depth(), std::size_t(std::min(maximum, 3)));
        for (std::size_t z = 0; z < image.depth(); ++z) {
            EXPECT_EQ(image.sample(0, 0, z), 1 + z * 4);
            EXPECT_EQ(image.sample(1, 1, z), 4 + z * 4);
        }
    }
}

TEST(Desktop, RawImportRemembersAllAcceptedOptionsAcrossWindowsAndCancellation)
{
    QSettings settings; settings.remove("rawImport");
    QTemporaryDir directory;
    const auto path = directory.filePath("remember.raw");
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(QByteArray(8 + 3 * 2 * 4 * 2 * 3 * 4 + 5, char(0)));
    file.close();
    {
        MainWindow window;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<QDialog *>("rawImportDialog");
            EXPECT_NE(dialog, nullptr);
            if (!dialog) return;
            auto *type = dialog->findChild<QComboBox *>("rawPixelType");
            type->setCurrentIndex(type->findData("float32"));
            dialog->findChild<QSpinBox *>("rawWidth")->setValue(3);
            dialog->findChild<QSpinBox *>("rawHeight")->setValue(2);
            dialog->findChild<QSpinBox *>("rawImages")->setValue(9999);
            dialog->findChild<QLineEdit *>("rawOffset")->setText("8");
            auto *order = dialog->findChild<QComboBox *>("rawByteOrder");
            order->setCurrentIndex(order->findData("big"));
            dialog->findChild<QToolButton *>("rawAdvanced")->setChecked(true);
            dialog->findChild<QSpinBox *>("rawChannels")->setValue(2);
            dialog->findChild<QSpinBox *>("rawFrames")->setValue(3);
            dialog->findChild<QLineEdit *>("rawSpacing")->setText("0.5,1.25,2.5");
            dialog->findChild<QLineEdit *>("rawUnit")->setText("um");
            dialog->findChild<QCheckBox *>("rawMemoryMap")->setChecked(false);
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            if (dialog->isVisible()) { ADD_FAILURE() << "Valid layout was rejected"; dialog->reject(); }
        });
        window.openPaths({path});
        ASSERT_TRUE(finishJobs(window));
        ASSERT_NE(window.activeDocument(), nullptr);
        EXPECT_EQ(window.activeDocument()->image.depth(), 4u);
    }
    settings.sync();
    // Recreate both launcher and dialog, then ensure Cancel preserves the last accepted options.
    for (int attempt = 0; attempt < 2; ++attempt) {
        MainWindow window;
        bool restored = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<QDialog *>("rawImportDialog");
            EXPECT_NE(dialog, nullptr);
            if (!dialog) return;
            restored = true;
            EXPECT_EQ(dialog->findChild<QComboBox *>("rawPixelType")->currentData().toString(), "float32");
            EXPECT_EQ(dialog->findChild<QSpinBox *>("rawWidth")->value(), 3);
            EXPECT_EQ(dialog->findChild<QSpinBox *>("rawHeight")->value(), 2);
            EXPECT_EQ(dialog->findChild<QSpinBox *>("rawImages")->value(), 9999);
            EXPECT_EQ(dialog->findChild<QLineEdit *>("rawOffset")->text(), "8");
            EXPECT_EQ(dialog->findChild<QComboBox *>("rawByteOrder")->currentData().toString(), "big");
            EXPECT_TRUE(dialog->findChild<QToolButton *>("rawAdvanced")->isChecked());
            EXPECT_EQ(dialog->findChild<QSpinBox *>("rawChannels")->value(), 2);
            EXPECT_EQ(dialog->findChild<QSpinBox *>("rawFrames")->value(), 3);
            EXPECT_EQ(dialog->findChild<QLineEdit *>("rawSpacing")->text(), "0.5,1.25,2.5");
            EXPECT_EQ(dialog->findChild<QLineEdit *>("rawUnit")->text(), "um");
            EXPECT_FALSE(dialog->findChild<QCheckBox *>("rawMemoryMap")->isChecked());
            dialog->findChild<QSpinBox *>("rawWidth")->setValue(123);
            dialog->findChild<QLineEdit *>("rawOffset")->setText("invalid");
            dialog->findChild<QToolButton *>("rawAdvanced")->setChecked(false);
            dialog->reject();
        });
        window.openPaths({path});
        EXPECT_TRUE(restored);
        EXPECT_FALSE(window.jobs().busy());
        EXPECT_TRUE(window.documents().empty());
    }
}

TEST(Desktop, CancellingRawImportContinuesOpeningOtherFiles)
{
    QSettings settings; settings.remove("rawImport");
    QTemporaryDir directory;
    QStringList rawPaths;
    for (const auto *name : {"cancel.bin", "cancel.raw", "cancel-uppercase.RAW"}) {
        const auto path = directory.filePath(name);
        QFile file(path); ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(QByteArray(16, char(0))); file.close();
        rawPaths.append(path);
    }
    const auto imagePath = directory.filePath("next.tif");
    auto image = Image::allocate(PixelType::UInt8, {4, 4});
    image.mutableData()[15] = std::byte{173};
    writeImageWithMetadata(image, imagePath);
    for (bool onDocument : {false, true}) {
        MainWindow window; window.show();
        auto *document = onDocument ? window.addDocument(Image::allocate(PixelType::UInt8, {4, 4})) : nullptr;
        int cancelled = 0;
        QStringList errors;
        QTimer dismiss;
        QObject::connect(&dismiss, &QTimer::timeout, &window, [&] {
            auto *modal = QApplication::activeModalWidget();
            if (auto *message = qobject_cast<QMessageBox *>(modal)) {
                errors.append(message->text());
                message->accept();
            } else if (auto *dialog = qobject_cast<QDialog *>(modal);
                       dialog && dialog->objectName() == "rawImportDialog") {
                ++cancelled;
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel)->click();
            }
        });
        dismiss.start(1);
        auto paths = rawPaths;
        paths.append(rawPaths.front());
        paths.append(imagePath);
        dropPaths(document ? static_cast<QWidget *>(document) : &window, paths);
        EXPECT_EQ(cancelled, rawPaths.size());
        ASSERT_TRUE(finishJobs(window));
        EXPECT_TRUE(errors.isEmpty()) << errors.join('\n').toStdString();
        ASSERT_EQ(window.documents().size(), onDocument ? 2 : 1);
        EXPECT_EQ(window.activeDocument()->image.sample(3, 3), 173);
        EXPECT_EQ(window.activeDocument()->image.metadata().sourcePath, imagePath);
    }
}

TEST(Desktop, UnsupportedFileInRawDropReportsItsNameAndOtherImagesStillOpen)
{
    QSettings settings; settings.remove("rawImport");
    QTemporaryDir directory;
    QStringList paths;
    for (const auto *name : {"first.raw", "notes.txt", "second.bin"}) {
        QFile file(directory.filePath(name));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("Not an image\n"); file.close();
        paths.append(file.fileName());
    }
    const auto unsupported = paths[1];
    const auto imagePath = directory.filePath("next.tif");
    auto image = Image::allocate(PixelType::UInt8, {4, 4});
    image.mutableData()[15] = std::byte{173};
    writeImageWithMetadata(image, imagePath);
    paths.append(imagePath);
    MainWindow window; window.show();
    int cancelled = 0;
    QStringList errors;
    QTimer dismiss;
    QObject::connect(&dismiss, &QTimer::timeout, &window, [&] {
        auto *modal = QApplication::activeModalWidget();
        if (auto *message = qobject_cast<QMessageBox *>(modal)) {
            errors.append(message->text());
            message->accept();
        } else if (auto *dialog = qobject_cast<QDialog *>(modal);
                   dialog && dialog->objectName() == "rawImportDialog") {
            ++cancelled;
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel)->click();
        }
    });
    dismiss.start(1);
    dropPaths(&window, paths);
    EXPECT_EQ(cancelled, 2);
    ASSERT_TRUE(finishJobs(window));
    ASSERT_EQ(errors.size(), 1);
    EXPECT_TRUE(errors.front().contains(unsupported)) << errors.front().toStdString();
    EXPECT_TRUE(errors.front().contains("unsupported or invalid image format")) << errors.front().toStdString();
    EXPECT_FALSE(errors.front().contains("I/O suspension"));
    ASSERT_EQ(window.documents().size(), 1);
    EXPECT_EQ(window.activeDocument()->image.metadata().sourcePath, imagePath);
    EXPECT_EQ(window.activeDocument()->image.sample(3, 3), 173);
}

TEST(Desktop, RawWithSidecarOpensWithoutPrompting)
{
    QSettings settings; settings.remove("rawImport");
    QTemporaryDir directory;
    const auto path = directory.filePath("with-layout.bin");
    auto original = Image::allocate(PixelType::Float32, {3, 2, 2});
    original.setValue(std::array<std::size_t, 3>{1, 1, 1}, -2.5);
    writeImageWithMetadata(original, path);
    MainWindow window;
    bool prompted = false;
    QTimer guard;
    QObject::connect(&guard, &QTimer::timeout, &window, [&] {
        if (auto *dialog = window.findChild<QDialog *>("rawImportDialog")) {
            prompted = true;
            dialog->reject();
        }
    });
    guard.start(1);
    window.openPaths({path});
    ASSERT_TRUE(finishJobs(window));
    EXPECT_FALSE(prompted);
    ASSERT_NE(window.activeDocument(), nullptr);
    EXPECT_EQ(window.activeDocument()->image.pixelType(), PixelType::Float32);
    EXPECT_EQ(window.activeDocument()->image.sample(1, 1, 1), -2.5);
}

TEST(Desktop, ExplicitRawImportUsesTheSameDialogForAnyExtension)
{
    QSettings settings; settings.remove("rawImport");
    QTemporaryDir directory;
    const auto path = directory.filePath("pixels.dat");
    QFile file(path); ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(QByteArray(16, char(42))); file.close();
    MainWindow window;
    bool selected = false, prompted = false;
    QTimer automation;
    QObject::connect(&automation, &QTimer::timeout, &window, [&] {
        if (auto *dialog = window.findChild<QDialog *>("rawImportDialog")) {
            prompted = true;
            auto *type = dialog->findChild<QComboBox *>("rawPixelType");
            type->setCurrentIndex(type->findData("uint8"));
            dialog->findChild<QSpinBox *>("rawWidth")->setValue(2);
            dialog->findChild<QSpinBox *>("rawHeight")->setValue(2);
            dialog->findChild<QSpinBox *>("rawChannels")->setValue(2);
            dialog->findChild<QSpinBox *>("rawFrames")->setValue(2);
            dialog->findChild<QLineEdit *>("rawSpacing")->setText("0.5,0.75,2");
            automation.stop();
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        } else if (!selected) {
            for (auto *widget : QApplication::topLevelWidgets()) if (auto *dialog = qobject_cast<QFileDialog *>(widget)) {
                selected = true;
                dialog->setDirectory(directory.path());
                dialog->selectFile(QFileInfo(path).fileName());
                if (auto *name = dialog->findChild<QLineEdit *>("fileNameEdit")) name->setText(QFileInfo(path).fileName());
                QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
                return;
            }
        }
    });
    automation.start(1);
    window.executeCommand("file.raw");
    ASSERT_TRUE(selected); ASSERT_TRUE(prompted);
    ASSERT_TRUE(finishJobs(window));
    ASSERT_NE(window.activeDocument(), nullptr);
    const auto &image = window.activeDocument()->image;
    EXPECT_EQ(image.shape(), (std::vector<std::size_t>{2, 2, 1, 2, 2}));
    EXPECT_EQ(image.sample(1, 1, 0, 1, 1), 42);
    EXPECT_EQ(image.metadata().axes[0].spacing, 0.5);
    EXPECT_EQ(image.metadata().axes[1].spacing, 0.75);
}

TEST(Desktop, ProjectionOpensNewWindowAndRemembersAcceptedOptionsAcrossWindowsAndCancellation)
{
    QSettings settings; settings.remove("projection3d");
    auto image = Image::allocate(PixelType::UInt8, {64, 56, 20}); image.metadata().name = "Cells stack";
    for (std::size_t z = 0; z < image.depth(); ++z) for (std::size_t y = 0; y < image.height(); ++y)
        for (std::size_t x = 0; x < image.width(); ++x) {
            double value = 0;
            for (int cell = 0; cell < 5; ++cell) {
                const double dx = double(x) - (12 + cell * 9), dy = double(y) - (12 + cell % 3 * 14);
                const double dz = double(z) - (4 + cell % 3 * 5);
                value += 220 * std::exp(-(dx*dx + dy*dy + 4*dz*dz) / 45.);
            }
            image.setValue(std::array<std::size_t, 3>{x, y, z}, value);
        }
    const QMap<QString, double> saved{{"sliceSpacing", 2.5}, {"initialAngle", 30}, {"totalRotation", 180},
        {"angleIncrement", 45}, {"opacity", 25}, {"surfaceDepthCue", 75}, {"interiorDepthCue", 30}};
    const auto capture = qEnvironmentVariable("IMAGECXX_STACK_CAPTURE_DIR");
    for (int attempt = 0; attempt < 3; ++attempt) {
        MainWindow window; window.show(); window.move(30, 30);
        auto *action = window.findChild<QAction *>("stacks.project3d"); ASSERT_NE(action, nullptr);
        EXPECT_FALSE(action->isEnabled());
        window.addDocument(Image::allocate(PixelType::UInt8, {2, 2})); EXPECT_FALSE(action->isEnabled());
        auto *source = window.addDocument(image); source->move(30, 180); source->resize(420, 360);
        source->viewer->setLut("viridis");
        source->viewer->setInverted(true);
        EXPECT_TRUE(action->isEnabled());
        EXPECT_TRUE(window.findChild<QMenu *>("stacksMenu")->actions().contains(action));
        bool prompted = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<QDialog *>("projection3dDialog"); EXPECT_NE(dialog, nullptr);
            if (!dialog) return;
            prompted = true;
            EXPECT_TRUE(finishJobs(window));
            auto *method = dialog->findChild<QComboBox *>("projectionMethod");
            auto *axis = dialog->findChild<QComboBox *>("projectionAxis");
            auto *interpolate = dialog->findChild<QCheckBox *>("projectionInterpolate");
            auto *ok = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
            if (attempt == 0) {
                if (!capture.isEmpty()) EXPECT_TRUE(dialog->grab().save(capture + "/3d-project-options.png"));
                dialog->findChild<QLineEdit *>("lower")->setText("300");
                EXPECT_FALSE(ok->isEnabled());
                dialog->findChild<QLineEdit *>("lower")->setText("2");
                dialog->findChild<QLineEdit *>("upper")->setText("240");
                method->setCurrentIndex(2); axis->setCurrentIndex(0); interpolate->setChecked(true);
                for (auto it = saved.cbegin(); it != saved.cend(); ++it) dialog->findChild<QDoubleSpinBox *>(it.key())->setValue(it.value());
                EXPECT_TRUE(ok->isEnabled()); ok->click();
            } else {
                EXPECT_EQ(method->currentIndex(), 2); EXPECT_EQ(axis->currentIndex(), 0); EXPECT_TRUE(interpolate->isChecked());
                for (auto it = saved.cbegin(); it != saved.cend(); ++it)
                    EXPECT_DOUBLE_EQ(dialog->findChild<QDoubleSpinBox *>(it.key())->value(), it.value());
                method->setCurrentIndex(0); axis->setCurrentIndex(2); interpolate->setChecked(false);
                dialog->findChild<QDoubleSpinBox *>("sliceSpacing")->setValue(9); dialog->reject();
            }
        });
        action->trigger(); EXPECT_TRUE(prompted); ASSERT_TRUE(finishJobs(window));
        EXPECT_EQ(window.documents().size(), attempt == 0 ? 3 : 2);
        EXPECT_FALSE(source->modified); EXPECT_FALSE(source->history.canUndo());
        EXPECT_EQ(source->image.sample(12, 12, 4), image.sample(12, 12, 4));
        if (attempt == 0) {
            auto *output = window.activeDocument(); ASSERT_NE(output, source);
            EXPECT_EQ(output->viewer->lut(), "viridis");
            EXPECT_TRUE(output->viewer->isInverted());
            EXPECT_TRUE(output->modified); EXPECT_TRUE(output->image.metadata().sourcePath.isEmpty());
            EXPECT_EQ(output->image.depth(), 5u); EXPECT_EQ(output->image.metadata().axes[2].origin, 30);
            if (!capture.isEmpty()) {
                window.documents().front()->hide(); output->move(500, 180); output->resize(420, 360);
                output->viewer->setSlice(1);
                QElapsedTimer timer; timer.start();
                while (timer.elapsed() < 400) { QCoreApplication::processEvents(); QThread::msleep(2); }
                const auto frame = output->frameGeometry();
                EXPECT_TRUE(output->screen()->grabWindow(0, frame.x(), frame.y(), frame.width(), frame.height())
                    .save(capture + "/3d-project-result.png"));
                auto *menu = window.findChild<QMenu *>("stacksMenu");
                menu->popup(window.mapToGlobal(QPoint(300, window.height()))); QCoreApplication::processEvents();
                EXPECT_TRUE(window.screen()->grabWindow(0, 0, 0, 950, 590).save(capture + "/3d-project-windows.png"));
                menu->hide();
            }
        }
    }
    settings.remove("projection3d");
}

TEST(Desktop, ProjectionBoundsUseTheWholeImageRangeAndNeverPersist)
{
    QSettings settings; settings.remove("projection3d");
    settings.setValue("projection3d/v1/lower", 2); settings.setValue("projection3d/v1/upper", 240);
    MainWindow window;
    const std::array<std::array<double, 2>, 5> ranges{{
        {1e-12, .875}, {-2048, 8192}, {7, 7},
        {-std::numeric_limits<double>::max(), std::numeric_limits<double>::max()}, {-.25, .75}
    }};
    for (std::size_t attempt = 0; attempt < ranges.size(); ++attempt) {
        const auto [minimum, maximum] = ranges[attempt];
        auto image = Image::allocate(attempt == 3 ? PixelType::Float64 : PixelType::Float32, {1, 1, 3, 2, 2});
        image.metadata().intensityScale = 100; image.metadata().intensityOffset = 50;
        for (std::size_t t = 0; t < 2; ++t) for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t z = 0; z < 3; ++z)
                image.setValue(std::array<std::size_t, 5>{0, 0, z, c, t}, maximum);
        image.setValue(std::array<std::size_t, 5>{0, 0, 1, 1, 1}, minimum);
        image.setValue(std::array<std::size_t, 5>{0, 0, 0, 0, 0}, std::numeric_limits<double>::quiet_NaN());
        image.setValue(std::array<std::size_t, 5>{0, 0, 2, 0, 0}, std::numeric_limits<double>::infinity());
        auto *source = window.addDocument(image);
        bool prompted = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<QDialog *>("projection3dDialog"); EXPECT_NE(dialog, nullptr);
            if (!dialog) return;
            prompted = true;
            EXPECT_TRUE(finishJobs(window));
            auto *lower = dialog->findChild<QLineEdit *>("lower");
            auto *upper = dialog->findChild<QLineEdit *>("upper");
            const double imageMinimum = image.sample(0, 0, 1, 1, 1), imageMaximum = image.sample(0, 0, 1);
            EXPECT_DOUBLE_EQ(lower->text().toDouble(), imageMinimum);
            EXPECT_DOUBLE_EQ(upper->text().toDouble(), imageMaximum);
            auto *lowerSlider = dialog->findChild<QSlider *>("projectionLowerSlider");
            auto *upperSlider = dialog->findChild<QSlider *>("projectionUpperSlider");
            EXPECT_EQ(lowerSlider->value(), 0);
            EXPECT_EQ(upperSlider->value(), minimum < maximum ? 1000 : 0);
            EXPECT_EQ(lowerSlider->isEnabled(), minimum < maximum);
            EXPECT_EQ(upperSlider->isEnabled(), minimum < maximum);
            auto *ok = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
            EXPECT_TRUE(ok->isEnabled());
            lower->setText("invalid"); EXPECT_FALSE(ok->isEnabled());
            lower->setText(QString::number(maximum, 'g', 17)); EXPECT_TRUE(ok->isEnabled());
            EXPECT_EQ(lowerSlider->value(), minimum < maximum ? 1000 : 0);
            if (minimum < maximum) {
                lowerSlider->setValue(250);
                EXPECT_DOUBLE_EQ(lower->text().toDouble(), std::lerp(imageMinimum, imageMaximum, .25));
                upperSlider->setValue(750);
                EXPECT_DOUBLE_EQ(upper->text().toDouble(), std::lerp(imageMinimum, imageMaximum, .75));
                lower->setText(QString::number(imageMinimum, 'g', 17));
                upper->setText(QString::number(imageMaximum, 'g', 17));
                EXPECT_EQ(lowerSlider->value(), 0); EXPECT_EQ(upperSlider->value(), 1000);
            }
            dialog->findChild<QComboBox *>("projectionAxis")->setCurrentIndex(2);
            dialog->findChild<QDoubleSpinBox *>("totalRotation")->setValue(0);
            dialog->findChild<QDoubleSpinBox *>("interiorDepthCue")->setValue(0);
            if (attempt == 1) dialog->reject(); else ok->click();
        });
        window.commands().execute("stacks.project3d");
        EXPECT_TRUE(prompted); ASSERT_TRUE(finishJobs(window));
        EXPECT_FALSE(settings.contains("projection3d/v1/lower"));
        EXPECT_FALSE(settings.contains("projection3d/v1/upper"));
        if (attempt != 1) {
            ASSERT_NE(window.activeDocument(), source);
            EXPECT_EQ(window.activeDocument()->image.pixelType(), image.pixelType());
            EXPECT_DOUBLE_EQ(window.activeDocument()->image.sample(1, 1, 0, 1, 1), image.sample(0, 0, 1));
        }
    }
    auto nonfinite = Image::allocate(PixelType::Float32, {1, 1, 2});
    nonfinite.setValue(std::array<std::size_t, 3>{0, 0, 0}, std::numeric_limits<double>::quiet_NaN());
    nonfinite.setValue(std::array<std::size_t, 3>{0, 0, 1}, std::numeric_limits<double>::infinity());
    window.addDocument(nonfinite);
    for (bool waitForRange : {true, false}) {
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<QDialog *>("projection3dDialog"); EXPECT_NE(dialog, nullptr);
            if (!dialog) return;
            if (waitForRange) {
                EXPECT_TRUE(finishJobs(window));
                EXPECT_FALSE(dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->isEnabled());
                EXPECT_TRUE(dialog->findChild<QLabel *>("projectionOutputSize")->text().contains("no finite"));
            }
            dialog->reject();
        });
        window.commands().execute("stacks.project3d");
        ASSERT_TRUE(finishJobs(window));
    }
    settings.remove("projection3d");
}

TEST(Desktop, ResliceRemembersAllAcceptedOptionsAndKeepsInterpolationSpacingAfterCancellation)
{
    QSettings settings; settings.remove("reslice");
    for (int attempt = 0; attempt < 3; ++attempt) {
        MainWindow window;
        auto *source = window.addDocument(Image::allocate(PixelType::UInt8, {8, 6, 4}));
        source->viewer->setLut("hot");
        source->viewer->setInverted(true);
        if (attempt == 0) window.executeCommand("view.overlay");
        bool prompted = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<QDialog *>("resliceDialog"); EXPECT_NE(dialog, nullptr);
            if (!dialog) return;
            prompted = true;
            auto *spacing = dialog->findChild<QDoubleSpinBox *>("resliceSpacing");
            auto *start = dialog->findChild<QComboBox *>("resliceStart");
            auto *flip = dialog->findChild<QCheckBox *>("resliceFlip");
            auto *rotate = dialog->findChild<QCheckBox *>("resliceRotate");
            auto *avoid = dialog->findChild<QCheckBox *>("resliceAvoidInterpolation");
            if (attempt == 0) {
                spacing->setValue(2.5); start->setCurrentIndex(3); flip->setChecked(true); rotate->setChecked(true); avoid->setChecked(true);
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            } else {
                EXPECT_EQ(start->currentIndex(), 3); EXPECT_TRUE(flip->isChecked()); EXPECT_TRUE(rotate->isChecked());
                EXPECT_TRUE(avoid->isChecked()); EXPECT_FALSE(spacing->isEnabled()); EXPECT_EQ(spacing->value(), 1);
                const auto capture = qEnvironmentVariable("IMAGECXX_STACK_CAPTURE_DIR");
                if (attempt == 1 && !capture.isEmpty()) EXPECT_TRUE(dialog->grab().save(capture + "/reslice-options.png"));
                avoid->setChecked(false); EXPECT_DOUBLE_EQ(spacing->value(), 2.5);
                spacing->setValue(9); start->setCurrentIndex(0); flip->setChecked(false); rotate->setChecked(false); dialog->reject();
            }
        });
        window.executeCommand("stacks.reslice"); EXPECT_TRUE(prompted); ASSERT_TRUE(finishJobs(window));
        EXPECT_EQ(window.documents().size(), attempt == 0 ? 2 : 1); EXPECT_FALSE(source->modified);
        if (attempt == 0) {
            auto *output = window.activeDocument(); ASSERT_NE(output, source);
            EXPECT_EQ(output->viewer->lut(), "hot");
            EXPECT_TRUE(output->viewer->isInverted());
            auto *layers = window.findChild<QTreeWidget *>("overlayImages");
            ASSERT_NE(layers, nullptr);
            ASSERT_EQ(layers->topLevelItemCount(), 2);
            EXPECT_EQ(layers->topLevelItem(0)->data(0, Qt::UserRole + 1).toString(), "hot");
        }
    }
    settings.remove("reslice");
}

TEST(Desktop, GeneratedFilterFormRunsThroughRegistryAndPreservesUndo)
{
    MainWindow window;
    auto image = Image::allocate(PixelType::UInt16, {8, 8});
    reinterpret_cast<std::uint16_t *>(image.mutableData())[3 * 8 + 3] = 1200;
    auto *document = window.addDocument(image);
    bool accepted = false;
    QTimer::singleShot(0, &window, [&] {
        for (auto *widget : QApplication::topLevelWidgets()) if (auto *dialog = qobject_cast<QDialog *>(widget)) {
            if (auto *sigma = dialog->findChild<QDoubleSpinBox *>("sigma")) {
                sigma->setValue(1.25); accepted = true; dialog->accept(); return;
            }
        }
    });
    window.executeCommand("process.gaussian");
    ASSERT_TRUE(accepted);
    QElapsedTimer timer; timer.start();
    while (window.jobs().busy() && timer.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(2); }
    ASSERT_FALSE(window.jobs().busy());
    EXPECT_TRUE(document->history.canUndo());
    EXPECT_EQ(document->image.pixelType(), PixelType::UInt16);
    EXPECT_GT(document->image.sample(3, 3), 0);
    EXPECT_LT(document->image.sample(3, 3), 1200);
    EXPECT_GT(document->image.sample(3, 4), 0);
    EXPECT_TRUE(document->modified);
    window.executeCommand("edit.undo");
    EXPECT_DOUBLE_EQ(document->image.sample(3, 3), 1200);
    EXPECT_DOUBLE_EQ(document->image.sample(3, 4), 0);
}

TEST(Desktop, RoiChangesDoNotPromptOnCloseButPixelChangesStillDo)
{
    for (bool closeApp : {false, true}) for (bool removeRoi : {false, true}) for (bool editPixels : {false, true}) {
        SCOPED_TRACE(::testing::Message() << "app=" << closeApp << " remove=" << removeRoi << " pixels=" << editPixels);
        MainWindow window;
        auto image = Image::allocate(PixelType::UInt8, {8, 8});
        image.metadata().name = "Cells <1>.tif";
        auto *document = window.addDocument(std::move(image));
        Roi roi; roi.points = {{1, 1}, {5, 5}};
        document->setSelection(roi);
        window.executeCommand("selection.add");
        ASSERT_EQ(document->rois.size(), 1u);
        auto edited = document->rois.front(); edited.points[1] = {6, 6};
        document->viewer->roiEdited(edited);
        EXPECT_EQ(document->rois.front().points, edited.points);
        if (removeRoi) {
            window.showPanel("rois");
            auto *list = window.panel("rois")->findChild<QListWidget *>("roiList");
            ASSERT_NE(list, nullptr); list->setCurrentRow(0);
            for (auto *button : window.panel("rois")->findChildren<QPushButton *>())
                if (button->text() == "Remove") button->click();
            ASSERT_TRUE(document->rois.empty());
        }
        EXPECT_FALSE(document->modified);
        EXPECT_FALSE(document->title().endsWith(" *"));
        if (editPixels) document->replaceImage(document->image.clone(), "Edit pixels");
        if (closeApp) {
            auto clean = Image::allocate(PixelType::UInt8, {8, 8});
            clean.metadata().name = "Unchanged.tif";
            window.addDocument(std::move(clean));
            if (editPixels) window.addDocument(Image::allocate(PixelType::UInt8, {8, 8}))->modified = true;
        }
        bool prompted = false;
        QTimer::singleShot(0, &window, [&] {
            for (auto *widget : QApplication::topLevelWidgets()) if (auto *dialog = qobject_cast<QMessageBox *>(widget)) {
                prompted = true;
                EXPECT_TRUE(dialog->text().contains("Cells <1>.tif"));
                EXPECT_TRUE(dialog->text().contains("unsaved pixel changes"));
                EXPECT_FALSE(dialog->text().contains("Unchanged.tif"));
                EXPECT_FALSE(dialog->text().contains("session", Qt::CaseInsensitive));
                EXPECT_EQ(dialog->textFormat(), Qt::PlainText);
                if (closeApp) EXPECT_TRUE(dialog->text().contains("Untitled"));
                EXPECT_EQ(dialog->defaultButton(), dialog->button(QMessageBox::Cancel));
                dialog->done(QMessageBox::Cancel);
            }
        });
        QCloseEvent close;
        QCoreApplication::sendEvent(closeApp ? static_cast<QWidget *>(&window) : document, &close);
        QCoreApplication::processEvents();
        EXPECT_EQ(prompted, editPixels);
        EXPECT_EQ(close.isAccepted(), !editPixels);
    }
}

TEST(Desktop, MeasurementsWarnOnWindowCloseAndVisibleAppExitUntilExported)
{
    for (bool closeApp : {false, true}) for (bool exportCsv : {false, true}) {
        QTemporaryDir directory;
        MainWindow window;
        auto image = Image::allocate(PixelType::UInt8, {8, 8});
        image.metadata().name = exportCsv ? "=Measured image" : "Measured image";
        std::fill_n(image.mutableData(), image.byteCount(), std::byte{7});
        auto *document = window.addDocument(std::move(image));
        window.executeCommand("analyze.measure");
        ASSERT_TRUE(finishJobs(window));
        window.showPanel("measurements");
        auto *table = window.panel("measurements")->findChild<QTableView *>();
        ASSERT_NE(table, nullptr); ASSERT_EQ(table->model()->rowCount(), 1);
        EXPECT_FALSE(document->modified);
        if (exportCsv) {
            const auto output = directory.filePath("measurements.csv");
            QTimer::singleShot(0, &window, [output] {
                for (auto *widget : QApplication::topLevelWidgets()) if (auto *dialog = qobject_cast<QFileDialog *>(widget)) {
                    dialog->setDirectory(QFileInfo(output).absolutePath());
                    dialog->selectFile(QFileInfo(output).fileName());
                    if (auto *name = dialog->findChild<QLineEdit *>("fileNameEdit")) name->setText(QFileInfo(output).fileName());
                    QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection); return;
                }
            });
            window.executeCommand("results.export");
            QFile file(output); ASSERT_TRUE(file.open(QIODevice::ReadOnly));
            const auto csv = file.readAll();
            EXPECT_TRUE(csv.contains("\"Text: =Measured image\""));
            EXPECT_TRUE(csv.contains("Slice Z1 C1 T1"));
            const auto rows = csv.trimmed().split('\n');
            ASSERT_EQ(rows.size(), 2);
            const auto columns = rows[1].split(',');
            ASSERT_EQ(columns.size(), 11);
            EXPECT_EQ(columns[2], "\"64\"");
            EXPECT_EQ(columns[4], "\"7\"");
            EXPECT_EQ(columns[5], "\"0\"");
            EXPECT_EQ(columns[8], "\"448\"");
        }
        auto *measurements = window.panel("measurements");
        ASSERT_TRUE(measurements->isVisible());
        bool prompted = false;
        QTimer::singleShot(0, &window, [&] {
            for (auto *widget : QApplication::topLevelWidgets()) if (auto *dialog = qobject_cast<QMessageBox *>(widget)) {
                prompted = true;
                EXPECT_TRUE(dialog->text().contains("unsaved measurements"));
                EXPECT_TRUE(dialog->text().contains("Measured image: Slice Z1 C1 T1"));
                EXPECT_FALSE(dialog->text().contains("pixel changes"));
                EXPECT_EQ(dialog->textFormat(), Qt::PlainText);
                EXPECT_EQ(dialog->defaultButton(), dialog->button(QMessageBox::Cancel));
                dialog->done(QMessageBox::Cancel);
            }
        });
        QCloseEvent close;
        QCoreApplication::sendEvent(closeApp ? static_cast<QWidget *>(&window) : measurements, &close);
        QCoreApplication::processEvents();
        EXPECT_EQ(prompted, !exportCsv);
        EXPECT_EQ(close.isAccepted(), exportCsv);
        if (!exportCsv) {
            EXPECT_TRUE(measurements->isVisible());
            EXPECT_EQ(table->model()->rowCount(), 1);
        }
        if (!closeApp) {
            // A real panel close hides the retained results after Don't Save.
            QTimer::singleShot(0, &window, [] {
                for (auto *widget : QApplication::topLevelWidgets()) if (auto *dialog = qobject_cast<QMessageBox *>(widget))
                    dialog->done(QMessageBox::Discard);
            });
            EXPECT_TRUE(measurements->close());
            EXPECT_FALSE(measurements->isVisible());
            prompted = false;
            QCoreApplication::processEvents();
            QTimer::singleShot(0, &window, [&] {
                for (auto *widget : QApplication::topLevelWidgets()) if (auto *dialog = qobject_cast<QMessageBox *>(widget)) {
                    prompted = true; dialog->done(QMessageBox::Cancel);
                }
            });
            QCloseEvent appClose;
            QCoreApplication::sendEvent(&window, &appClose);
            QCoreApplication::processEvents();
            EXPECT_FALSE(prompted);
            EXPECT_TRUE(appClose.isAccepted());
        }
    }
}

TEST(Desktop, NewMeasurementsAndUnsuccessfulExportsKeepCloseWarning)
{
    QTemporaryDir directory;
    MainWindow window;
    auto image = Image::allocate(PixelType::UInt8, {8, 8});
    image.metadata().name = "First image";
    window.addDocument(std::move(image));
    window.executeCommand("analyze.measure");
    ASSERT_TRUE(finishJobs(window));
    auto expectWarning = [&] {
        bool prompted = false;
        QTimer::singleShot(0, &window, [&] {
            for (auto *widget : QApplication::topLevelWidgets()) if (auto *dialog = qobject_cast<QMessageBox *>(widget)) {
                prompted = true;
                EXPECT_TRUE(dialog->text().contains("unsaved measurements"));
                dialog->done(QMessageBox::Cancel);
            }
        });
        QCloseEvent close;
        QCoreApplication::sendEvent(&window, &close);
        QCoreApplication::processEvents();
        EXPECT_TRUE(prompted);
        EXPECT_FALSE(close.isAccepted());
    };
    QTimer::singleShot(0, &window, [] {
        for (auto *widget : QApplication::topLevelWidgets()) if (auto *dialog = qobject_cast<QFileDialog *>(widget)) dialog->reject();
    });
    window.executeCommand("results.export");
    expectWarning();
    const auto output = directory.filePath("measurements.csv");
    bool failed = false;
    // Once the picker accepts, remove the destination directory to force an I/O failure.
    QTimer::singleShot(0, &window, [&] {
        for (auto *widget : QApplication::topLevelWidgets()) if (auto *dialog = qobject_cast<QFileDialog *>(widget)) {
            dialog->setDirectory(directory.path()); dialog->selectFile("measurements.csv");
            if (auto *name = dialog->findChild<QLineEdit *>("fileNameEdit")) name->setText("measurements.csv");
            QObject::connect(dialog, &QDialog::accepted, &window, [&] {
                EXPECT_TRUE(QDir(directory.path()).removeRecursively());
                QTimer::singleShot(0, &window, [&] {
                    for (auto *widget : QApplication::topLevelWidgets()) if (auto *message = qobject_cast<QMessageBox *>(widget)) {
                        failed = true; EXPECT_EQ(message->text(), "Cannot write measurements."); message->done(QMessageBox::Ok);
                    }
                });
            });
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection); return;
        }
    });
    window.executeCommand("results.export");
    EXPECT_TRUE(failed);
    expectWarning();
    ASSERT_TRUE(QDir().mkpath(directory.path()));
    QTimer::singleShot(0, &window, [&] {
        for (auto *widget : QApplication::topLevelWidgets()) if (auto *dialog = qobject_cast<QFileDialog *>(widget)) {
            dialog->setDirectory(directory.path()); dialog->selectFile("measurements.csv");
            if (auto *name = dialog->findChild<QLineEdit *>("fileNameEdit")) name->setText("measurements.csv");
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection); return;
        }
    });
    window.executeCommand("results.export");
    ASSERT_TRUE(QFile::exists(output));
    auto later = Image::allocate(PixelType::UInt8, {8, 8}); later.metadata().name = "Later image";
    window.addDocument(std::move(later));
    window.executeCommand("analyze.measure");
    ASSERT_TRUE(finishJobs(window));
    expectWarning();
    // Escape closes the same panel through the warning, rather than bypassing it.
    bool prompted = false;
    QTimer::singleShot(0, &window, [&] {
        for (auto *widget : QApplication::topLevelWidgets()) if (auto *dialog = qobject_cast<QMessageBox *>(widget)) {
            prompted = true;
            EXPECT_TRUE(dialog->text().contains("Later image"));
            EXPECT_FALSE(dialog->text().contains("First image"));
            dialog->done(QMessageBox::Cancel);
        }
    });
    sendKey(window.panel("measurements"), Qt::Key_Escape);
    EXPECT_TRUE(prompted);
    EXPECT_TRUE(window.panel("measurements")->isVisible());
}

TEST(Desktop, SavingPixelsWithRoisClearsModifiedState)
{
    QTemporaryDir directory;
    MainWindow window;
    auto image = Image::allocate(PixelType::UInt16, {8, 8, 2});
    image.metadata().name = "original.tif";
    auto *pixels = reinterpret_cast<std::uint16_t *>(image.mutableData());
    for (std::size_t i = 0; i < image.pixelCount(); ++i) pixels[i] = std::uint16_t(30000 + i * 17);
    auto *document = window.addDocument(image);
    Roi roi; roi.points = {{1, 1}, {5, 5}}; document->rois.push_back(roi);
    document->modified = true;
    document->setOrthogonalViews(true);
    for (auto *view : document->views()) EXPECT_TRUE(view->window()->windowTitle().contains("original.tif *"));
    const auto output = directory.filePath("saved.tif");
    QTimer::singleShot(0, &window, [output] {
        for (auto *widget : QApplication::topLevelWidgets()) if (auto *dialog = qobject_cast<QFileDialog *>(widget)) {
            auto *checkbox = dialog->findChild<QCheckBox *>("includeFullMetadata");
            ASSERT_NE(checkbox, nullptr);
            checkbox->setChecked(true);
            dialog->setDirectory(QFileInfo(output).absolutePath());
            dialog->selectFile(QFileInfo(output).fileName());
            if (auto *name = dialog->findChild<QLineEdit *>("fileNameEdit")) name->setText(QFileInfo(output).fileName());
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection); return;
        }
    });
    sendKey(document->viewer, Qt::Key_S, Qt::ControlModifier);
    QElapsedTimer timer; timer.start();
    while (window.jobs().busy() && timer.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(2); }
    ASSERT_FALSE(window.jobs().busy());
    const auto saved = readImage(output);
    ASSERT_EQ(saved.shape(), image.shape());
    ASSERT_EQ(saved.pixelType(), PixelType::UInt16);
    for (std::size_t z = 0; z < 2; ++z) for (std::size_t y = 0; y < 8; ++y)
        for (std::size_t x = 0; x < 8; ++x)
            EXPECT_EQ(saved.sample(x, y, z), 30000 + ((z * 8 + y) * 8 + x) * 17);
    EXPECT_FALSE(document->modified);
    EXPECT_FALSE(document->title().endsWith(" *"));
    for (auto *view : document->views()) {
        EXPECT_TRUE(view->window()->windowTitle().startsWith("saved.tif"));
        EXPECT_FALSE(view->window()->windowTitle().contains(" *"));
    }
    EXPECT_EQ(document->rois.size(), 1u);
}

