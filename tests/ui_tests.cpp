#include "mainwindow.h"
#include "io/image_io.h"
#include "core/memory.h"
#include <QLineSeries>
#include <QValueAxis>
#include "ui/theme.h"
#include <dcmtk/dcmdata/dctk.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <iterator>
#include <limits>
#include <atomic>
#include <cmath>
#include <cstring>
#include <QApplication>
#include <QAction>
#include <QKeyEvent>
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QMenu>
#include <QSet>
#include <QSettings>
#include <QScreen>
#include <QWindow>
#include <QCheckBox>
#include <QToolButton>
#include <QClipboard>
#include <QColorDialog>
#include <QImage>
#include <QJsonArray>
#include <QBuffer>
#include <QCoreApplication>
#include <QDockWidget>
#include <QTabWidget>
#include <QToolBar>
#include <QEvent>
#include <QSlider>
#include <QElapsedTimer>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTableView>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QDialog>
#include <QCloseEvent>
#include <QMessageBox>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QMimeData>
#include <QFile>
#include <QDir>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QUrl>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QLineEdit>
#include <QInputDialog>
#include <QListWidget>
#include <QTreeWidget>

using namespace imagecxx;
namespace {
void sendKey(QWidget *target, Qt::Key key, Qt::KeyboardModifiers modifiers = {})
{
    QKeyEvent overrideEvent(QEvent::ShortcutOverride, key, modifiers);
    QCoreApplication::sendEvent(target, &overrideEvent);
    QKeyEvent press(QEvent::KeyPress, key, modifiers);
    QCoreApplication::sendEvent(target, &press);
}

bool finishJobs(MainWindow &window)
{
    QElapsedTimer timer; timer.start();
    while (window.jobs().busy() && timer.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(2); }
    QCoreApplication::processEvents();
    return !window.jobs().busy();
}

void chooseDrawingColor(MainWindow &window, QColor color, bool accept = true)
{
    QTimer::singleShot(0, &window, [color, accept] {
        auto *dialog = qobject_cast<QColorDialog *>(QApplication::activeModalWidget());
        ASSERT_NE(dialog, nullptr);
        dialog->setCurrentColor(color);
        if (accept) dialog->accept(); else dialog->reject();
    });
    window.commands().execute("edit.color");
}

QString saveImage(MainWindow &window, const QString &path)
{
    QString error;
    QTimer picker;
    QObject::connect(&picker, &QTimer::timeout, &window, [&] {
        auto *modal = QApplication::activeModalWidget();
        if (auto *dialog = qobject_cast<QFileDialog *>(modal)) {
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
}

TEST(Shortcuts, BindingsHaveNoConflicts)
{
    MainWindow window;
    QMap<QKeySequence, QString> used;
    for (auto *action : window.findChildren<QAction *>()) {
        if (action->objectName().isEmpty()) continue;
        for (const auto &key : action->shortcuts()) {
            EXPECT_FALSE(used.contains(key)) << key.toString().toStdString() << " " << action->objectName().toStdString();
            used.insert(key, action->objectName());
        }
    }

}

TEST(Shortcuts, BareAndControlAliasesWorkAcrossWindowsWithoutStealingText)
{
    MainWindow window; window.show();
    auto *doc = window.addDocument(Image::allocate(PixelType::UInt8, {8, 8}));
    sendKey(doc->viewer, Qt::Key_J, Qt::ControlModifier);
    EXPECT_TRUE(window.panel("display")->isVisible());
    window.panel("display")->hide();
    sendKey(&window, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_TRUE(window.panel("display")->isVisible());
    QLineEdit editor(&window); editor.setText("shortcut text"); editor.selectAll();
    sendKey(&editor, Qt::Key_A, Qt::ControlModifier);
    EXPECT_EQ(editor.selectedText(), "shortcut text");
    sendKey(&editor, Qt::Key_C, Qt::ShiftModifier);
    EXPECT_FALSE(doc->selection.has_value());
    QDialog dialog(&window); dialog.setModal(true); dialog.show(); QCoreApplication::processEvents();
    window.panel("display")->hide();
    sendKey(&dialog, Qt::Key_C, Qt::ShiftModifier);
    EXPECT_FALSE(window.panel("display")->isVisible());
    dialog.hide();
}

TEST(Shortcuts, SelectionShortcutsKeepSavedRoisAndSupportRestoreAndArrows)
{
    MainWindow window;
    auto *doc = window.addDocument(Image::allocate(PixelType::UInt8, {16, 12}));
    Roi saved; saved.points = {{2, 2}, {6, 6}}; doc->rois.push_back(saved); doc->changed();
    sendKey(doc->viewer, Qt::Key_A);
    ASSERT_TRUE(doc->selection.has_value()); EXPECT_EQ(doc->selection->bounds(), QRectF(0, 0, 16, 12));
    sendKey(doc->viewer, Qt::Key_A, Qt::ShiftModifier);
    EXPECT_TRUE(doc->selectionCleared); EXPECT_EQ(doc->rois.size(), 1u);
    sendKey(doc->viewer, Qt::Key_E, Qt::ShiftModifier);
    ASSERT_TRUE(doc->selection.has_value());
    sendKey(doc->viewer, Qt::Key_Right);
    EXPECT_DOUBLE_EQ(doc->selection->bounds().left(), 1);
    sendKey(doc->viewer, Qt::Key_Left, Qt::AltModifier);
    EXPECT_DOUBLE_EQ(doc->selection->bounds().width(), 15);
    sendKey(doc->viewer, Qt::Key_T);
    EXPECT_EQ(doc->rois.size(), 2u);
    sendKey(doc->viewer, Qt::Key_T);
    EXPECT_EQ(doc->rois.size(), 2u);
}

TEST(Shortcuts, SavedRoiKeyboardEditsSurviveReselectionAndKeepSavedSlice)
{
    MainWindow window;
    auto *doc = window.addDocument(Image::allocate(PixelType::UInt8, {16, 12, 3}));
    Roi roi; roi.points = {{2, 2}, {6, 6}};
    doc->rois.push_back(roi); doc->setSelection(roi);
    doc->viewer->setSlice(2);
    sendKey(doc->viewer, Qt::Key_Right);
    sendKey(doc->viewer, Qt::Key_Down, Qt::AltModifier);
    EXPECT_EQ(doc->rois.front().bounds(), QRectF(3, 2, 4, 5));
    EXPECT_EQ(doc->rois.front().z, 0u);
    window.commands().execute("selection.none");
    auto *list = window.panel("rois")->findChild<QListWidget *>("roiList");
    ASSERT_NE(list, nullptr);
    list->setCurrentRow(0);
    ASSERT_TRUE(doc->selection);
    EXPECT_EQ(doc->selection->bounds(), QRectF(3, 2, 4, 5));
    EXPECT_EQ(doc->viewer->slice(), 0u);
}

TEST(Desktop, CropAndDuplicatePreserveStackAxesPixelsAndOrigins)
{
    for (const auto *command : {"image.crop", "image.duplicate"}) {
        SCOPED_TRACE(command);
        MainWindow window;
        ImageMetadata metadata;
        metadata.axes = {{"X", 2, 10, "mm"}, {"Y", 3, 20, "mm"}, {"Z", 4, 30, "mm"},
                         {"C", 1, 0, ""}, {"T", 5, 0, "s"}};
        auto image = Image::allocate(PixelType::UInt16, {6, 5, 3, 2, 2}, metadata);
        auto *pixels = reinterpret_cast<std::uint16_t *>(image.mutableData());
        for (std::size_t i = 0; i < image.pixelCount(); ++i) pixels[i] = std::uint16_t(i);
        auto *doc = window.addDocument(image);
        Roi roi; roi.points = {{1, 2}, {4, 4}}; doc->setSelection(roi);
        ASSERT_NO_THROW(window.commands().execute(command));
        const auto &output = window.activeDocument()->image;
        EXPECT_EQ(output.shape(), (std::vector<std::size_t>{3, 2, 3, 2, 2}));
        EXPECT_DOUBLE_EQ(output.metadata().axes[0].origin, 12);
        EXPECT_DOUBLE_EQ(output.metadata().axes[1].origin, 26);
        EXPECT_DOUBLE_EQ(output.metadata().axes[2].origin, 30);
        for (std::size_t t = 0; t < 2; ++t) for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t z = 0; z < 3; ++z) for (std::size_t y = 0; y < 2; ++y)
                for (std::size_t x = 0; x < 3; ++x)
                    EXPECT_EQ(output.sample(x, y, z, c, t), image.sample(x + 1, y + 2, z, c, t));
        if (QString(command) == "image.duplicate") EXPECT_EQ(doc->image.shape(), image.shape());
    }
}

TEST(Desktop, BatchChooserUsesOnlyRegisteredProcessingOperations)
{
    MainWindow window;
    bool inspected = false;
    QTimer::singleShot(0, &window, [&] {
        for (auto *widget : QApplication::topLevelWidgets()) if (auto *dialog = qobject_cast<QInputDialog *>(widget)) {
            inspected = true;
            const auto items = dialog->comboBoxItems();
            EXPECT_TRUE(items.contains(window.commands().command("process.gaussian").label));
            EXPECT_TRUE(items.contains(window.commands().command("process.resample").label));
            EXPECT_FALSE(items.contains(window.commands().command("process.repeat").label));
            EXPECT_FALSE(items.contains(window.commands().command("process.batch").label));
            dialog->reject();
        }
    });
    window.commands().execute("process.batch");
    EXPECT_TRUE(inspected);
}

TEST(Shortcuts, ClipboardImportKeepsSixteenBitGrayscale)
{
    MainWindow window;
    QImage raster(3, 2, QImage::Format_Grayscale16);
    for (int y = 0; y < raster.height(); ++y) std::fill_n(reinterpret_cast<quint16 *>(raster.scanLine(y)), 3, quint16{45000});
    QApplication::clipboard()->setImage(raster);
    window.executeCommand("file.clipboard");
    ASSERT_NE(window.activeDocument(), nullptr);
    EXPECT_EQ(window.activeDocument()->image.pixelType(), PixelType::UInt16);
    EXPECT_DOUBLE_EQ(window.activeDocument()->image.sample(2, 1), 45000);
}

TEST(Clipboard, CopyPublishesPngAtNativeSizeWithAMinimumOf320OnOneAxis)
{
    MainWindow window;
    const std::vector<std::pair<QSize, QSize>> sizes{
        {{2, 2}, {320, 320}}, {{3, 2}, {320, 213}}, {{320, 320}, {320, 320}},
        {{640, 480}, {640, 480}}, {{640, 100}, {640, 100}}, {{100, 640}, {100, 640}},
        {{4096, 1}, {4096, 1}}, {{1, 4096}, {1, 4096}}};
    for (const auto &[sourceSize, pngSize] : sizes) {
        SCOPED_TRACE(std::to_string(sourceSize.width()) + "x" + std::to_string(sourceSize.height()));
        auto image = Image::allocate(PixelType::UInt16, {std::size_t(sourceSize.width()), std::size_t(sourceSize.height())});
        image.setValue(std::array<std::size_t, 2>{0, 0}, 1200);
        image.setValue(std::array<std::size_t, 2>{image.width() - 1, image.height() - 1}, 65000);
        auto *doc = window.addDocument(image);
        doc->viewer->setWindow(1200, 65000);
        doc->viewer->setZoom(2);
        window.commands().execute("edit.copy");
        ASSERT_TRUE(finishJobs(window));
        const auto *mime = QApplication::clipboard()->mimeData(); ASSERT_NE(mime, nullptr);
        ASSERT_TRUE(mime->hasFormat("image/png"));
        const auto bytes = mime->data("image/png");
        EXPECT_TRUE(bytes.startsWith(QByteArray::fromHex("89504e470d0a1a0a")));
        const auto png = QImage::fromData(bytes, "PNG"); ASSERT_FALSE(png.isNull());
        EXPECT_EQ(png.size(), pngSize);
        EXPECT_EQ(QApplication::clipboard()->image().size(), pngSize);
        EXPECT_EQ(png.pixelColor(0, 0), QColor(Qt::black));
        EXPECT_EQ(png.pixelColor(png.width() - 1, png.height() - 1), QColor(Qt::white));
        EXPECT_EQ(doc->revision, 0u);
    }
}

TEST(Clipboard, TypedSelectionSurvivesSourceEditsAndWindowClosure)
{
    for (const auto type : {PixelType::UInt8, PixelType::UInt16, PixelType::Int16, PixelType::Float32,
                           PixelType::Float64, PixelType::RGB8, PixelType::RGBA8}) {
        SCOPED_TRACE(pixelTypeName(type).toStdString());
        MainWindow destination;
        Image expected;
        {
            MainWindow source;
            auto image = Image::allocate(type, {5, 4, 2, 2, 2});
            image.metadata().sourcePath = "source.tif";
            image.metadata().axes[0].spacing = .25;
            image.metadata().intensityScale = 2;
            for (std::size_t y = 0; y < 4; ++y) for (std::size_t x = 0; x < 5; ++x)
                for (std::size_t c = 0; c < componentCount(type); ++c)
                    image.setValue(std::array<std::size_t, 5>{x, y, 1, 1, 1}, 10 * y + x + c + .123456789, c);
            auto *doc = source.addDocument(image);
            doc->viewer->setSlice(1, 1, 1);
            Roi roi; roi.points = {{1, 1}, {4, 3}}; roi.z = roi.channel = roi.time = 1;
            doc->setSelection(roi);
            expected = image.region(std::array<std::size_t, 5>{1, 1, 1, 1, 1},
                                    std::array<std::size_t, 5>{3, 2, 1, 1, 1}).clone();
            source.commands().execute("edit.copy");
            doc->replaceImage(Image::allocate(type, {1, 1}), "Replace source");
        }
        destination.commands().execute("edit.paste");
        auto *pasted = destination.activeDocument(); ASSERT_NE(pasted, nullptr);
        EXPECT_EQ(pasted->image.shape(), (std::vector<std::size_t>{3, 2}));
        EXPECT_EQ(pasted->image.pixelType(), type);
        EXPECT_EQ(std::memcmp(pasted->image.data(), expected.data(), expected.byteCount()), 0);
        EXPECT_DOUBLE_EQ(pasted->image.metadata().axes[0].spacing, .25);
        EXPECT_DOUBLE_EQ(pasted->image.metadata().intensityScale, 2);
        EXPECT_TRUE(pasted->image.metadata().sourcePath.isEmpty());
        EXPECT_TRUE(pasted->modified);
    }
}

TEST(Clipboard, TypedCopyIsAvailableWhilePngWaitsForAWorker)
{
    std::atomic<bool> release{false};
    MainWindow window;
    for (int i = 0; i < 2; ++i) window.jobs().submit("Hold worker", [&](auto stop, auto) {
        while (!release.load() && !stop.stop_requested()) QThread::msleep(1);
    }, {});
    auto image = Image::allocate(PixelType::UInt16, {2, 1});
    image.setValue(std::array<std::size_t, 2>{0, 0}, 1000);
    image.setValue(std::array<std::size_t, 2>{1, 0}, 50000);
    auto *source = window.addDocument(image);
    source->viewer->setWindow(1000, 50000);
    window.commands().execute("edit.copy");
    ASSERT_FALSE(QApplication::clipboard()->mimeData()->hasFormat("image/png"));
    window.commands().execute("edit.pasteSpecial", {{"destination", "New image"}});
    ASSERT_EQ(window.documents().size(), 2);
    EXPECT_DOUBLE_EQ(window.activeDocument()->image.sample(1, 0), 50000);
    source->viewer->setWindow(0, 1);
    source->replaceImage(Image::allocate(PixelType::UInt16, {1, 1}), "Replace source");
    bool timerRan = false;
    QTimer::singleShot(0, &window, [&] { timerRan = true; });
    QCoreApplication::processEvents();
    EXPECT_TRUE(timerRan);
    EXPECT_FALSE(QApplication::clipboard()->mimeData()->hasFormat("image/png"));
    release.store(true);
    ASSERT_TRUE(finishJobs(window));
    const auto png = QImage::fromData(QApplication::clipboard()->mimeData()->data("image/png"), "PNG");
    ASSERT_EQ(png.size(), QSize(320, 160));
    EXPECT_EQ(png.pixelColor(0, 0), QColor(Qt::black));
    EXPECT_EQ(png.pixelColor(319, 0), QColor(Qt::white));
}

TEST(Clipboard, PngExportFailureKeepsTheTypedCopy)
{
    MainWindow window;
    QString failure;
    QObject::connect(window.statusBar(), &QStatusBar::messageChanged, &window, [&](const QString &message) {
        if (message.contains("PNG export unavailable")) failure = message;
    });
    auto image = Image::allocate(PixelType::UInt8, {8192, 4096});
    image.setValue(std::array<std::size_t, 2>{0, 0}, 37);
    auto *source = window.addDocument(image);
    window.commands().execute("edit.copy");
    ASSERT_TRUE(finishJobs(window));
    EXPECT_TRUE(failure.contains("clipboard memory limit"));
    EXPECT_FALSE(QApplication::clipboard()->mimeData()->hasFormat("image/png"));
    window.commands().execute("edit.pasteSpecial", {{"destination", "New image"}});
    ASSERT_EQ(window.documents().size(), 2);
    EXPECT_EQ(window.activeDocument()->image.shape(), image.shape());
    EXPECT_EQ(window.activeDocument()->image.pixelType(), PixelType::UInt8);
    EXPECT_DOUBLE_EQ(window.activeDocument()->image.sample(0, 0), 37);
    EXPECT_EQ(source->revision, 0u);
}

TEST(Clipboard, PendingExportsDoNotReplaceNewCopiesOrExternalClipboardContent)
{
    for (int scenario = 0; scenario < 3; ++scenario) {
        SCOPED_TRACE(scenario);
        std::atomic<bool> release{false};
        MainWindow window;
        for (int i = 0; i < 2; ++i) window.jobs().submit("Hold worker", [&](auto stop, auto) {
            while (!release.load() && !stop.stop_requested()) QThread::msleep(1);
        }, {});
        auto image = Image::allocate(PixelType::UInt16, {2, 1});
        image.setValue(std::array<std::size_t, 2>{1, 0}, 50000);
        auto *source = window.addDocument(image);
        window.commands().execute("edit.copy");
        if (scenario == 0) {
            auto newer = Image::allocate(PixelType::UInt16, {3, 2});
            newer.setValue(std::array<std::size_t, 2>{1, 0}, 999);
            source->replaceImage(newer, "New pixels");
            window.commands().execute("edit.copy");
        } else if (scenario == 1) {
            QApplication::clipboard()->setText("New clipboard content");
        } else {
            window.jobs().cancelAll();
        }
        release.store(true);
        ASSERT_TRUE(finishJobs(window));
        if (scenario == 1) {
            EXPECT_EQ(QApplication::clipboard()->text(), "New clipboard content");
            EXPECT_FALSE(QApplication::clipboard()->mimeData()->hasFormat("image/png"));
        } else {
            EXPECT_EQ(QApplication::clipboard()->mimeData()->hasFormat("image/png"), scenario == 0);
            window.commands().execute("edit.pasteSpecial", {{"destination", "New image"}});
            const auto *pasted = window.activeDocument(); ASSERT_NE(pasted, source);
            EXPECT_EQ(pasted->image.pixelType(), PixelType::UInt16);
            EXPECT_EQ(pasted->image.width(), scenario == 0 ? 3u : 2u);
            EXPECT_DOUBLE_EQ(pasted->image.sample(1, 0), scenario == 0 ? 999 : 50000);
        }
    }
}

TEST(Clipboard, CutOfAThinImageKeepsNativePngDimensionsAndSupportsUndo)
{
    MainWindow window;
    auto image = Image::allocate(PixelType::UInt16, {4096, 1});
    std::fill_n(reinterpret_cast<std::uint16_t *>(image.mutableData()), 4096, std::uint16_t{1200});
    auto *source = window.addDocument(image);
    window.commands().execute("edit.cut");
    ASSERT_TRUE(finishJobs(window));
    const auto png = QImage::fromData(QApplication::clipboard()->mimeData()->data("image/png"), "PNG");
    EXPECT_EQ(png.size(), QSize(4096, 1));
    EXPECT_DOUBLE_EQ(source->image.sample(0, 0), 0);
    window.commands().execute("edit.undo");
    EXPECT_DOUBLE_EQ(source->image.sample(0, 0), 1200);
    window.commands().execute("edit.pasteSpecial", {{"destination", "New image"}});
    ASSERT_NE(window.activeDocument(), source);
    EXPECT_EQ(window.activeDocument()->image.shape(), image.shape());
    EXPECT_DOUBLE_EQ(window.activeDocument()->image.sample(0, 0), 1200);
}

TEST(Clipboard, PasteSpecialChoosesTypedOrSystemPixelsInANewImage)
{
    MainWindow window;
    auto image = Image::allocate(PixelType::UInt16, {2, 1});
    image.setValue(std::array<std::size_t, 2>{0, 0}, 1000);
    image.setValue(std::array<std::size_t, 2>{1, 0}, 50000);
    auto *source = window.addDocument(image);
    source->viewer->setWindow(1000, 50000);
    window.commands().execute("edit.copy");
    window.commands().execute("edit.pasteSpecial", {{"destination", "New image"}});
    ASSERT_EQ(window.documents().size(), 2);
    auto *typed = window.activeDocument(); ASSERT_NE(typed, source);
    EXPECT_EQ(typed->image.pixelType(), PixelType::UInt16);
    EXPECT_EQ(typed->image.shape(), image.shape());
    EXPECT_DOUBLE_EQ(typed->image.sample(1, 0), 50000);
    ASSERT_TRUE(finishJobs(window));

    bool inspected = false;
    QTimer::singleShot(0, &window, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) return;
        auto *source = dialog->findChild<QComboBox *>("source");
        auto *destination = dialog->findChild<QComboBox *>("destination");
        EXPECT_NE(source, nullptr); EXPECT_NE(destination, nullptr);
        if (source && destination) {
            source->setCurrentText("System image");
            destination->setCurrentText("New image");
            inspected = true;
        }
        dialog->accept();
    });
    window.executeCommand("edit.pasteSpecial");
    ASSERT_TRUE(inspected);
    ASSERT_EQ(window.documents().size(), 3);
    const auto *rendered = window.activeDocument();
    EXPECT_EQ(rendered->image.pixelType(), PixelType::RGBA8);
    EXPECT_EQ(rendered->image.shape(), (std::vector<std::size_t>{320, 160}));
    EXPECT_DOUBLE_EQ(rendered->image.sample(0, 0), 0);
    EXPECT_DOUBLE_EQ(rendered->image.sample(319, 0), 255);
    EXPECT_EQ(source->revision, 0u); EXPECT_EQ(typed->revision, 0u);
}

TEST(Clipboard, ExternalPngReplacesTypedCopyAndPastesIntoCurrentPlaneWithUndo)
{
    MainWindow window;
    auto *source = window.addDocument(Image::allocate(PixelType::UInt16, {1, 1}));
    window.commands().execute("edit.copy");
    QImage raster(3, 2, QImage::Format_RGBA8888);
    raster.fill(QColor(12, 34, 56, 78));
    QBuffer png; ASSERT_TRUE(png.open(QIODevice::WriteOnly)); ASSERT_TRUE(raster.save(&png, "PNG"));
    auto *mime = new QMimeData;
    mime->setData("image/png", png.data());
    QApplication::clipboard()->setMimeData(mime);
    auto *target = window.addDocument(Image::allocate(PixelType::RGBA8, {4, 4, 2, 2, 2}));
    target->viewer->setSlice(1, 1, 1);
    Roi roi; roi.points = {{-1, 1}, {2, 3}}; roi.z = roi.channel = roi.time = 1;
    target->setSelection(roi);
    window.commands().execute("edit.paste");
    QApplication::clipboard()->setText("Later clipboard content");
    target->viewer->setSlice(0, 0, 0);
    ASSERT_TRUE(finishJobs(window));
    const std::array<double, 4> expected{12, 34, 56, 78};
    for (std::size_t c = 0; c < 4; ++c) {
        EXPECT_DOUBLE_EQ(target->image.sample(0, 1, 1, 1, 1, c), expected[c]);
        EXPECT_DOUBLE_EQ(target->image.sample(1, 2, 1, 1, 1, c), expected[c]);
        EXPECT_DOUBLE_EQ(target->image.sample(2, 1, 1, 1, 1, c), 0);
        EXPECT_DOUBLE_EQ(target->image.sample(0, 1, 0, 0, 0, c), 0);
    }
    EXPECT_EQ(source->revision, 0u);
    QEvent activate(QEvent::WindowActivate); QCoreApplication::sendEvent(target, &activate);
    window.commands().execute("edit.undo");
    EXPECT_DOUBLE_EQ(target->image.sample(0, 1, 1, 1, 1, 3), 0);
    const auto revision = target->revision;
    window.commands().execute("edit.paste");
    EXPECT_FALSE(window.jobs().busy()); EXPECT_EQ(target->revision, revision);
}

TEST(Shortcuts, FillCopyPasteInvertAndUndoPreserveScientificValues)
{
    MainWindow window;
    auto image = Image::allocate(PixelType::UInt16, {8, 8});
    std::fill_n(reinterpret_cast<std::uint16_t *>(image.mutableData()), 64, std::uint16_t{1200});
    auto *doc = window.addDocument(image);
    Roi selection; selection.points = {{1, 1}, {3, 3}}; doc->setSelection(selection);
    sendKey(doc->viewer, Qt::Key_C);
    sendKey(doc->viewer, Qt::Key_F); ASSERT_TRUE(finishJobs(window));
    EXPECT_DOUBLE_EQ(doc->image.sample(1, 1), 65535); EXPECT_DOUBLE_EQ(doc->image.sample(0, 0), 1200);
    sendKey(doc->viewer, Qt::Key_V); ASSERT_TRUE(finishJobs(window));
    EXPECT_DOUBLE_EQ(doc->image.sample(1, 1), 1200);
    sendKey(doc->viewer, Qt::Key_I, Qt::ShiftModifier); ASSERT_TRUE(finishJobs(window));
    EXPECT_DOUBLE_EQ(doc->image.sample(1, 1), 64335);
    sendKey(doc->viewer, Qt::Key_Z, Qt::ControlModifier);
    EXPECT_DOUBLE_EQ(doc->image.sample(1, 1), 1200);
}

TEST(Desktop, DrawingColorUsesScientificTypesAndPreservesAlphaAndOtherPlanes)
{
    for (const auto type : {PixelType::UInt8, PixelType::UInt16, PixelType::Int16,
                           PixelType::Float32, PixelType::Float64, PixelType::RGB8, PixelType::RGBA8}) {
        SCOPED_TRACE(pixelTypeName(type).toStdString());
        MainWindow window;
        chooseDrawingColor(window, QColor(255, 0, 0));
        chooseDrawingColor(window, Qt::blue, false);
        auto *action = window.findChild<QAction *>("edit.color"); ASSERT_NE(action, nullptr);
        EXPECT_EQ(action->icon().pixmap(24, 24).toImage().pixelColor(12, 12), QColor(Qt::red));
        auto image = Image::allocate(type, {6, 6, 2, 2, 2});
        const bool floating = type == PixelType::Float32 || type == PixelType::Float64;
        const double low = type == PixelType::Float64 ? 1e12 : floating ? -10 : 0;
        const double high = type == PixelType::Float64 ? 1e12 + .25 : floating ? 30 : 100;
        if (floating) for (std::size_t y = 0; y < 6; ++y) for (std::size_t x = 0; x < 6; ++x)
            image.setValue(std::array<std::size_t, 5>{x, y, 1, 1, 1}, low);
        image.setValue(std::array<std::size_t, 5>{0, 0, 1, 1, 1}, low);
        image.setValue(std::array<std::size_t, 5>{5, 5, 1, 1, 1}, high);
        if (type == PixelType::RGBA8) image.setValue(std::array<std::size_t, 5>{1, 1, 1, 1, 1}, 23, 3);
        image.metadata().intensityScale = 2; image.metadata().intensityOffset = 100;
        auto *doc = window.addDocument(image);
        doc->viewer->setSlice(1, 1, 1);
        doc->viewer->setLut("hot"); doc->viewer->setWindow(1000, 2000);
        Roi roi; roi.points = {{1, 1}, {5, 5}}; roi.z = roi.channel = roi.time = 1;
        doc->setSelection(roi);
        const double gray = qGray(qRgb(255, 0, 0)) / 255.0;
        const double expected = floating ? std::lerp(low, high, gray)
            : std::round(std::lerp(type == PixelType::Int16 ? -32768.0 : 0.0,
                type == PixelType::UInt16 ? 65535.0 : type == PixelType::Int16 ? 32767.0 : 255.0, gray));
        for (const auto *command : {"edit.draw", "edit.fill"}) {
            window.commands().execute(command); ASSERT_TRUE(finishJobs(window));
            for (std::size_t component = 0; component < componentCount(type); ++component) {
                const double value = component == 3 ? 23 : componentCount(type) > 1 ? (component == 0 ? 255 : 0) : expected;
                EXPECT_NEAR(doc->image.sample(1, 1, 1, 1, 1, component), value, type == PixelType::Float32 ? 1e-5 : 0);
            }
            const double stored = componentCount(type) > 1 ? 255 : type == PixelType::Float32 ? double(float(expected)) : expected;
            EXPECT_DOUBLE_EQ(doc->image.sample(3, 3, 1, 1, 1), QString(command) == "edit.draw" ? low : stored);
            EXPECT_DOUBLE_EQ(doc->image.sample(0, 0, 1, 1, 1), low);
            EXPECT_DOUBLE_EQ(doc->image.sample(5, 5, 1, 1, 1), high);
            EXPECT_DOUBLE_EQ(doc->image.sample(1, 1, 0, 1, 1), 0);
            EXPECT_DOUBLE_EQ(doc->image.sample(1, 1, 1, 0, 1), 0);
            EXPECT_DOUBLE_EQ(doc->image.sample(1, 1, 1, 1, 0), 0);
            EXPECT_EQ(doc->image.pixelType(), type);
            EXPECT_DOUBLE_EQ(doc->image.metadata().intensityScale, 2);
            EXPECT_DOUBLE_EQ(doc->image.metadata().intensityOffset, 100);
            window.commands().execute("edit.undo");
            EXPECT_DOUBLE_EQ(doc->image.sample(1, 1, 1, 1, 1), image.sample(1, 1, 1, 1, 1));
            window.commands().execute("edit.redo");
            EXPECT_DOUBLE_EQ(doc->image.sample(1, 1, 1, 1, 1), stored);
            window.commands().execute("edit.undo");
        }
        window.commands().execute("edit.clear"); ASSERT_TRUE(finishJobs(window));
        EXPECT_DOUBLE_EQ(doc->image.sample(1, 1, 1, 1, 1), 0);
    }
}

TEST(Desktop, FloatDrawingHandlesConstantExtremeAndNonFiniteRanges)
{
    for (const auto type : {PixelType::Float32, PixelType::Float64}) {
        MainWindow window;
        chooseDrawingColor(window, Qt::black);
        auto image = Image::allocate(type, {2, 2});
        for (std::size_t y = 0; y < 2; ++y) for (std::size_t x = 0; x < 2; ++x)
            image.setValue(std::array<std::size_t, 2>{x, y}, -42.5);
        auto *doc = window.addDocument(image);
        window.commands().execute("edit.fill"); ASSERT_TRUE(finishJobs(window));
        EXPECT_DOUBLE_EQ(doc->image.sample(0, 0), -42.5);

        const double limit = type == PixelType::Float32 ? std::numeric_limits<float>::max() : std::numeric_limits<double>::max();
        image.setValue(std::array<std::size_t, 2>{0, 0}, -limit);
        image.setValue(std::array<std::size_t, 2>{1, 0}, limit);
        doc = window.addDocument(image);
        chooseDrawingColor(window, QColor(128, 128, 128));
        window.commands().execute("edit.fill"); ASSERT_TRUE(finishJobs(window));
        EXPECT_TRUE(std::isfinite(doc->image.sample(0, 0)));
        EXPECT_EQ(doc->image.pixelType(), type);

        for (std::size_t y = 0; y < 2; ++y) for (std::size_t x = 0; x < 2; ++x)
            image.setValue(std::array<std::size_t, 2>{x, y}, std::numeric_limits<double>::quiet_NaN());
        doc = window.addDocument(image);
        QString error;
        QTimer dismiss;
        QObject::connect(&dismiss, &QTimer::timeout, &window, [&error] {
            if (auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                error = dialog->text(); dialog->accept();
            }
        });
        dismiss.start(1);
        window.commands().execute("edit.fill"); ASSERT_TRUE(finishJobs(window));
        EXPECT_TRUE(error.contains("finite intensity range"));
        EXPECT_TRUE(std::isnan(doc->image.sample(0, 0)));
        EXPECT_EQ(doc->revision, 0u);
    }
}

TEST(Desktop, FloatInvertUsesRawLimitsAndKeepsCalibration)
{
    for (const auto type : {PixelType::Float32, PixelType::Float64}) {
        MainWindow window;
        auto image = Image::allocate(type, {2, 2});
        image.setValue(std::array<std::size_t, 2>{0, 0}, 10);
        image.setValue(std::array<std::size_t, 2>{1, 0}, 20);
        image.setValue(std::array<std::size_t, 2>{0, 1}, 10);
        image.setValue(std::array<std::size_t, 2>{1, 1}, 20);
        image.metadata().intensityScale = 2; image.metadata().intensityOffset = 100;
        auto *doc = window.addDocument(image);
        window.commands().execute("edit.invert"); ASSERT_TRUE(finishJobs(window));
        EXPECT_DOUBLE_EQ(doc->image.sample(0, 0), 20);
        EXPECT_DOUBLE_EQ(doc->image.sample(1, 0), 10);
        EXPECT_DOUBLE_EQ(doc->image.metadata().intensityScale, 2);
        EXPECT_DOUBLE_EQ(doc->image.metadata().intensityOffset, 100);
    }
}

TEST(Desktop, SmoothKeepsLargeFiniteSamplesAndOtherPlanesAndSupportsUndo)
{
    for (const double value : {1e308, std::numeric_limits<double>::max(), -std::numeric_limits<double>::max()}) {
        MainWindow window;
        auto image = Image::allocate(PixelType::Float64, {3, 3, 2});
        for (std::size_t y = 0; y < 3; ++y) for (std::size_t x = 0; x < 3; ++x) {
            image.setValue(std::array<std::size_t, 3>{x, y, 0}, value);
            image.setValue(std::array<std::size_t, 3>{x, y, 1}, 17);
        }
        auto *doc = window.addDocument(image);
        window.commands().execute("edit.smooth");
        ASSERT_TRUE(finishJobs(window));
        for (std::size_t y = 0; y < 3; ++y) for (std::size_t x = 0; x < 3; ++x) {
            EXPECT_DOUBLE_EQ(doc->image.sample(x, y, 0), value);
            EXPECT_DOUBLE_EQ(doc->image.sample(x, y, 1), 17);
        }
        EXPECT_TRUE(doc->history.canUndo());
        window.commands().execute("edit.undo");
        EXPECT_DOUBLE_EQ(doc->image.sample(1, 1), value);
    }
}

TEST(Desktop, LineProfilesSamplePixelCentersAndKeepCalibratedDistances)
{
    MainWindow window;
    auto image = Image::allocate(PixelType::UInt16, {4, 2});
    image.metadata().axes[0].spacing = image.metadata().axes[1].spacing = .25;
    image.metadata().intensityScale = 2;
    image.metadata().intensityOffset = 100;
    for (std::size_t y = 0; y < 2; ++y) for (std::size_t x = 0; x < 4; ++x)
        image.setValue(std::array<std::size_t, 2>{x, y}, 10 * x);
    auto *doc = window.addDocument(image);
    for (bool reverse : {false, true}) {
        Roi line; line.type = RoiType::Line;
        line.points = reverse ? std::vector<QPointF>{{3.5, .5}, {.5, .5}} : std::vector<QPointF>{{.5, .5}, {3.5, .5}};
        doc->setSelection(line);
        window.commands().execute("analyze.profile");
        ASSERT_TRUE(finishJobs(window));
        auto *panel = window.panel("profile");
        ASSERT_NE(panel, nullptr);
        auto *series = panel->findChild<QLineSeries *>();
        ASSERT_NE(series, nullptr);
        const auto points = series->points();
        ASSERT_EQ(points.size(), 4);
        for (qsizetype i = 0; i < points.size(); ++i) {
            EXPECT_DOUBLE_EQ(points[i].x(), double(i) * .25);
            EXPECT_DOUBLE_EQ(points[i].y(), 100 + 20 * (reverse ? 3 - i : i));
        }
    }
}

TEST(Desktop, UnrepresentableLineProfileShowsAnErrorAndFinishesItsJob)
{
    MainWindow window;
    auto image = Image::allocate(PixelType::Float64, {2, 2});
    for (std::size_t y = 0; y < 2; ++y) {
        image.setValue(std::array<std::size_t, 2>{0, y}, -1e308);
        image.setValue(std::array<std::size_t, 2>{1, y}, 1e308);
    }
    auto *doc = window.addDocument(image);
    Roi line; line.type = RoiType::Line; line.points = {{.5, .5}, {1.5, .5}};
    doc->setSelection(line);
    QString error;
    QTimer dismiss;
    QObject::connect(&dismiss, &QTimer::timeout, [&] {
        if (auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            error = dialog->text(); dialog->accept();
        }
    });
    dismiss.start(1);
    window.commands().execute("analyze.profile");
    ASSERT_TRUE(finishJobs(window));
    EXPECT_TRUE(error.contains("too wide")) << error.toStdString();
    ASSERT_NE(window.panel("profile"), nullptr);
    EXPECT_FALSE(window.panel("profile")->isVisible());
}

TEST(Desktop, RoiMeasurementPlotsPreserveCalibrationScopeAndExportSnapshotStatistics)
{
    for (bool overTime : {true, false}) {
        SCOPED_TRACE(overTime);
        MainWindow window;
        ImageMetadata metadata;
        metadata.name = "Sample,\"A\"";
        metadata.axes = {{"X", .5, 0, "um"}, {"Y", .25, 0, "um"}, {"Z", 2, 10, "um"},
                         {"C", 1, 0, ""}, {"T", .5, 7, "s"}};
        metadata.intensityScale = 2; metadata.intensityOffset = 3; metadata.intensityUnit = "a.u.";
        auto image = Image::allocate(PixelType::UInt16, {3, 2, 3, 2, 3}, metadata);
        for (std::size_t t = 0; t < 3; ++t) for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t z = 0; z < 3; ++z) for (std::size_t y = 0; y < 2; ++y)
                for (std::size_t x = 0; x < 3; ++x)
                    image.setValue(std::array<std::size_t, 5>{x, y, z, c, t},
                        y == 0 && x < 2 ? 100 * t + 10 * c + z + 2 * x : 10000);
        auto *doc = window.addDocument(image);
        doc->setPosition(0, 0, 1, 1, 1);
        Roi roi; roi.name = "ROI,\"one\""; roi.points = {{0, 0}, {2, 1}};
        roi.channel = 1; roi.time = 1;
        doc->setSelection(roi);
        EXPECT_EQ(window.findChild<QDialog *>("roiMeasurementsWindow"), nullptr);
        // Exercise the generated parameter form used by the menu command.
        QTimer::singleShot(0, &window, [overTime] {
            auto *form = qobject_cast<QDialog *>(QApplication::activeModalWidget()); ASSERT_NE(form, nullptr);
            auto *axis = form->findChild<QComboBox *>("axis"); ASSERT_NE(axis, nullptr);
            EXPECT_EQ(axis->currentText(), "Time");
            axis->setCurrentText(overTime ? "Time" : "Z"); form->accept();
        });
        window.executeCommand("analyze.roiPlot");
        // Results must describe the requested image and ROI, even after subsequent edits.
        doc->image.setValue(std::array<std::size_t, 5>{0, 0, 1, 1, 0}, 60000);
        doc->setSelection({}); doc->setPosition(0, 0, 0);
        ASSERT_TRUE(finishJobs(window));
        auto *plot = window.findChild<QDialog *>("roiMeasurementsWindow"); ASSERT_NE(plot, nullptr);
        auto *series = plot->findChild<QLineSeries *>(); ASSERT_NE(series, nullptr);
        const auto points = series->points(); ASSERT_EQ(points.size(), 3);
        const auto axes = plot->findChildren<QValueAxis *>(); ASSERT_EQ(axes.size(), 2);
        const QString xTitle = overTime ? "Time [s]" : "Z [um]";
        EXPECT_EQ(axes[0]->titleText(), xTitle);
        EXPECT_EQ(axes[1]->titleText(), "Mean intensity [a.u.]");
        EXPECT_TRUE(plot->windowTitle().contains(overTime ? "C2, Z2" : "C2, T2"));
        for (int i = 0; i < 3; ++i) {
            EXPECT_DOUBLE_EQ(points[i].x(), overTime ? 7 + .5 * i : 10 + 2 * i);
            EXPECT_DOUBLE_EQ(points[i].y(), overTime ? 27 + 200 * i : 225 + 2 * i);
        }
        // Completed plots remain accessible through the Window menu.
        auto *menu = window.findChild<QMenu *>("windowMenu"); ASSERT_NE(menu, nullptr);
        menu->aboutToShow(); plot->hide();
        QAction *plotAction = nullptr;
        for (auto *action : menu->actions()) if (action->text() == plot->windowTitle()) plotAction = action;
        ASSERT_NE(plotAction, nullptr); plotAction->trigger(); EXPECT_TRUE(plot->isVisible());
        QTemporaryDir directory; ASSERT_TRUE(directory.isValid());
        const auto path = directory.filePath("measurements.csv");
        auto *exportButton = plot->findChild<QPushButton *>("exportRoiMeasurements"); ASSERT_NE(exportButton, nullptr);
        QTimer picker;
        QObject::connect(&picker, &QTimer::timeout, &window, [path] {
            auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            dialog->setDirectory(QFileInfo(path).absolutePath()); dialog->selectFile(QFileInfo(path).fileName());
            if (auto *name = dialog->findChild<QLineEdit *>("fileNameEdit")) name->setText(QFileInfo(path).fileName());
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        });
        picker.start(1); exportButton->click(); picker.stop();
        QFile csv(path); ASSERT_TRUE(csv.open(QIODevice::ReadOnly));
        const auto rows = QString::fromUtf8(csv.readAll()).split('\n'); ASSERT_EQ(rows.size(), 5);
        EXPECT_EQ(rows[0], QString("\"Image\",\"ROI\",\"Z\",\"Channel\",\"Frame\",\"%1\",\"Count\",\"Area\",\"Mean\","
            "\"StdDev\",\"Min\",\"Max\",\"Sum\",\"Area unit\",\"Intensity unit\"").arg(xTitle));
        for (int i = 0; i < 3; ++i) {
            const int mean = overTime ? 27 + 200 * i : 225 + 2 * i;
            const auto &row = rows[i + 1];
            ASSERT_TRUE(row.startsWith('"') && row.endsWith('"'));
            const auto fields = row.mid(1, row.size() - 2).split("\",\""); ASSERT_EQ(fields.size(), 15);
            EXPECT_EQ(fields[0], "Sample,\"\"A\"\""); EXPECT_EQ(fields[1], "ROI,\"\"one\"\"");
            EXPECT_EQ(fields[2].toInt(), overTime ? 2 : i + 1); EXPECT_EQ(fields[3].toInt(), 2);
            EXPECT_EQ(fields[4].toInt(), overTime ? i + 1 : 2);
            EXPECT_DOUBLE_EQ(fields[5].toDouble(), points[i].x());
            EXPECT_EQ(fields[6].toInt(), 2); EXPECT_DOUBLE_EQ(fields[7].toDouble(), .25);
            EXPECT_DOUBLE_EQ(fields[8].toDouble(), points[i].y());
            EXPECT_NEAR(fields[9].toDouble(), 2, 1e-12);
            EXPECT_DOUBLE_EQ(fields[10].toDouble(), mean - 2); EXPECT_DOUBLE_EQ(fields[11].toDouble(), mean + 2);
            EXPECT_NEAR(fields[12].toDouble(), mean * 2, 1e-12);
            EXPECT_EQ(fields[13], "um²"); EXPECT_EQ(fields[14], "a.u.");
        }
        QPointer<QDialog> closed(plot); plot->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        EXPECT_TRUE(closed.isNull());
    }
}

TEST(Desktop, RoiMeasurementPlotsUseIndicesWithoutCalibrationAndSupportAreaShapes)
{
    for (const auto type : {RoiType::Rectangle, RoiType::Ellipse, RoiType::Polygon}) {
        for (bool overTime : {true, false}) {
            MainWindow window;
            auto image = Image::allocate(PixelType::Float64, {2, 2, 2, 1, 2});
            for (std::size_t t = 0; t < 2; ++t) for (std::size_t z = 0; z < 2; ++z)
                for (std::size_t y = 0; y < 2; ++y) for (std::size_t x = 0; x < 2; ++x)
                    image.setValue(std::array<std::size_t, 5>{x, y, z, 0, t}, .123456789012345 + 10 * t + z);
            auto *doc = window.addDocument(image);
            Roi roi; roi.type = type;
            roi.points = type == RoiType::Polygon ? std::vector<QPointF>{{0, 0}, {2, 0}, {2, 2}, {0, 2}}
                                                 : std::vector<QPointF>{{0, 0}, {2, 2}};
            doc->setSelection(roi);
            window.commands().execute("analyze.roiPlot", {{"axis", overTime ? "Time" : "Z"}});
            ASSERT_TRUE(finishJobs(window));
            auto *plot = window.findChild<QDialog *>("roiMeasurementsWindow"); ASSERT_NE(plot, nullptr);
            auto *series = plot->findChild<QLineSeries *>(); ASSERT_NE(series, nullptr);
            const auto points = series->points(); ASSERT_EQ(points.size(), 2);
            EXPECT_DOUBLE_EQ(points[0].x(), 1); EXPECT_DOUBLE_EQ(points[1].x(), 2);
            EXPECT_DOUBLE_EQ(points[0].y(), .123456789012345);
            EXPECT_DOUBLE_EQ(points[1].y(), .123456789012345 + (overTime ? 10 : 1));
            const auto axes = plot->findChildren<QValueAxis *>(); ASSERT_EQ(axes.size(), 2);
            EXPECT_EQ(axes[0]->titleText(), overTime ? "Frame" : "Slice");
            EXPECT_EQ(axes[1]->titleText(), "Mean intensity");
        }
    }
}

TEST(Desktop, RoiMeasurementPlotsValidateSelectionsAxesAndHonorCancellation)
{
    MainWindow window;
    auto *doc = window.addDocument(Image::allocate(PixelType::UInt8, {4, 4, 2, 1, 2}));
    QString error;
    QTimer dismiss;
    QObject::connect(&dismiss, &QTimer::timeout, [&] {
        if (auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            error = dialog->text(); dialog->accept();
        }
    });
    dismiss.start(1);
    window.commands().execute("analyze.roiPlot"); EXPECT_EQ(error, "Select an area ROI first.");
    Roi roi; roi.type = RoiType::Line; roi.points = {{0, 0}, {2, 2}}; doc->setSelection(roi);
    error.clear(); window.commands().execute("analyze.roiPlot"); EXPECT_EQ(error, "Select an area ROI first.");
    EXPECT_FALSE(window.jobs().busy());
    EXPECT_THROW(window.commands().execute("analyze.roiPlot", {{"axis", "Channel"}}), std::invalid_argument);
    roi.type = RoiType::Rectangle; doc->setSelection(roi);
    window.commands().execute("analyze.roiPlot"); window.jobs().cancelAll();
    ASSERT_TRUE(finishJobs(window));
    EXPECT_EQ(window.findChild<QDialog *>("roiMeasurementsWindow"), nullptr);
    doc = window.addDocument(Image::allocate(PixelType::UInt8, {4, 4})); doc->setSelection(roi);
    error.clear(); window.commands().execute("analyze.roiPlot"); EXPECT_EQ(error, "The image needs at least two time frames.");
    error.clear(); window.commands().execute("analyze.roiPlot", {{"axis", "Z"}});
    EXPECT_EQ(error, "The image needs at least two Z slices.");
    EXPECT_FALSE(window.jobs().busy());
    EXPECT_EQ(window.findChild<QDialog *>("roiMeasurementsWindow"), nullptr);
}

TEST(Desktop, NewImagesAllocateInBackgroundAndHonorCancellation)
{
    for (bool cancel : {false, true}) {
        MainWindow window;
        window.commands().execute("file.new", {{"width", 16}, {"height", 8}, {"depth", 2}});
        EXPECT_TRUE(window.documents().isEmpty());
        EXPECT_TRUE(window.jobs().busy());
        if (cancel) window.jobs().cancelAll();
        ASSERT_TRUE(finishJobs(window));
        if (cancel) EXPECT_TRUE(window.documents().isEmpty());
        else {
            ASSERT_EQ(window.documents().size(), 1);
            const auto *doc = window.activeDocument();
            ASSERT_NE(doc, nullptr);
            EXPECT_EQ(doc->image.shape(), (std::vector<std::size_t>{16, 8, 2}));
            EXPECT_TRUE(doc->modified);
            for (std::size_t i = 0; i < doc->image.byteCount(); ++i) EXPECT_EQ(doc->image.data()[i], std::byte{0});
        }
    }
}

TEST(Desktop, NewImagesRejectUnaffordableDimensionsBeforeAllocating)
{
    constexpr std::uint64_t required = 16384ull * 16384 * 10000;
    const auto available = detail::availableMemoryBytes();
    if (available && *available >= required) GTEST_SKIP() << "Requested image fits this machine's memory";
    MainWindow window;
    EXPECT_THROW(window.commands().execute("file.new", {{"width", 16384}, {"height", 16384}, {"depth", 10000}}), std::exception);
    EXPECT_FALSE(window.jobs().busy());
    EXPECT_TRUE(window.documents().isEmpty());
}

TEST(Desktop, BackgroundPasteCapturesItsPlaneAndClipsWithoutChangingOtherPixels)
{
    for (const auto type : {PixelType::UInt16, PixelType::Float64, PixelType::RGB8, PixelType::RGBA8}) {
        MainWindow window;
        auto source = Image::allocate(type, {3, 2});
        for (std::size_t y = 0; y < 2; ++y) for (std::size_t x = 0; x < 3; ++x)
            for (std::size_t c = 0; c < componentCount(type); ++c)
                source.setValue(std::array<std::size_t, 2>{x, y}, 1 + x + 10 * y + c, c);
        window.addDocument(source);
        window.commands().execute("edit.copy");
        auto *doc = window.addDocument(Image::allocate(type, {4, 4, 2, 2, 2}));
        doc->viewer->setSlice(1, 1, 1);
        Roi selection; selection.points = {{-1, 1}, {2, 3}}; selection.z = selection.channel = selection.time = 1;
        doc->setSelection(selection);
        window.commands().execute("edit.paste");
        EXPECT_EQ(doc->revision, 0u); // Completion must return through the UI event loop.
        doc->viewer->setSlice(0, 0, 0); doc->setSelection({});
        ASSERT_TRUE(finishJobs(window));
        for (std::size_t c = 0; c < componentCount(type); ++c) {
            EXPECT_DOUBLE_EQ(doc->image.sample(0, 1, 1, 1, 1, c), source.sample(1, 0, 0, 0, 0, c));
            EXPECT_DOUBLE_EQ(doc->image.sample(1, 2, 1, 1, 1, c), source.sample(2, 1, 0, 0, 0, c));
            EXPECT_DOUBLE_EQ(doc->image.sample(2, 1, 1, 1, 1, c), 0);
            EXPECT_DOUBLE_EQ(doc->image.sample(0, 1, 0, 0, 0, c), 0);
        }
        QEvent activate(QEvent::WindowActivate); QCoreApplication::sendEvent(doc, &activate);
        window.commands().execute("edit.undo");
        EXPECT_DOUBLE_EQ(doc->image.sample(0, 1, 1, 1, 1), 0);
    }
}

TEST(Desktop, BackgroundPasteKeepsNewerEditsAndClosedTargetsInSeparateDocuments)
{
    for (bool closeTarget : {false, true}) {
        std::atomic<bool> release{false};
        MainWindow window;
        auto clipboard = Image::allocate(PixelType::UInt16, {2, 2});
        clipboard.setValue(std::array<std::size_t, 2>{0, 0}, 111);
        auto *source = window.addDocument(clipboard);
        window.commands().execute("edit.copy");
        auto original = Image::allocate(PixelType::UInt16, {4, 4});
        original.metadata().sourcePath = "original.tif";
        QPointer<DocumentWidget> target = window.addDocument(original);
        for (int i = 0; i < 2; ++i) window.jobs().submit("Hold worker", [&](auto stop, auto) {
            while (!release.load() && !stop.stop_requested()) QThread::msleep(1);
        }, {});
        window.commands().execute("edit.paste");
        if (closeTarget) {
            ASSERT_TRUE(target->close());
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            ASSERT_TRUE(target.isNull());
        } else {
            auto newer = Image::allocate(PixelType::UInt16, {3, 3});
            newer.setValue(std::array<std::size_t, 2>{0, 0}, 222);
            target->replaceImage(newer, "Newer edit");
        }
        release.store(true);
        ASSERT_TRUE(finishJobs(window));
        ASSERT_EQ(window.documents().size(), closeTarget ? 2 : 3);
        DocumentWidget *pasted = nullptr;
        for (auto *document : window.documents())
            if (document != source && document != target.data()) pasted = document;
        ASSERT_NE(pasted, nullptr);
        EXPECT_EQ(pasted->image.shape(), original.shape());
        EXPECT_DOUBLE_EQ(pasted->image.sample(0, 0), 111);
        EXPECT_TRUE(pasted->image.metadata().sourcePath.isEmpty());
        EXPECT_TRUE(pasted->modified);
        EXPECT_TRUE(pasted->title().endsWith(" *"));
        if (target) {
            EXPECT_EQ(target->image.shape(), (std::vector<std::size_t>{3, 3}));
            EXPECT_DOUBLE_EQ(target->image.sample(0, 0), 222);
            EXPECT_EQ(target->revision, 1u);
            EXPECT_TRUE(target->modified);
        }
    }
}

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
        EXPECT_TRUE(saveImage(window, path).isEmpty());
        auto newer = Image::allocate(PixelType::UInt16, {2, 2});
        newer.mutableData()[0] = std::byte{22};
        doc->replaceImage(newer, "New revision");
        EXPECT_TRUE(saveImage(window, directory.filePath("./" + fileName)).contains("still being saved"));
        if (cancel) window.jobs().cancelAll(); else release.store(true);
        ASSERT_TRUE(finishJobs(window));
        EXPECT_TRUE(doc->modified);
        if (cancel) EXPECT_FALSE(QFileInfo::exists(path));
        else EXPECT_DOUBLE_EQ(readImage(path).sample(0, 0), 11);
        EXPECT_TRUE(saveImage(window, path).isEmpty());
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
    ASSERT_TRUE(saveImage(window, path).isEmpty());
    ASSERT_TRUE(finishJobs(window));
    EXPECT_FALSE(document->modified);
    EXPECT_EQ(document->image.metadata().sourcePath, path);
    const auto saved = readImage(path);
    EXPECT_EQ(saved.metadata().dicomSequences, metadata.dicomSequences);
    EXPECT_EQ(saved.metadata().attributes.value("Experiment"), "Preserve this too");
}

