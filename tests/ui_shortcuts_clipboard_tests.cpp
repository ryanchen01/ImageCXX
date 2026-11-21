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

using namespace imagecxx;
using namespace imagecxx::test;

namespace {
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
} // namespace

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
    window.showPanel("rois");
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

TEST(Clipboard, InvertedCopyAndCutPngPreserveAlphaAndTypedPixels)
{
    for (const bool cut : {false, true}) for (const auto type : {PixelType::UInt16, PixelType::RGB8, PixelType::RGBA8}) {
        SCOPED_TRACE(pixelTypeName(type).toStdString() + (cut ? " cut" : " copy"));
        MainWindow window;
        auto image = Image::allocate(type, {4, 2});
        const bool color = componentCount(type) >= 3;
        for (std::size_t c = 0; c < componentCount(type); ++c) {
            image.setValue(std::array<std::size_t, 2>{1, 0}, color ? (c == 3 ? 127 : 30 * c) : 1000, c);
            image.setValue(std::array<std::size_t, 2>{2, 0}, color ? (c == 3 ? 127 : 255 - 30 * c) : 50000, c);
        }
        auto *doc = window.addDocument(image);
        doc->viewer->setWindow(color ? 0 : 1000, color ? 255 : 50000);
        doc->viewer->setInverted(true);
        Roi roi; roi.points = {{1, 0}, {3, 1}};
        doc->setSelection(roi);
        window.commands().execute(cut ? "edit.cut" : "edit.copy");
        // Export must retain the inversion from the moment of copying.
        doc->viewer->setInverted(false);
        ASSERT_TRUE(finishJobs(window));
        const auto png = QImage::fromData(QApplication::clipboard()->mimeData()->data("image/png"), "PNG");
        ASSERT_EQ(png.size(), QSize(320, 160));
        const int alpha = type == PixelType::RGBA8 ? 127 : 255;
        EXPECT_EQ(png.pixelColor(0, 0), color ? QColor(255, 225, 195, alpha) : QColor(Qt::white));
        EXPECT_EQ(png.pixelColor(319, 0), color ? QColor(0, 30, 60, alpha) : QColor(Qt::black));
        EXPECT_EQ(QApplication::clipboard()->image().pixelColor(0, 0), png.pixelColor(0, 0));
        window.commands().execute("edit.pasteSpecial", {{"destination", "New image"}});
        const auto &pasted = window.activeDocument()->image;
        EXPECT_EQ(pasted.pixelType(), type);
        EXPECT_EQ(pasted.width(), 2u);
        EXPECT_DOUBLE_EQ(pasted.sample(0, 0), color ? 0 : 1000);
        EXPECT_DOUBLE_EQ(pasted.sample(1, 0), color ? 255 : 50000);
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
        for (const auto &[command, roiType] : {std::pair{"edit.draw", RoiType::Rectangle},
                 std::pair{"edit.fill", RoiType::Rectangle}, std::pair{"edit.draw", RoiType::Line}}) {
            roi.type = roiType; doc->setSelection(roi);
            window.commands().execute(command); ASSERT_TRUE(finishJobs(window));
            for (std::size_t component = 0; component < componentCount(type); ++component) {
                const double value = component == 3 ? 23 : componentCount(type) > 1 ? (component == 0 ? 255 : 0) : expected;
                EXPECT_NEAR(doc->image.sample(1, 1, 1, 1, 1, component), value, type == PixelType::Float32 ? 1e-5 : 0);
            }
            const double stored = componentCount(type) > 1 ? 255 : type == PixelType::Float32 ? double(float(expected)) : expected;
            EXPECT_DOUBLE_EQ(doc->image.sample(3, 3, 1, 1, 1), QString(command) == "edit.draw" && roiType != RoiType::Line ? low : stored);
            EXPECT_DOUBLE_EQ(doc->image.sample(0, 0, 1, 1, 1), low);
            EXPECT_DOUBLE_EQ(doc->image.sample(5, 5, 1, 1, 1), roiType == RoiType::Line ? stored : high);
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
        roi.type = RoiType::Rectangle; doc->setSelection(roi);
        window.commands().execute("edit.clear"); ASSERT_TRUE(finishJobs(window));
        EXPECT_DOUBLE_EQ(doc->image.sample(1, 1, 1, 1, 1), 0);
    }
}

TEST(Desktop, DrawLineRasterizesEndpointsAllDirectionsAndClipsToImage)
{
    MainWindow window;
    chooseDrawingColor(window, Qt::white);
    auto *doc = window.addDocument(Image::allocate(PixelType::UInt8, {6, 6}));
    struct Case {
        QPointF first, last;
        std::vector<QPoint> pixels;
    };
    const std::vector<Case> cases{
        {{1, 2}, {4, 2}, {{1, 2}, {2, 2}, {3, 2}, {4, 2}}},
        {{2, 1}, {2, 4}, {{2, 1}, {2, 2}, {2, 3}, {2, 4}}},
        {{1, 1}, {4, 4}, {{1, 1}, {2, 2}, {3, 3}, {4, 4}}},
        {{1, 4}, {4, 1}, {{1, 4}, {2, 3}, {3, 2}, {4, 1}}},
        {{0, 1}, {4, 3}, {{0, 1}, {1, 2}, {2, 2}, {3, 3}, {4, 3}}},
        {{1, 0}, {3, 4}, {{1, 0}, {2, 1}, {2, 2}, {3, 3}, {3, 4}}},
        {{0, 0}, {6, 3}, {{0, 0}, {1, 1}, {2, 1}, {3, 2}, {4, 2}, {5, 3}}},
        {{0, 0}, {3, 6}, {{0, 0}, {1, 1}, {1, 2}, {2, 3}, {2, 4}, {3, 5}}},
        {{-2, -2}, {7, 7}, {{0, 0}, {1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}}},
        {{-2, 2}, {8, 2}, {{0, 2}, {1, 2}, {2, 2}, {3, 2}, {4, 2}, {5, 2}}},
        {{-2, 0}, {-2, 5}, {}},
        {{2.2, 3.8}, {2.7, 3.1}, {{2, 3}}},
        {{1.8, 1.2}, {4.2, 4.9}, {{1, 1}, {2, 2}, {3, 3}, {4, 4}}}
    };
    for (const auto &test : cases) for (const bool reverse : {false, true}) {
        SCOPED_TRACE(QString("(%1,%2) to (%3,%4), reverse=%5").arg(test.first.x()).arg(test.first.y())
            .arg(test.last.x()).arg(test.last.y()).arg(reverse).toStdString());
        Roi roi; roi.type = RoiType::Line;
        roi.points = reverse ? std::vector{test.last, test.first} : std::vector{test.first, test.last};
        doc->setSelection(roi);
        window.commands().execute("edit.draw"); ASSERT_TRUE(finishJobs(window));
        for (int y = 0; y < 6; ++y) for (int x = 0; x < 6; ++x) {
            const bool painted = std::find(test.pixels.begin(), test.pixels.end(), QPoint(x, y)) != test.pixels.end();
            EXPECT_DOUBLE_EQ(doc->image.sample(x, y), painted ? 255 : 0) << x << "," << y;
        }
        window.commands().execute("edit.undo");
        for (int y = 0; y < 6; ++y) for (int x = 0; x < 6; ++x)
            EXPECT_DOUBLE_EQ(doc->image.sample(x, y), 0);
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

TEST(Localization, TranslatedChoicesPreserveCommandValues)
{
    class Translator final : public QTranslator {
    public:
        bool isEmpty() const override { return false; }
        QString translate(const char *context, const char *source, const char *, int) const override
        {
            if (QByteArray(context) == "CommandChoices" || QByteArray(context) == "CommandCategories")
                return "Translated " + QString::fromUtf8(source);
            return {};
        }
    } translator;
    ASSERT_TRUE(QCoreApplication::installTranslator(&translator));
    MainWindow window;
    auto *doc = window.addDocument(Image::allocate(PixelType::UInt8, {8, 8}));
    EXPECT_EQ(window.menuBar()->actions().front()->text(), "Translated File");
    QTimer::singleShot(0, &window, [] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_NE(dialog, nullptr);
        auto *orientation = dialog->findChild<QComboBox *>("orientation");
        ASSERT_NE(orientation, nullptr);
        EXPECT_EQ(orientation->currentText(), "Translated Vertical");
        EXPECT_EQ(orientation->currentData().toString(), "Vertical");
        orientation->setCurrentIndex(orientation->findData("Horizontal"));
        dialog->accept();
    });
    window.executeCommand("gel.options");
    EXPECT_TRUE(doc->gelHorizontal);
    const auto values = window.commands().validate("process.gaussian", {});
    EXPECT_EQ(values.value("scope").toString(), "Full image");
    QCoreApplication::removeTranslator(&translator);
}

TEST(Shortcuts, OverlaySnapshotsStaySeparateFromRoisAndPixels)
{
    MainWindow window;
    auto *doc = window.addDocument(Image::allocate(PixelType::UInt16, {16, 12, 2}));
    Roi roi; roi.points = {{2, 2}, {8, 6}}; roi.name = "Label";
    doc->setSelection(roi);
    window.commands().execute("overlay.add");
    ASSERT_EQ(doc->annotations.size(), 1u);
    EXPECT_TRUE(doc->rois.empty()); EXPECT_EQ(doc->revision, 0u); EXPECT_FALSE(doc->modified);
    EXPECT_NE(doc->annotations[0].geometry.id, roi.id);
    roi.points[1] = {10, 10}; doc->editRoi(roi);
    EXPECT_EQ(doc->annotations[0].geometry.bounds(), QRectF(2, 2, 6, 4));
    EXPECT_EQ(doc->annotations[0].text, "Label");
    window.commands().execute("overlay.toggle"); EXPECT_FALSE(doc->annotationsVisible);
    window.commands().execute("overlay.removeLast"); EXPECT_TRUE(doc->annotations.empty());
}

TEST(Shortcuts, ControlPanelUsesRegistryAndPreservesTextInput)
{
    MainWindow window;
    sendKey(&window, Qt::Key_U, Qt::ShiftModifier);
    auto *panel = window.panel("controlPanel"); ASSERT_NE(panel, nullptr); EXPECT_TRUE(panel->isVisible());
    auto *tree = panel->findChild<QTreeWidget *>("controlPanelCommands"); ASSERT_NE(tree, nullptr);
    auto *filter = panel->findChild<QLineEdit *>(); filter->setText("Select all");
    QTreeWidgetItem *select = nullptr;
    QTreeWidgetItemIterator it(tree);
    while (*it) { if ((*it)->data(0, Qt::UserRole).toString() == "selection.all") select = *it; ++it; }
    ASSERT_NE(select, nullptr); EXPECT_TRUE(select->isDisabled()); EXPECT_FALSE(select->isHidden());
    auto *doc = window.addDocument(Image::allocate(PixelType::UInt8, {8, 8}));
    EXPECT_FALSE(select->isDisabled());
    tree->itemActivated(select, 0); ASSERT_TRUE(doc->selection);
    sendKey(filter, Qt::Key_A, Qt::ShiftModifier); EXPECT_TRUE(doc->selection);
    panel->hide(); window.commands().execute("view.controlPanel"); EXPECT_EQ(window.panel("controlPanel"), panel);
}

TEST(Shortcuts, HistogramAndRectangularProfileOptionsUseScientificValues)
{
    MainWindow window;
    auto image = Image::allocate(PixelType::UInt16, {4, 3});
    image.metadata().intensityScale = 2; image.metadata().intensityOffset = 10;
    for (std::size_t y = 0; y < 3; ++y) for (std::size_t x = 0; x < 4; ++x)
        image.setValue(std::array<std::size_t, 2>{x, y}, x + 10 * y);
    auto *doc = window.addDocument(image);
    Roi roi; roi.points = {{0, 0}, {4, 3}}; doc->setSelection(roi);
    window.commands().execute("analyze.profile"); ASSERT_TRUE(finishJobs(window));
    auto *series = window.panel("profile")->findChild<QLineSeries *>(); ASSERT_NE(series, nullptr);
    ASSERT_EQ(series->count(), 4); EXPECT_DOUBLE_EQ(series->at(0).y(), 30);
    window.commands().execute("analyze.profile.options", {{"rows", true}, {"calibrated", false}});
    ASSERT_TRUE(finishJobs(window)); ASSERT_EQ(series->count(), 3); EXPECT_DOUBLE_EQ(series->at(1).y(), 11.5);
    window.commands().execute("analyze.histogram.options", {{"bins", 2}, {"log", true}, {"calibrated", false}});
    ASSERT_TRUE(finishJobs(window)); series = window.panel("histogram")->findChild<QLineSeries *>(); ASSERT_NE(series, nullptr);
    ASSERT_EQ(series->count(), 2); EXPECT_NEAR(series->at(0).y(), std::log10(7), 1e-12);
    EXPECT_LT(series->at(0).x(), 10);
    EXPECT_DOUBLE_EQ(doc->image.sample(3, 2), 23);
}

TEST(Shortcuts, ModifierPlotShortcutsOpenGeneratedDialogsAndCanCancel)
{
    MainWindow window;
    auto *doc = window.addDocument(Image::allocate(PixelType::UInt8, {8, 8}));
    for (const auto key : {Qt::Key_H, Qt::Key_K}) {
        bool seen = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); ASSERT_NE(dialog, nullptr);
            seen = true; EXPECT_NE(dialog->findChild<QCheckBox *>("calibrated"), nullptr); dialog->reject();
        });
        sendKey(doc->viewer, key, Qt::AltModifier); EXPECT_TRUE(seen); EXPECT_FALSE(window.jobs().busy());
    }
}

TEST(Shortcuts, PrintPreviewCanProducePdfWithoutEditingScientificImage)
{
    QTemporaryDir directory; ASSERT_TRUE(directory.isValid());
    MainWindow window; auto *doc = window.addDocument(Image::allocate(PixelType::UInt16, {32, 24}));
    doc->viewer->setLut("viridis");
    const auto path = directory.filePath("print.pdf"); bool seen = false;
    QTimer::singleShot(0, &window, [&] {
        auto *dialog = qobject_cast<QPrintPreviewDialog *>(QApplication::activeModalWidget()); ASSERT_NE(dialog, nullptr);
        seen = true; dialog->printer()->setOutputFormat(QPrinter::PdfFormat); dialog->printer()->setOutputFileName(path);
        auto *preview = dialog->findChild<QPrintPreviewWidget *>(); ASSERT_NE(preview, nullptr);
        preview->print(); dialog->reject();
    });
    window.commands().execute("file.printPreview");
    EXPECT_TRUE(seen); QFile pdf(path); ASSERT_TRUE(pdf.open(QIODevice::ReadOnly)); EXPECT_EQ(pdf.read(4), "%PDF");
    EXPECT_FALSE(doc->modified); EXPECT_EQ(doc->revision, 0u);
}