TEST(Desktop, StartsCompactAndOpensIndependentImageAndToolWindows)
{
    MainWindow window; window.show(); QCoreApplication::processEvents();
    EXPECT_LE(window.width(), 720); EXPECT_LE(window.height(), 160);
    EXPECT_TRUE(window.documents().empty());
    EXPECT_EQ(window.findChildren<QTabWidget *>().size(), 0);
    EXPECT_EQ(window.findChildren<QDockWidget *>().size(), 0);
    for (const auto *id : {"display", "properties", "rois", "history", "measurements"}) {
        ASSERT_NE(window.panel(id), nullptr); EXPECT_FALSE(window.panel(id)->isVisible());
    }
    auto *first = window.addDocument(Image::allocate(PixelType::UInt8, {16, 16}));
    auto *second = window.addDocument(Image::allocate(PixelType::UInt8, {16, 16}));
    QCoreApplication::processEvents();
    EXPECT_TRUE(first->isWindow()); EXPECT_TRUE(second->isWindow());
    EXPECT_NE(first->window(), second->window()); EXPECT_NE(first->window(), window.window());
    ASSERT_EQ(window.documents().size(), 2);
    QEvent activated(QEvent::WindowActivate); QCoreApplication::sendEvent(first, &activated);
    EXPECT_EQ(window.activeDocument(), first);
    window.showPanel("display");
    ASSERT_TRUE(window.panel("display")->isWindow()); EXPECT_TRUE(window.panel("display")->isVisible());
    EXPECT_EQ(window.activeDocument(), first);
    auto *gamma = window.panel("display")->findChild<QDoubleSpinBox *>("displayGamma");
    if (!gamma) for (auto *spin : window.panel("display")->findChildren<QDoubleSpinBox *>())
        if (spin->minimum() == .05 && spin->maximum() == 20) gamma = spin;
    ASSERT_NE(gamma, nullptr); gamma->setValue(1.6);
    EXPECT_DOUBLE_EQ(first->viewer->gamma(), 1.6); EXPECT_DOUBLE_EQ(second->viewer->gamma(), 1);
    window.panel("display")->close();
    EXPECT_EQ(window.activeDocument(), first);
    first->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_EQ(window.documents().size(), 1); EXPECT_EQ(window.activeDocument(), second);
    EXPECT_TRUE(window.isVisible());
}

TEST(Desktop, PointerStartsSelectedBeforePanAndNavigationToolsApplyToEveryPlane)
{
    MainWindow window;
    auto *toolbar = window.findChild<QToolBar *>("imageTools");
    ASSERT_NE(toolbar, nullptr);
    const auto actions = toolbar->actions();
    auto pointer = std::find_if(actions.begin(), actions.end(), [](auto *action) { return action->text() == "Pointer"; });
    ASSERT_NE(pointer, actions.end());
    auto pan = std::next(pointer);
    ASSERT_NE(pan, actions.end());
    EXPECT_EQ((*pan)->text(), "Pan");
    EXPECT_TRUE((*pointer)->isChecked()); EXPECT_FALSE((*pan)->isChecked());
    EXPECT_FALSE((*pointer)->icon().pixmap(24, 24).isNull());
    auto *document = window.addDocument(Image::allocate(PixelType::UInt8, {20, 16, 8}));
    EXPECT_EQ(document->viewer->tool(), ImageViewer::Tool::Pointer);
    document->setViewMode(DocumentWidget::ViewMode::Orthogonal);
    (*pan)->trigger();
    for (auto *view : document->views()) EXPECT_EQ(view->tool(), ImageViewer::Tool::Pan);
    (*pointer)->trigger();
    for (auto *view : document->views()) EXPECT_EQ(view->tool(), ImageViewer::Tool::Pointer);
    EXPECT_TRUE((*pointer)->isChecked()); EXPECT_FALSE((*pan)->isChecked());
}

TEST(Desktop, SynchronizedWindowsUseOneDocumentAndKeepOtherWindowsIndependent)
{
    MainWindow window;
    auto *toolbar = window.findChild<QToolBar *>("imageTools");
    auto *orthogonal = window.findChild<QAction *>("view.orthogonal");
    ASSERT_NE(toolbar, nullptr); ASSERT_NE(orthogonal, nullptr);
    EXPECT_FALSE(toolbar->actions().contains(orthogonal));
    EXPECT_EQ(toolbar->widgetForAction(orthogonal), nullptr);
    EXPECT_FALSE(orthogonal->isEnabled());
    bool inMenu = false;
    for (auto *menu : window.findChildren<QMenu *>()) if (menu->actions().contains(orthogonal)) inMenu = true;
    EXPECT_TRUE(inMenu);
    ImageMetadata metadata;
    metadata.name = "Calibration phantom";
    metadata.axes = {{"X", .6, 0, "mm"}, {"Y", .6, 0, "mm"}, {"Z", 1.2, 0, "mm"}, {"C"}, {"T"}};
    auto image = Image::allocate(PixelType::UInt16, {128, 112, 64, 2, 2}, metadata);
    auto *pixels = reinterpret_cast<std::uint16_t *>(image.mutableData());
    for (std::size_t t = 0; t < 2; ++t)
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t z = 0; z < image.depth(); ++z)
                for (std::size_t y = 0; y < image.height(); ++y)
                    for (std::size_t x = 0; x < image.width(); ++x) {
                        const double dx = (double(x) - 63.5) / 53, dy = (double(y) - 55.5) / 46, dz = (double(z) - 31.5) / 27;
                        const double radius = std::sqrt(dx*dx + dy*dy + dz*dz);
                        double value = radius < 1 ? 1500 + 220 * std::sin(dx*24) * std::cos(dy*20) * std::cos(dz*16) : 0;
                        if (radius > .91 && radius < 1) value = 3300;
                        if (std::pow((dx + .32) / .22, 2) + std::pow(dy / .38, 2) + std::pow(dz / .5, 2) < 1) value = 600;
                        if (std::pow((dx - .3) / .2, 2) + std::pow((dy + .12) / .24, 2) + std::pow((dz - .08) / .3, 2) < 1) value = 2600;
                        const auto offset = x + image.width() * (y + image.height() * (z + image.depth() * (c + image.channels()*t)));
                        pixels[offset] = std::uint16_t(value * (1 + c*.08) + t*80);
                    }
    auto otherMetadata = metadata; otherMetadata.axes.resize(2);
    auto *other = window.addDocument(Image::allocate(PixelType::UInt8, {8, 8}, otherMetadata));
    auto *document = window.addDocument(image);
    EXPECT_TRUE(window.findChild<QAction *>("view.orthogonal")->isEnabled());
    orthogonal->trigger();
    EXPECT_EQ(document->viewMode(), DocumentWidget::ViewMode::Orthogonal);
    EXPECT_TRUE(window.findChild<QAction *>("view.orthogonal")->isChecked());
    for (auto *view : document->views())
        EXPECT_TRUE(view->window()->findChildren<QComboBox *>(QString{}, Qt::FindDirectChildrenOnly).isEmpty());
    EXPECT_EQ(window.documents().size(), 2);
    EXPECT_EQ(other->viewMode(), DocumentWidget::ViewMode::Single);
    QPointer<QWidget> previousXzWindow(document->views()[1]->window());
    QPointer<QWidget> previousYzWindow(document->views()[2]->window());
    window.findChild<QAction *>("view.single")->trigger();
    EXPECT_EQ(document->viewMode(), DocumentWidget::ViewMode::Single);
    EXPECT_FALSE(orthogonal->isChecked());
    EXPECT_TRUE(window.findChild<QAction *>("view.single")->isChecked());
    ASSERT_EQ(document->views().size(), 1);
    EXPECT_TRUE(previousXzWindow.isNull()); EXPECT_TRUE(previousYzWindow.isNull());
    EXPECT_EQ(window.documents().size(), 2);
    EXPECT_EQ(other->viewMode(), DocumentWidget::ViewMode::Single);
    orthogonal->trigger();
    EXPECT_EQ(document->viewMode(), DocumentWidget::ViewMode::Orthogonal);
    EXPECT_TRUE(orthogonal->isChecked());
    EXPECT_FALSE(window.findChild<QAction *>("view.single")->isChecked());
    ASSERT_EQ(document->views().size(), 3);
    document->setPosition(64, 56, 32, 0, 0);
    document->viewer->setWindow(0, 3600);
    document->viewer->setScaleBarVisible(true);
    document->viewer->setLinearInterpolation(true);
    // Place windows for review captures only. The application leaves placement to the user.
    document->resize(600, 650); document->move(16, 160);
    document->views()[1]->window()->resize(480, 300); document->views()[1]->window()->move(660, 160);
    document->views()[2]->window()->resize(480, 300); document->views()[2]->window()->move(660, 520);

    const auto capture = qEnvironmentVariable("IMAGECXX_VIEWS_CAPTURE_DIR");
    const auto settle = [] {
        QElapsedTimer timer; timer.start();
        while (timer.elapsed() < 350) { QCoreApplication::processEvents(); QThread::msleep(2); }
    };
    const auto screenshot = [&](const QString &name) {
        if (capture.isEmpty()) return;
        ASSERT_NE(QApplication::platformName(), "offscreen");
        QDir().mkpath(capture);
        other->hide();
        window.move(16, 16); window.show(); window.raise();
        document->showViews();
        for (auto *view : document->views()) view->window()->raise();
        document->activateWindow(); settle();
        window.repaint(); QCoreApplication::processEvents();
        auto frame = window.frameGeometry().united(document->frameGeometry());
        for (auto *view : document->views()) frame = frame.united(view->window()->frameGeometry());
        EXPECT_TRUE(document->screen()->grabWindow(0, frame.x(), frame.y(), frame.width(), frame.height()).save(capture + "/" + name));
    };
    settle();
    auto *xzWindow = document->views()[1]->window();
    auto *yzWindow = document->views()[2]->window();
    EXPECT_NE(xzWindow, yzWindow); EXPECT_NE(xzWindow, document);
    for (auto *viewWindow : {xzWindow, yzWindow}) {
        EXPECT_EQ(viewWindow->parentWidget(), document->parentWidget());
        ASSERT_NE(viewWindow->windowHandle(), nullptr);
        EXPECT_NE(viewWindow->windowHandle()->transientParent(), document->windowHandle());
    }
    if (QApplication::platformName() == "xcb") {
        const auto position = xzWindow->pos();
        xzWindow->move(document->pos());
        xzWindow->raise(); xzWindow->activateWindow(); settle();
        const auto overlap = xzWindow->mapToGlobal(xzWindow->rect().center());
        auto *front = QApplication::widgetAt(overlap);
        ASSERT_NE(front, nullptr);
        EXPECT_EQ(front->window(), xzWindow) << front->window()->windowTitle().toStdString();
        document->raise(); document->activateWindow(); settle();
        front = QApplication::widgetAt(overlap);
        ASSERT_NE(front, nullptr);
        EXPECT_EQ(front->window(), document) << front->window()->windowTitle().toStdString();
        xzWindow->move(position);
    }
    QEvent activated(QEvent::WindowActivate);
    QCoreApplication::sendEvent(other, &activated);
    EXPECT_EQ(window.activeDocument(), other);
    QCoreApplication::sendEvent(xzWindow, &activated);
    EXPECT_EQ(window.activeDocument(), document);
    sendKey(document->views()[1], Qt::Key_I);
    EXPECT_TRUE(window.panel("properties")->isVisible());
    window.panel("properties")->hide();
    xzWindow->close();
    EXPECT_FALSE(xzWindow->isVisible()); EXPECT_TRUE(window.documents().contains(document));
    QCoreApplication::sendEvent(document, &activated);
    orthogonal->trigger();
    EXPECT_TRUE(window.findChild<QAction *>("view.orthogonal")->isChecked());
    EXPECT_EQ(document->views()[1]->window(), xzWindow);
    EXPECT_TRUE(xzWindow->isVisible());
    screenshot("orthogonal-menu-only.png");
    document->viewer->window()->findChild<QSlider *>("sliceSlider", Qt::FindDirectChildrenOnly)->setValue(25);
    xzWindow->findChild<QSlider *>("sliceSlider", Qt::FindDirectChildrenOnly)->setValue(50);
    yzWindow->findChild<QSlider *>("sliceSlider", Qt::FindDirectChildrenOnly)->setValue(80);
    EXPECT_EQ(document->views()[1]->slice(), 50u);
    EXPECT_EQ(document->viewer->slice(), 25u);
    EXPECT_EQ(document->views()[2]->slice(), 80u);
    screenshot("orthogonal-menu-navigation.png");
    QCoreApplication::sendEvent(document, &activated);
    QTimer::singleShot(0, &window, [] {
        auto *picker = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_NE(picker, nullptr);
        auto *images = picker->findChild<QListWidget *>("linkedImages");
        ASSERT_NE(images, nullptr);
        images->item(0)->setCheckState(Qt::Checked); picker->accept();
    });
    window.commands().execute("view.linked"); settle();
    EXPECT_EQ(document->views().size(), 1);
    EXPECT_EQ(other->viewMode(), DocumentWidget::ViewMode::Linked);
    EXPECT_EQ(window.documents().size(), 2);
    document->findChild<QSlider *>("sliceSlider", Qt::FindDirectChildrenOnly)->setValue(28);
    window.panel("display")->findChild<QSpinBox *>("displayChannel")->setValue(2);
    window.panel("display")->findChild<QSpinBox *>("displayTime")->setValue(2);
    EXPECT_EQ(document->viewer->slice(), 28u);
    EXPECT_EQ(other->viewer->slice(), 0u); EXPECT_DOUBLE_EQ(other->viewer->gamma(), 1);
    screenshot("linked-view-menu.png");
    window.commands().execute("view.single");
    EXPECT_EQ(document->views().size(), 1);
    auto *flat = window.addDocument(Image::allocate(PixelType::UInt8, {8, 8}));
    EXPECT_FALSE(window.findChild<QAction *>("view.orthogonal")->isEnabled());
    EXPECT_EQ(flat->viewMode(), DocumentWidget::ViewMode::Single);
    document->setViewMode(DocumentWidget::ViewMode::Orthogonal);
    QPointer<QWidget> retiredWindow(document->views()[1]->window());
    document->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_TRUE(retiredWindow.isNull());
}

TEST(Desktop, SynchronizedWindowsUseNormalImageSizingForTheirCalibratedPlanes)
{
    MainWindow window;
    const auto xzSize = window.addDocument(Image::allocate(PixelType::UInt8, {400, 320}))->size();
    const auto yzSize = window.addDocument(Image::allocate(PixelType::UInt8, {280, 160}))->size();
    ImageMetadata metadata;
    metadata.axes = {{"X", .5, 0, "mm"}, {"Y", 1, 0, "mm"}, {"Z", 2, 0, "mm"}};
    auto *document = window.addDocument(Image::allocate(PixelType::UInt8, {400, 280, 80}, metadata));
    document->resize(640, 600);
    document->setViewMode(DocumentWidget::ViewMode::Orthogonal);
    ASSERT_EQ(document->views().size(), 3);
    EXPECT_EQ(document->views()[1]->window()->size(), xzSize);
    EXPECT_EQ(document->views()[2]->window()->size(), yzSize);
    for (auto *view : document->views())
        EXPECT_EQ(view->window()->minimumSize(), document->minimumSize());
    document->setViewMode(DocumentWidget::ViewMode::Single);
    EXPECT_EQ(document->size(), QSize(640, 600));
}

TEST(Desktop, LinkedViewChecklistReopensWithTheGroupAndCanAddOrRemoveImages)
{
    MainWindow window;
    auto *action = window.findChild<QAction *>("view.linked");
    auto *first = window.addDocument(Image::allocate(PixelType::UInt8, {10, 8, 6}));
    EXPECT_FALSE(action->isEnabled());
    auto *second = window.addDocument(Image::allocate(PixelType::UInt8, {10, 8, 6}));
    auto *third = window.addDocument(Image::allocate(PixelType::UInt8, {10, 8, 6}));
    EXPECT_TRUE(action->isEnabled());
    QTimer::singleShot(0, &window, [] {
        auto *picker = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_NE(picker, nullptr); picker->reject();
    });
    window.commands().execute("view.linked");
    EXPECT_EQ(third->viewMode(), DocumentWidget::ViewMode::Single);
    QTimer::singleShot(0, &window, [] {
        auto *picker = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_NE(picker, nullptr);
        auto *images = picker->findChild<QListWidget *>("linkedImages");
        ASSERT_NE(images, nullptr); ASSERT_EQ(images->count(), 2);
        EXPECT_NE(images->item(0)->text(), images->item(1)->text());
        images->item(1)->setCheckState(Qt::Checked); picker->accept();
    });
    window.commands().execute("view.linked");
    EXPECT_EQ(first->viewMode(), DocumentWidget::ViewMode::Single);
    EXPECT_EQ(second->viewMode(), DocumentWidget::ViewMode::Linked);
    EXPECT_EQ(third->viewMode(), DocumentWidget::ViewMode::Linked);
    QEvent activated(QEvent::WindowActivate); QCoreApplication::sendEvent(third, &activated);
    QTimer::singleShot(0, &window, [] {
        auto *picker = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_NE(picker, nullptr);
        auto *images = picker->findChild<QListWidget *>("linkedImages");
        ASSERT_NE(images, nullptr);
        EXPECT_EQ(images->item(0)->checkState(), Qt::Unchecked);
        EXPECT_EQ(images->item(1)->checkState(), Qt::Checked);
        images->item(0)->setCheckState(Qt::Checked); picker->accept();
    });
    window.commands().execute("view.linked");
    EXPECT_EQ(window.documents().size(), 3);
    third->viewer->setSlice(4);
    EXPECT_EQ(second->viewer->slice(), 4u); EXPECT_EQ(first->viewer->slice(), 4u);
    QCoreApplication::sendEvent(first, &activated);
    QTimer::singleShot(0, &window, [] {
        auto *picker = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_NE(picker, nullptr);
        auto *images = picker->findChild<QListWidget *>("linkedImages");
        ASSERT_NE(images, nullptr);
        EXPECT_EQ(images->item(0)->checkState(), Qt::Checked);
        EXPECT_EQ(images->item(1)->checkState(), Qt::Checked);
        images->item(1)->setCheckState(Qt::Unchecked); picker->accept();
    });
    window.commands().execute("view.linked");
    EXPECT_EQ(third->viewMode(), DocumentWidget::ViewMode::Single);
    first->viewer->setSlice(2);
    EXPECT_EQ(second->viewer->slice(), 2u); EXPECT_EQ(third->viewer->slice(), 4u);
    QCoreApplication::sendEvent(first, &activated);
    QTimer::singleShot(0, &window, [] {
        auto *picker = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_NE(picker, nullptr);
        auto *images = picker->findChild<QListWidget *>("linkedImages");
        ASSERT_NE(images, nullptr);
        images->item(0)->setCheckState(Qt::Unchecked); picker->accept();
    });
    window.commands().execute("view.linked");
    EXPECT_EQ(first->viewMode(), DocumentWidget::ViewMode::Single);
    EXPECT_EQ(second->viewMode(), DocumentWidget::ViewMode::Single);
}

TEST(Desktop, ShowingWindowsKeepsTheirFramesAndTitleBarsOnScreen)
{
    MainWindow window;
    window.move(-5000, -5000); window.show();
    QCoreApplication::processEvents();
    EXPECT_TRUE(window.screen()->availableGeometry().contains(window.frameGeometry()));
    auto *document = window.addDocument(Image::allocate(PixelType::UInt8, {80, 60, 8}));
    bool ready = false;
    QObject::connect(document->viewer, &ImageViewer::sliceReady, &window, [&] { ready = true; });
    QElapsedTimer timer; timer.start();
    while (!ready && timer.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(2); }
    ASSERT_TRUE(ready);
    document->hide();
    document->move(0, -5000); document->resize(5000, 5000); document->show();
    QCoreApplication::processEvents();
    EXPECT_TRUE(document->screen()->availableGeometry().contains(document->frameGeometry()));
    document->setViewMode(DocumentWidget::ViewMode::Orthogonal);
    for (auto *view : document->views()) {
        auto *imageWindow = view->window();
        imageWindow->hide(); imageWindow->move(5000, -5000); imageWindow->show();
        QCoreApplication::processEvents();
        EXPECT_TRUE(imageWindow->screen()->availableGeometry().contains(imageWindow->frameGeometry()));
    }
    auto *panel = window.panel("display");
    panel->hide(); panel->move(5000, -5000); panel->show();
    QCoreApplication::processEvents();
    EXPECT_TRUE(panel->screen()->availableGeometry().contains(panel->frameGeometry()));
}

TEST(Desktop, DisplayChannelTimeSelectorsSynchronizeViewsAndFollowTheActiveDocument)
{
    MainWindow window;
    auto *document = window.addDocument(Image::allocate(PixelType::UInt16, {10, 8, 6, 2, 4}));
    document->setViewMode(DocumentWidget::ViewMode::Orthogonal);
    auto *channel = window.panel("display")->findChild<QSpinBox *>("displayChannel");
    auto *time = window.panel("display")->findChild<QSpinBox *>("displayTime");
    ASSERT_NE(channel, nullptr); ASSERT_NE(time, nullptr);
    EXPECT_EQ(channel->maximum(), 2); EXPECT_EQ(time->maximum(), 4);
    channel->setValue(2); time->setValue(3);
    for (auto *view : document->views()) { EXPECT_EQ(view->channel(), 1u); EXPECT_EQ(view->time(), 2u); }
    document->views()[1]->setSlice(3, 0, 3);
    EXPECT_EQ(channel->value(), 1); EXPECT_EQ(time->value(), 4);
    auto *flat = window.addDocument(Image::allocate(PixelType::UInt8, {4, 4}));
    EXPECT_FALSE(channel->isEnabled()); EXPECT_FALSE(time->isEnabled());
    EXPECT_EQ(channel->maximum(), 1); EXPECT_EQ(time->maximum(), 1);
    QEvent activated(QEvent::WindowActivate); QCoreApplication::sendEvent(document, &activated);
    EXPECT_TRUE(channel->isEnabled()); EXPECT_TRUE(time->isEnabled());
    EXPECT_EQ(channel->value(), 1); EXPECT_EQ(time->value(), 4);
    channel->setValue(2);
    for (auto *view : document->views()) { EXPECT_EQ(view->channel(), 1u); EXPECT_EQ(view->time(), 3u); }
    EXPECT_EQ(flat->viewer->channel(), 0u); EXPECT_EQ(flat->viewer->time(), 0u);
    EXPECT_FALSE(document->modified);
}

TEST(Desktop, InvertLutTracksDisplaySettingsAndActiveDocument)
{
    MainWindow window;
    auto *menu = window.findChild<QMenu *>("lutMenu");
    ASSERT_NE(menu, nullptr);
    EXPECT_FALSE(menu->menuAction()->isEnabled());
    const auto actions = menu->actions();
    ASSERT_EQ(actions.size(), 5);
    auto *invert = actions[0];
    EXPECT_EQ(invert->text(), "Invert LUT");
    EXPECT_TRUE(invert->isCheckable());
    EXPECT_TRUE(actions[1]->isSeparator());
    auto *first = window.addDocument(Image::allocate(PixelType::UInt8, {4, 4}));
    EXPECT_TRUE(menu->menuAction()->isEnabled());
    EXPECT_FALSE(invert->isChecked());
    invert->trigger();
    EXPECT_TRUE(first->viewer->isInverted());
    EXPECT_TRUE(invert->isChecked());
    actions[3]->trigger();
    EXPECT_EQ(first->viewer->lut(), "hot");
    EXPECT_TRUE(actions[3]->isChecked());
    EXPECT_TRUE(invert->isChecked());
    invert->trigger();
    EXPECT_FALSE(first->viewer->isInverted());
    EXPECT_EQ(first->viewer->lut(), "hot");
    EXPECT_TRUE(actions[3]->isChecked());
    first->viewer->setInverted(true);
    EXPECT_TRUE(invert->isChecked());
    auto *display = window.panel("display");
    for (auto *check : display->findChildren<QCheckBox *>())
        EXPECT_NE(check->text(), "Invert");
    display->findChild<QDoubleSpinBox *>("displayGamma")->setValue(2);
    EXPECT_TRUE(first->viewer->isInverted());
    EXPECT_TRUE(invert->isChecked());
    auto *second = window.addDocument(Image::allocate(PixelType::UInt8, {4, 4}));
    EXPECT_FALSE(invert->isChecked());
    QEvent activated(QEvent::WindowActivate); QCoreApplication::sendEvent(first, &activated);
    EXPECT_TRUE(invert->isChecked());
    EXPECT_TRUE(actions[3]->isChecked());
    EXPECT_FALSE(second->viewer->isInverted());
    EXPECT_FALSE(first->modified);
    EXPECT_DOUBLE_EQ(first->image.sample(0, 0), 0);
}

TEST(Desktop, IntersectionNavigationPreservesDocumentPanels)
{
    MainWindow window;
    auto *document = window.addDocument(Image::allocate(PixelType::UInt16, {10, 8, 6, 2, 3}));
    document->setViewMode(DocumentWidget::ViewMode::Orthogonal);
    Roi roi; roi.name = "Saved selection"; roi.points = {{1, 1}, {4, 4}};
    document->rois.push_back(roi); document->setSelection(roi);
    auto *properties = window.panel("properties")->findChild<QTreeWidget *>();
    auto *rois = window.panel("rois")->findChild<QListWidget *>();
    auto *history = window.panel("history")->findChild<QListWidget *>();
    ASSERT_NE(properties, nullptr); ASSERT_NE(rois, nullptr); ASSERT_NE(history, nullptr);
    properties->topLevelItem(0)->setExpanded(true);
    ASSERT_NE(rois->currentItem(), nullptr);
    const auto selectedId = rois->currentItem()->data(Qt::UserRole);
    int resets = 0;
    for (auto *model : {properties->model(), rois->model(), history->model()})
        QObject::connect(model, &QAbstractItemModel::modelReset, &window, [&] { ++resets; });
    document->views()[1]->positionPicked({2.5, 1.5});
    document->views()[1]->positionPicked({2.5, 1.5});
    document->views()[2]->positionPicked({3.5, 4.5});
    document->setPosition(2, 3, 4, 1, 2);
    EXPECT_EQ(resets, 0);
    EXPECT_TRUE(properties->topLevelItem(0)->isExpanded());
    ASSERT_NE(rois->currentItem(), nullptr);
    EXPECT_EQ(rois->currentItem()->data(Qt::UserRole), selectedId);
    EXPECT_EQ(window.panel("display")->findChild<QSpinBox *>("displayChannel")->value(), 2);
    EXPECT_EQ(window.panel("display")->findChild<QSpinBox *>("displayTime")->value(), 3);
    for (auto *view : document->views()) { EXPECT_EQ(view->channel(), 1u); EXPECT_EQ(view->time(), 2u); }
}

TEST(Desktop, DisplaySlidersTrackValuesAutoRangeAndActiveDocument)
{
    MainWindow window;
    auto *panel = window.panel("display");
    auto *minimum = panel->findChild<QDoubleSpinBox *>("displayMinimum");
    auto *maximum = panel->findChild<QDoubleSpinBox *>("displayMaximum");
    auto *gamma = panel->findChild<QDoubleSpinBox *>("displayGamma");
    auto *minimumSlider = panel->findChild<QSlider *>("displayMinimumSlider");
    auto *maximumSlider = panel->findChild<QSlider *>("displayMaximumSlider");
    auto *gammaSlider = panel->findChild<QSlider *>("displayGammaSlider");
    ASSERT_NE(minimumSlider, nullptr); ASSERT_NE(maximumSlider, nullptr); ASSERT_NE(gammaSlider, nullptr);
    ASSERT_NE(minimum, nullptr); ASSERT_NE(maximum, nullptr); ASSERT_NE(gamma, nullptr);
    EXPECT_FALSE(minimumSlider->isEnabled()); EXPECT_FALSE(maximumSlider->isEnabled()); EXPECT_FALSE(gammaSlider->isEnabled());

    auto image = Image::allocate(PixelType::Float32, {2, 2});
    const std::array<float, 4> pixels{-100, 0, 50, 100};
    std::copy(pixels.begin(), pixels.end(), reinterpret_cast<float *>(image.mutableData()));
    auto *first = window.addDocument(image);
    bool ready = false;
    QObject::connect(first->viewer, &ImageViewer::sliceReady, &window, [&] { ready = true; });
    QElapsedTimer timer; timer.start();
    while (!ready && timer.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(2); }
    ASSERT_TRUE(ready);
    auto *properties = window.panel("properties")->findChild<QTreeWidget *>();
    ASSERT_NE(properties, nullptr);
    auto *property = properties->topLevelItem(0);
    ASSERT_NE(property, nullptr);
    property->setExpanded(true);
    EXPECT_TRUE(minimumSlider->isEnabled()); EXPECT_TRUE(maximumSlider->isEnabled()); EXPECT_TRUE(gammaSlider->isEnabled());
    EXPECT_DOUBLE_EQ(minimum->value(), -100); EXPECT_DOUBLE_EQ(maximum->value(), 100);
    minimumSlider->setValue(250); maximumSlider->setValue(750);
    EXPECT_DOUBLE_EQ(first->viewer->windowMinimum(), -50); EXPECT_DOUBLE_EQ(first->viewer->windowMaximum(), 50);
    minimum->setValue(-25); maximum->setValue(75);
    EXPECT_EQ(minimumSlider->value(), 375); EXPECT_EQ(maximumSlider->value(), 875);
    // Crossing either endpoint keeps the valid range and restores the handle.
    minimumSlider->setValue(1000); maximumSlider->setValue(0);
    EXPECT_DOUBLE_EQ(first->viewer->windowMinimum(), -25); EXPECT_DOUBLE_EQ(first->viewer->windowMaximum(), 75);
    EXPECT_EQ(minimumSlider->value(), 375); EXPECT_EQ(maximumSlider->value(), 875);
    gammaSlider->setValue(1000); EXPECT_DOUBLE_EQ(first->viewer->gamma(), 20);
    gammaSlider->setValue(0); EXPECT_DOUBLE_EQ(first->viewer->gamma(), .05);
    gammaSlider->setValue(500); EXPECT_DOUBLE_EQ(first->viewer->gamma(), 1);
    gamma->setValue(2); EXPECT_NEAR(gammaSlider->value(), 616, 1);
    first->viewer->autoWindow();
    EXPECT_EQ(minimumSlider->value(), 0); EXPECT_EQ(maximumSlider->value(), 1000);
    EXPECT_TRUE(properties->topLevelItem(0)->isExpanded());

    auto *second = window.addDocument(Image::allocate(PixelType::UInt16, {2, 2}));
    second->viewer->setWindow(1000, 5000);
    gammaSlider->setValue(500);
    EXPECT_DOUBLE_EQ(second->viewer->gamma(), 1); EXPECT_DOUBLE_EQ(first->viewer->gamma(), 2);
    QEvent activated(QEvent::WindowActivate); QCoreApplication::sendEvent(first, &activated);
    EXPECT_DOUBLE_EQ(minimum->value(), -100); EXPECT_DOUBLE_EQ(maximum->value(), 100);
    EXPECT_DOUBLE_EQ(gamma->value(), 2); EXPECT_NEAR(gammaSlider->value(), 616, 1);
    EXPECT_DOUBLE_EQ(first->image.sample(0, 0), -100);
}

TEST(Desktop, ShowsImageWindowOnlyAfterFirstSliceIsReady)
{
    MainWindow window;
    auto *document = window.addDocument(Image::allocate(PixelType::UInt16, {16, 16, 2}));
    EXPECT_FALSE(document->isVisible());
    bool ready = false;
    QObject::connect(document->viewer, &ImageViewer::sliceReady, &window, [&] { ready = true; });
    QElapsedTimer timer; timer.start();
    while (!ready && timer.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(2); }
    ASSERT_TRUE(ready);
    EXPECT_TRUE(document->isVisible());

    // Later slice loads must not reopen a window the user has hidden.
    document->hide();
    ready = false;
    document->viewer->setSlice(1);
    timer.restart();
    while (!ready && timer.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(2); }
    ASSERT_TRUE(ready);
    EXPECT_FALSE(document->isVisible());
}

TEST(Desktop, FailedFirstSliceDoesNotOpenAnEmptyWindow)
{
    MainWindow window;
    auto *document = window.addDocument(Image::allocate(PixelType::UInt16, {16, 16}));
    QPointer<DocumentWidget> pending(document);
    document->viewer->setCacheBudgets(1, 1);
    bool failed = false;
    QObject::connect(document->viewer, &ImageViewer::loadFailed, &window, [&] {
        failed = true;
        EXPECT_FALSE(document->isVisible());
    });
    QElapsedTimer timer; timer.start();
    while (!failed && timer.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(2); }
    ASSERT_TRUE(failed);
    EXPECT_TRUE(window.documents().empty());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_TRUE(pending.isNull());
}

TEST(Desktop, OpensTypedStackMeasuresAndUndoRedoRestoresValues)
{
    MainWindow window;
    ImageMetadata metadata; metadata.name = "Numeric stack";
    metadata.axes = {{"X", .5, 0, "um"}, {"Y", .5, 0, "um"}, {"Z", 2, 0, "um"}};
    auto image = Image::allocate(PixelType::UInt16, {8, 8, 2}, metadata);
    auto *pixels = reinterpret_cast<std::uint16_t *>(image.mutableData());
    std::fill(pixels, pixels + 128, std::uint16_t{1000});
    auto *document = window.addDocument(image);
    EXPECT_EQ(window.activeDocument(), document);
    Roi roi; roi.name = "Patch"; roi.points = {{0, 0}, {4, 4}};
    document->rois.push_back(roi); document->viewer->setRois(document->rois); document->changed();
    window.executeCommand("analyze.measure");
    QElapsedTimer timer; timer.start();
    while (window.jobs().busy() && timer.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(2); }
    QCoreApplication::processEvents();
    auto *table = window.findChild<QTableView *>("measurementTable"); ASSERT_NE(table, nullptr);
    ASSERT_EQ(table->model()->rowCount(), 1);
    EXPECT_DOUBLE_EQ(table->model()->data(table->model()->index(0, 4)).toDouble(), 1000);
    auto replacement = image.clone(); reinterpret_cast<std::uint16_t *>(replacement.mutableData())[0] = 42;
    document->replaceImage(replacement, "Change");
    EXPECT_DOUBLE_EQ(document->image.sample(0, 0), 42);
    window.executeCommand("edit.undo"); EXPECT_DOUBLE_EQ(document->image.sample(0, 0), 1000);
    window.executeCommand("edit.redo"); EXPECT_DOUBLE_EQ(document->image.sample(0, 0), 42);
}

TEST(Desktop, ImagePropertiesShowDicomTagNamesWithoutChangingMetadata)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ImageMetadata metadata;
    metadata.axes = {{"X", 1, 0, "mm"}, {"Y", 1, 0, "mm"}};
    metadata.attributes.insert("DICOM.0010,0010", "Example^Patient");
    const auto path = directory.filePath("properties.dcm");
    writeDicom(Image::allocate(PixelType::UInt16, {4, 4}, metadata), path);
    DcmFileFormat file;
    ASSERT_TRUE(file.loadFile(QFile::encodeName(path).constData()).good());
    for (int index = 0; index < 2; ++index) {
        DcmItem *series = nullptr, *reference = nullptr;
        ASSERT_TRUE(file.getDataset()->findOrCreateSequenceItem(DCM_ReferencedSeriesSequence, series, index).good());
        ASSERT_TRUE(series->findOrCreateSequenceItem(DCM_ReferencedImageSequence, reference).good());
        ASSERT_TRUE(reference->putAndInsertString(DCM_ReferencedSOPInstanceUID, index == 0 ? "1.2.3" : "1.2.4").good());
    }
    ASSERT_TRUE(file.getDataset()->insert(new DcmSequenceOfItems(DCM_ReferencedStudySequence)).good());
    ASSERT_TRUE(file.saveFile(QFile::encodeName(path).constData(), EXS_LittleEndianExplicit).good());
    auto image = readDicom(path);
    image.metadata().attributes.insert("DICOM.7777,1234", "Private value");
    image.metadata().attributes.insert("DICOM.0010,invalid", "Malformed key");
    image.metadata().attributes.insert("Experiment", "Example");
    const auto attributes = image.metadata().attributes;

    MainWindow window;
    auto *document = window.addDocument(image);
    auto *properties = window.panel("properties")->findChild<QTreeWidget *>();
    ASSERT_NE(properties, nullptr);
    const QMap<QString, QString> expected{{"(0010,0010) Patient Name", "Example^Patient"},
        {"(0008,0016) SOP Class UID", attributes.value("DICOM.0008,0016")},
        {"(0028,0010) Rows", "4"}, {"(7777,1234)", "Private value"}};
    for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
        const auto items = properties->findItems(it.key(), Qt::MatchExactly, 0);
        ASSERT_EQ(items.size(), 1) << it.key().toStdString();
        EXPECT_EQ(items.front()->text(1), it.value());
    }
    EXPECT_EQ(document->image.metadata().attributes, attributes);
    const auto sequences = properties->findItems("(0008,1115) Referenced Series Sequence", Qt::MatchExactly, 0);
    ASSERT_EQ(sequences.size(), 1);
    auto *sequence = sequences.front();
    ASSERT_EQ(sequence->childCount(), 2);
    EXPECT_TRUE(sequence->isExpanded());
    for (int index = 0; index < 2; ++index) {
        auto *item = sequence->child(index);
        EXPECT_EQ(item->text(0), QString("Item %1").arg(index + 1));
        EXPECT_TRUE(item->isExpanded());
        ASSERT_EQ(item->childCount(), 1);
        auto *nested = item->child(0);
        EXPECT_EQ(nested->text(0), "(0008,1140) Referenced Image Sequence");
        EXPECT_TRUE(nested->isExpanded());
        ASSERT_EQ(nested->childCount(), 1);
        auto *reference = nested->child(0);
        EXPECT_TRUE(reference->isExpanded());
        ASSERT_EQ(reference->childCount(), 1);
        EXPECT_EQ(reference->child(0)->text(0), "(0008,1155) Referenced SOP Instance UID");
        EXPECT_EQ(reference->child(0)->text(1), index == 0 ? "1.2.3" : "1.2.4");
    }
    const auto empty = properties->findItems("(0008,1110) Referenced Study Sequence", Qt::MatchExactly, 0);
    ASSERT_EQ(empty.size(), 1);
    EXPECT_EQ(empty.front()->childCount(), 0);
    EXPECT_EQ(empty.front()->childIndicatorPolicy(), QTreeWidgetItem::ShowIndicator);
    EXPECT_TRUE(empty.front()->isExpanded());
    sequence->setExpanded(false);
    EXPECT_FALSE(sequence->isExpanded());
    sequence->setExpanded(true);
    EXPECT_TRUE(sequence->isExpanded());
    EXPECT_EQ(document->image.metadata().dicomSequences, image.metadata().dicomSequences);
}

TEST(Desktop, NiftiPropertiesShowOnlyReadableHeaderFields)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    for (const auto &extension : {QStringLiteral("nii"), QStringLiteral("nii.gz")}) {
        ImageMetadata metadata;
        metadata.axes = {{"X", .5, 0, "mm"}, {"Y", .75, 0, "mm"}, {"Z", 1, 0, "mm"}};
        metadata.attributes.insert("ITK_FileNotes", "Example header");
        const auto path = directory.filePath("properties." + extension);
        writeImage(Image::allocate(PixelType::UInt16, {4, 4, 2}, metadata), path);
        auto image = readImage(path);
        ASSERT_TRUE(image.metadata().attributes.contains("nifti_type"));
        image.metadata().attributes.insert("Experiment", "Unrelated metadata");
        if (extension == "nii.gz") image.metadata().sourcePath.clear();
        const auto attributes = image.metadata().attributes;

        MainWindow window;
        auto *document = window.addDocument(image);
        auto *properties = window.panel("properties")->findChild<QTreeWidget *>();
        ASSERT_NE(properties, nullptr);
        const QMap<QString, QString> expected{{"Data Type (datatype)", "512"},
            {"Bits per Voxel (bitpix)", "16"}, {"Description (descrip)", "Example header"},
            {"Grid Spacing (pixdim[1])", attributes.value("pixdim[1]")},
            {"Q-form Code (qform_code)", attributes.value("qform_code")}};
        for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
            const auto items = properties->findItems(it.key(), Qt::MatchExactly, 0);
            ASSERT_EQ(items.size(), 1) << it.key().toStdString();
            EXPECT_EQ(items.front()->text(1), it.value());
        }
        for (const auto &label : {"Name", "Pixel type", "Axis X", "Intensity scale", "Experiment", "ITK_FileNotes", "nifti_type"})
            EXPECT_TRUE(properties->findItems(label, Qt::MatchExactly, 0).empty()) << label;
        EXPECT_EQ(document->image.metadata().attributes, attributes);
    }
}

TEST(Desktop, PropertySearchMatchesNamesTagsAndValuesAndKeepsSequenceParents)
{
    ImageMetadata metadata;
    metadata.attributes = {{"DICOM.0028,0030", "4.418060\\4.418060"}, {"DICOM.0010,0010", "Example^Patient"}};
    metadata.dicomSequences = {{"DICOM.0008,1115", QJsonArray{
        QJsonObject{{"DICOM.0008,103e", "Needle^Value"}}, QJsonObject{{"DICOM.0008,103e", "Other value"}}}}};
    MainWindow window;
    auto *document = window.addDocument(Image::allocate(PixelType::UInt16, {4, 4}, metadata));
    auto *properties = window.panel("properties")->findChild<QTreeWidget *>();
    auto *search = window.panel("properties")->findChild<QLineEdit *>("propertiesSearch");
    ASSERT_NE(properties, nullptr); ASSERT_NE(search, nullptr);
    const auto spacingItems = properties->findItems("(0028,0030) Pixel Spacing", Qt::MatchExactly, 0);
    const auto patientItems = properties->findItems("(0010,0010) Patient Name", Qt::MatchExactly, 0);
    const auto sequences = properties->findItems("(0008,1115) Referenced Series Sequence", Qt::MatchExactly, 0);
    ASSERT_EQ(spacingItems.size(), 1); ASSERT_EQ(patientItems.size(), 1); ASSERT_EQ(sequences.size(), 1);
    auto *spacing = spacingItems.front();
    auto *patient = patientItems.front();
    auto *sequence = sequences.front();
    ASSERT_EQ(sequence->childCount(), 2);
    for (const auto &query : {"00280030", "pixel spacing", "PiXeL_sPaCiNg", "(0028,0030)", "4.4"}) {
        search->setText(query);
        EXPECT_FALSE(spacing->isHidden()) << query;
        EXPECT_TRUE(patient->isHidden()) << query;
        EXPECT_TRUE(sequence->isHidden()) << query;
    }
    search->setText("EXAM-PLE_patIENT");
    EXPECT_FALSE(patient->isHidden()); EXPECT_TRUE(spacing->isHidden());
    sequence->setExpanded(false);
    search->setText("needle value");
    EXPECT_FALSE(sequence->isHidden()); EXPECT_TRUE(sequence->isExpanded());
    EXPECT_FALSE(sequence->child(0)->isHidden()); EXPECT_TRUE(sequence->child(0)->isExpanded());
    EXPECT_TRUE(sequence->child(1)->isHidden()); EXPECT_TRUE(spacing->isHidden());
    search->setText("referenced SERIES");
    EXPECT_FALSE(sequence->isHidden());
    EXPECT_FALSE(sequence->child(0)->isHidden()); EXPECT_FALSE(sequence->child(1)->isHidden());
    search->setText("no such property");
    for (int index = 0; index < properties->topLevelItemCount(); ++index)
        EXPECT_TRUE(properties->topLevelItem(index)->isHidden());
    for (const auto &query : {" -- () ", ""}) {
        search->setText(query);
        EXPECT_FALSE(spacing->isHidden()); EXPECT_FALSE(patient->isHidden()); EXPECT_FALSE(sequence->isHidden());
        EXPECT_FALSE(sequence->child(0)->isHidden()); EXPECT_FALSE(sequence->child(1)->isHidden());
    }
    search->setText("pixel spacing");
    document->changed();
    const auto refreshed = properties->findItems("(0028,0030) Pixel Spacing", Qt::MatchExactly, 0);
    ASSERT_EQ(refreshed.size(), 1);
    EXPECT_FALSE(refreshed.front()->isHidden());
    EXPECT_EQ(search->text(), "pixel spacing");
    const auto refreshedPatients = properties->findItems("(0010,0010) Patient Name", Qt::MatchExactly, 0);
    ASSERT_EQ(refreshedPatients.size(), 1); EXPECT_TRUE(refreshedPatients.front()->isHidden());

    ImageMetadata nifti;
    nifti.attributes = {{"nifti_type", "1"}, {"pixdim[1]", "0.75"}, {"datatype", "512"}};
    window.addDocument(Image::allocate(PixelType::UInt16, {4, 4}, nifti));
    EXPECT_EQ(search->text(), "pixel spacing");
    search->setText("PIXDIM_1");
    const auto gridSpacing = properties->findItems("Grid Spacing (pixdim[1])", Qt::MatchExactly, 0);
    ASSERT_EQ(gridSpacing.size(), 1); EXPECT_FALSE(gridSpacing.front()->isHidden());
    const auto dataType = properties->findItems("Data Type (datatype)", Qt::MatchExactly, 0);
    ASSERT_EQ(dataType.size(), 1); EXPECT_TRUE(dataType.front()->isHidden());
    search->setText("5.12");
    EXPECT_FALSE(dataType.front()->isHidden()); EXPECT_TRUE(gridSpacing.front()->isHidden());
}

TEST(Desktop, PropertyRowsSupportMultipleSelectionAndContextAndKeyboardCopy)
{
    const QString description = "A, \"quoted\" value\nsecond line";
    const QString spacing = "4.418060\\4.418060";
    const QString firstCsv = "\"(0008,103e) Series Description\",\"A, \"\"quoted\"\" value\nsecond line\"";
    const QString middleCsv = "\"(0010,0010) Patient Name\",\"Example^Patient\"";
    const QString lastCsv = "\"(0028,0030) Pixel Spacing\",\"4.418060\\4.418060\"";
    ImageMetadata metadata;
    metadata.attributes = {{"DICOM.0008,103e", description}, {"DICOM.0010,0010", "Example^Patient"}, {"DICOM.0028,0030", spacing}};
    MainWindow window;
    window.addDocument(Image::allocate(PixelType::UInt16, {4, 4}, metadata));
    window.panel("properties")->resize(640, 400);
    window.showPanel("properties");
    QCoreApplication::processEvents();
    auto *properties = window.panel("properties")->findChild<QTreeWidget *>();
    auto *search = window.panel("properties")->findChild<QLineEdit *>("propertiesSearch");
    ASSERT_NE(properties, nullptr); ASSERT_NE(search, nullptr);
    ASSERT_EQ(properties->topLevelItemCount(), 3);
    auto *first = properties->topLevelItem(0);
    auto *middle = properties->topLevelItem(1);
    auto *last = properties->topLevelItem(2);
    const auto clickRow = [&](QTreeWidgetItem *item, Qt::MouseButton button, Qt::KeyboardModifiers modifiers = {}) {
        const auto position = properties->visualItemRect(item).center();
        const auto global = properties->viewport()->mapToGlobal(position);
        for (const auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            QMouseEvent event(type, position, global, button,
                type == QEvent::MouseButtonPress ? button : Qt::NoButton, modifiers);
            QCoreApplication::sendEvent(properties->viewport(), &event);
        }
    };
    const auto contextCopy = [&](QTreeWidgetItem *item, int actionIndex, bool multiple) {
        clickRow(item, Qt::RightButton);
        QTimer::singleShot(0, properties, [&, actionIndex, multiple] {
            auto *menu = properties->findChild<QMenu *>();
            EXPECT_NE(menu, nullptr);
            if (!menu) return;
            const QStringList labels = multiple ? QStringList{"Copy Rows", "Copy Properties", "Copy Values"}
                                               : QStringList{"Copy Row", "Copy Property", "Copy Value"};
            EXPECT_EQ(menu->actions().size(), 3);
            if (menu->actions().size() != 3) { menu->close(); return; }
            for (int index = 0; index < 3; ++index) EXPECT_EQ(menu->actions()[index]->text(), labels[index]);
            menu->setActiveAction(menu->actions()[actionIndex]);
            sendKey(menu, Qt::Key_Return);
        });
        const auto position = properties->visualItemRect(item).center();
        QContextMenuEvent event(QContextMenuEvent::Mouse, position, properties->viewport()->mapToGlobal(position));
        QCoreApplication::sendEvent(properties->viewport(), &event);
        return QApplication::clipboard()->text();
    };

    clickRow(first, Qt::LeftButton);
    EXPECT_EQ(contextCopy(first, 0, false), firstCsv);
    EXPECT_EQ(contextCopy(first, 1, false), "(0008,103e) Series Description");
    EXPECT_EQ(contextCopy(first, 2, false), description);
    sendKey(properties, Qt::Key_C, Qt::ControlModifier);
    EXPECT_EQ(QApplication::clipboard()->text(), firstCsv);
    clickRow(first, Qt::LeftButton);
    sendKey(properties, Qt::Key_Down, Qt::ShiftModifier);
    sendKey(properties, Qt::Key_Down, Qt::ShiftModifier);
    EXPECT_EQ(properties->selectedItems().size(), 3);
    clickRow(middle, Qt::LeftButton, Qt::ControlModifier);
    EXPECT_EQ(properties->selectedItems().size(), 2);
    clickRow(last, Qt::LeftButton);
    clickRow(first, Qt::LeftButton, Qt::ControlModifier);
    ASSERT_EQ(properties->selectedItems().size(), 2);
    EXPECT_EQ(contextCopy(first, 0, true), firstCsv + '\n' + lastCsv);
    EXPECT_EQ(contextCopy(first, 1, true), "(0008,103e) Series Description\n(0028,0030) Pixel Spacing");
    EXPECT_EQ(contextCopy(first, 2, true), description + '\n' + spacing);
    sendKey(properties, Qt::Key_C, Qt::ControlModifier);
    EXPECT_EQ(QApplication::clipboard()->text(), firstCsv + '\n' + lastCsv);
    EXPECT_EQ(contextCopy(middle, 0, false), middleCsv);
    EXPECT_EQ(contextCopy(middle, 1, false), "(0010,0010) Patient Name");
    EXPECT_EQ(contextCopy(middle, 2, false), "Example^Patient");

    properties->clearSelection(); first->setSelected(true); last->setSelected(true);
    search->setText("pixel spacing");
    EXPECT_EQ(contextCopy(last, 0, false), lastCsv);
    sendKey(properties, Qt::Key_C, Qt::ControlModifier);
    EXPECT_EQ(QApplication::clipboard()->text(), lastCsv);
    search->selectAll();
    sendKey(search, Qt::Key_C, Qt::ControlModifier);
    EXPECT_EQ(QApplication::clipboard()->text(), "pixel spacing");
    properties->clearSelection();
    sendKey(properties, Qt::Key_C, Qt::ControlModifier);
    EXPECT_EQ(QApplication::clipboard()->text(), "pixel spacing");
}

TEST(Desktop, SliceNavigationKeepsMetadataRoiAndHistoryItems)
{
    MainWindow window;
    auto *document = window.addDocument(Image::allocate(PixelType::UInt16, {8, 8, 3, 2, 2}));
    Roi roi; roi.name = "Saved patch"; roi.points = {{1, 1}, {3, 4}};
    document->rois.push_back(roi); document->changed();
    auto *properties = window.panel("properties")->findChild<QTreeWidget *>();
    auto *rois = window.panel("rois")->findChild<QListWidget *>();
    auto *history = window.panel("history")->findChild<QListWidget *>();
    ASSERT_NE(properties, nullptr); ASSERT_NE(rois, nullptr); ASSERT_NE(history, nullptr);
    properties->topLevelItem(0)->setExpanded(true);
    const QPersistentModelIndex propertyIndex(properties->model()->index(0, 0));
    const QPersistentModelIndex roiIndex(rois->model()->index(0, 0));
    const QPersistentModelIndex historyIndex(history->model()->index(0, 0));
    ASSERT_TRUE(propertyIndex.isValid()); ASSERT_TRUE(roiIndex.isValid()); ASSERT_TRUE(historyIndex.isValid());
    document->viewer->setSlice(2, 1, 1);
    EXPECT_TRUE(propertyIndex.isValid()); EXPECT_TRUE(roiIndex.isValid()); EXPECT_TRUE(historyIndex.isValid());
    EXPECT_TRUE(properties->topLevelItem(0)->isExpanded());
    const auto sliders = document->findChildren<QSlider *>();
    ASSERT_EQ(sliders.size(), 1);
    EXPECT_EQ(sliders[0]->value(), 2);
    EXPECT_EQ(document->viewer->channel(), 1u); EXPECT_EQ(document->viewer->time(), 1u);
    auto *position = document->findChild<QLabel *>("slicePosition");
    ASSERT_NE(position, nullptr);
    EXPECT_EQ(position->text(), "3/3");
    document->setSelection(roi);
    EXPECT_FALSE(propertyIndex.isValid()); // Document changes still refresh the panels.
}

TEST(Desktop, RoiMeasurementAndPixelEditsUseTheCurrentSliceAfterNavigation)
{
    MainWindow window;
    auto image = Image::allocate(PixelType::UInt16, {8, 8, 3});
    auto *pixels = reinterpret_cast<std::uint16_t *>(image.mutableData());
    std::fill(pixels, pixels + 192, std::uint16_t{100});
    for (int y = 1; y < 4; ++y) for (int x = 1; x < 3; ++x) pixels[128 + y * 8 + x] = 12;
    auto *document = window.addDocument(image);
    Roi roi; roi.name = "Persistent patch"; roi.points = {{1, 1}, {3, 4}};
    document->setSelection(roi);
    document->viewer->setSlice(2);
    window.executeCommand("analyze.measure");
    ASSERT_TRUE(finishJobs(window));
    auto *table = window.findChild<QTableView *>("measurementTable"); ASSERT_NE(table, nullptr);
    ASSERT_EQ(table->model()->rowCount(), 1);
    EXPECT_EQ(table->model()->data(table->model()->index(0, 2)).toULongLong(), 6u);
    EXPECT_DOUBLE_EQ(table->model()->data(table->model()->index(0, 4)).toDouble(), 12);
    window.executeCommand("edit.clear");
    ASSERT_TRUE(finishJobs(window));
    EXPECT_DOUBLE_EQ(document->image.sample(1, 1, 2), 0);
    EXPECT_DOUBLE_EQ(document->image.sample(0, 0, 2), 100);
    EXPECT_DOUBLE_EQ(document->image.sample(1, 1, 0), 100);
}

TEST(Desktop, WorkerOpensImageAndPublishesOnUiThread)
{
    QTemporaryDir directory;
    auto image = Image::allocate(PixelType::Float32, {12, 10, 3});
    reinterpret_cast<float *>(image.mutableData())[2 * 12 * 10 + 7 * 12 + 4] = -17.25f;
    writeImage(image, directory.filePath("input.tif"));
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
    writeImage(Image::allocate(PixelType::UInt8, {4, 4}), raster);
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
    writeImage(Image::allocate(PixelType::UInt8, {4, 4}), raster);
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
    writeImage(Image::allocate(PixelType::UInt16, {4, 4, 1}), raw);
    QFile header(directory.filePath("different-name.MHD"));
    ASSERT_TRUE(header.open(QIODevice::WriteOnly));
    header.write("ObjectType = Image\nNDims = 3\nBinaryData = True\nBinaryDataByteOrderMSB = False\n"
                 "ElementSpacing = 1 1 1\nDimSize = 4 4 1\nElementType = MET_USHORT\n"
                 "ElementDataFile = payloads/../payloads/scan data.raw\n");
    header.close();
    // A same-named raw file in another directory is still a separate image.
    const auto unrelated = directory.filePath("scan data.raw");
    writeImage(Image::allocate(PixelType::UInt16, {4, 4}), unrelated);
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
    writeImage(Image::allocate(PixelType::UInt8, {4, 4}), raster);
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
    writeImage(Image::allocate(PixelType::UInt16, {4, 4}), raw);
    writeImage(Image::allocate(PixelType::UInt8, {4, 4}), directory.filePath("ordinary.tif"));
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
    const auto rawPath = directory.filePath("cancel.bin");
    QFile file(rawPath); ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(QByteArray(16, char(0))); file.close();
    const auto imagePath = directory.filePath("next.tif");
    auto image = Image::allocate(PixelType::UInt8, {4, 4});
    image.mutableData()[15] = std::byte{173};
    writeImage(image, imagePath);
    MainWindow window;
    bool cancelled = false;
    QTimer::singleShot(0, &window, [&] {
        if (auto *dialog = window.findChild<QDialog *>("rawImportDialog")) {
            cancelled = true;
            dialog->reject();
        }
    });
    window.openPaths({rawPath, imagePath});
    ASSERT_TRUE(cancelled);
    ASSERT_TRUE(finishJobs(window));
    ASSERT_EQ(window.documents().size(), 1);
    EXPECT_EQ(window.activeDocument()->image.sample(3, 3), 173);
}

TEST(Desktop, RawWithSidecarOpensWithoutPrompting)
{
    QSettings settings; settings.remove("rawImport");
    QTemporaryDir directory;
    const auto path = directory.filePath("with-layout.bin");
    auto original = Image::allocate(PixelType::Float32, {3, 2, 2});
    original.setValue(std::array<std::size_t, 3>{1, 1, 1}, -2.5);
    writeImage(original, path);
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
        image.metadata().name = "Measured image";
        std::fill_n(image.mutableData(), image.byteCount(), std::byte{7});
        auto *document = window.addDocument(std::move(image));
        window.executeCommand("analyze.measure");
        ASSERT_TRUE(finishJobs(window));
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
            EXPECT_TRUE(csv.contains("Measured image"));
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
    document->setViewMode(DocumentWidget::ViewMode::Orthogonal);
    for (auto *view : document->views()) EXPECT_TRUE(view->window()->windowTitle().contains("original.tif *"));
    const auto output = directory.filePath("saved.tif");
    QTimer::singleShot(0, &window, [output] {
        for (auto *widget : QApplication::topLevelWidgets()) if (auto *dialog = qobject_cast<QFileDialog *>(widget)) {
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

TEST(Desktop, RoiRenameSurvivesAnAsynchronousPanelRefresh)
{
    MainWindow window;
    auto *document = window.addDocument(Image::allocate(PixelType::UInt8, {8, 8}));
    Roi roi; roi.name = "Before"; roi.points = {{0, 0}, {4, 4}};
    document->rois.push_back(roi); document->changed();
    document->setSelection(roi);
    auto *list = window.panel("rois")->findChild<QListWidget *>("roiList"); ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->count(), 1);
    QTimer::singleShot(0, &window, [document] {
        document->changed();
        for (auto *widget : QApplication::topLevelWidgets()) if (auto *dialog = qobject_cast<QInputDialog *>(widget)) {
            dialog->setTextValue("Renamed"); dialog->accept(); return;
        }
    });
    list->itemDoubleClicked(list->item(0));
    EXPECT_EQ(document->rois.front().name, "Renamed"); EXPECT_FALSE(document->modified);
    ASSERT_TRUE(document->selection);
    EXPECT_EQ(document->selection->name, "Renamed");
    window.commands().execute("analyze.measure");
    ASSERT_TRUE(finishJobs(window));
    auto *table = window.panel("measurements")->findChild<QTableView *>();
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->model()->data(table->model()->index(0, 1)).toString(), "Renamed");
    auto edited = *document->selection;
    edited.points[1] += QPointF(1, 0);
    document->viewer->roiEdited(edited);
    EXPECT_EQ(document->rois.front().name, "Renamed");
}

int main(int argc, char **argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication application(argc, argv);
    applyTheme();
    QCoreApplication::setOrganizationName("ImageCXXTests");
    QCoreApplication::setApplicationName("ImageCXXTests");
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
