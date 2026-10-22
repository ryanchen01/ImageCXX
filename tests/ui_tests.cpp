#include "mainwindow.h"
#include "io/image_io.h"
#include "ui/theme.h"
#include <dcmtk/dcmdata/dctk.h>
#include <gtest/gtest.h>
#include <limits>
#include <atomic>
#include <cmath>
#include <QApplication>
#include <QAction>
#include <QKeyEvent>
#include <QMenu>
#include <QSet>
#include <QSettings>
#include <QScreen>
#include <QCheckBox>
#include <QToolButton>
#include <QClipboard>
#include <QColorDialog>
#include <QImage>
#include <QCoreApplication>
#include <QDockWidget>
#include <QTabWidget>
#include <QToolBar>
#include <QEvent>
#include <QSlider>
#include <QElapsedTimer>
#include <QStandardItemModel>
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

TEST(Desktop, SynchronizedWindowsUseOneDocumentAndKeepOtherWindowsIndependent)
{
    MainWindow window;
    auto *toolbar = window.findChild<QToolBar *>("imageTools");
    auto *orthogonal = window.findChild<QAction *>("view.orthogonal");
    ASSERT_NE(toolbar, nullptr); ASSERT_NE(orthogonal, nullptr);
    auto *orthogonalButton = qobject_cast<QToolButton *>(toolbar->widgetForAction(orthogonal));
    ASSERT_NE(orthogonalButton, nullptr);
    EXPECT_FALSE(orthogonalButton->isEnabled());
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
    auto *other = window.addDocument(Image::allocate(PixelType::UInt8, {8, 8}));
    auto *document = window.addDocument(image);
    EXPECT_TRUE(window.findChild<QAction *>("view.orthogonal")->isEnabled());
    orthogonalButton->click();
    EXPECT_EQ(document->viewMode(), DocumentWidget::ViewMode::Orthogonal);
    EXPECT_TRUE(window.findChild<QAction *>("view.orthogonal")->isChecked());
    EXPECT_TRUE(orthogonalButton->isChecked());
    EXPECT_EQ(orthogonalButton->text(), "Ortho");
    for (auto *view : document->views())
        EXPECT_TRUE(view->window()->findChildren<QComboBox *>(QString{}, Qt::FindDirectChildrenOnly).isEmpty());
    EXPECT_EQ(window.documents().size(), 2);
    EXPECT_EQ(other->viewMode(), DocumentWidget::ViewMode::Single);
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
    window.findChild<QAction *>("view.orthogonal")->trigger();
    EXPECT_TRUE(window.findChild<QAction *>("view.orthogonal")->isChecked());
    EXPECT_EQ(document->views()[1]->window(), xzWindow);
    EXPECT_TRUE(xzWindow->isVisible());
    screenshot("orthogonal-toolbar.png");
    document->viewer->window()->findChild<QSlider *>("sliceSlider", Qt::FindDirectChildrenOnly)->setValue(25);
    xzWindow->findChild<QSlider *>("sliceSlider", Qt::FindDirectChildrenOnly)->setValue(50);
    yzWindow->findChild<QSlider *>("sliceSlider", Qt::FindDirectChildrenOnly)->setValue(80);
    EXPECT_EQ(document->views()[1]->slice(), 50u);
    EXPECT_EQ(document->viewer->slice(), 25u);
    EXPECT_EQ(document->views()[2]->slice(), 80u);
    screenshot("orthogonal-toolbar-navigation.png");
    QCoreApplication::sendEvent(document, &activated);
    window.commands().execute("view.linked"); settle();
    EXPECT_EQ(document->views().size(), 2);
    EXPECT_NE(document->viewer->window(), document->views()[1]->window());
    document->views()[1]->window()->resize(480, 420);
    document->views()[1]->window()->move(660, 160);
    auto *linkedWindow = document->views()[1]->window();
    linkedWindow->findChild<QSlider *>("sliceSlider", Qt::FindDirectChildrenOnly)->setValue(28);
    window.panel("display")->findChild<QSpinBox *>("displayChannel")->setValue(2);
    window.panel("display")->findChild<QSpinBox *>("displayTime")->setValue(2);
    EXPECT_EQ(document->viewer->slice(), 28u);
    EXPECT_EQ(other->viewer->slice(), 0u); EXPECT_DOUBLE_EQ(other->viewer->gamma(), 1);
    screenshot("linked-view-menu.png");
    window.commands().execute("view.single");
    EXPECT_EQ(document->views().size(), 1);
    auto *flat = window.addDocument(Image::allocate(PixelType::UInt8, {8, 8}));
    EXPECT_FALSE(window.findChild<QAction *>("view.orthogonal")->isEnabled());
    EXPECT_FALSE(orthogonalButton->isEnabled());
    EXPECT_EQ(flat->viewMode(), DocumentWidget::ViewMode::Single);
    document->setViewMode(DocumentWidget::ViewMode::Orthogonal);
    QPointer<QWidget> retiredWindow(document->views()[1]->window());
    document->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_TRUE(retiredWindow.isNull());
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
    auto *pixels = reinterpret_cast<std::uint16_t *>(image.mutableData());
    for (std::size_t i = 0; i < image.pixelCount(); ++i) pixels[i] = std::uint16_t(30000 + i * 17);
    auto *document = window.addDocument(image);
    Roi roi; roi.points = {{1, 1}, {5, 5}}; document->rois.push_back(roi);
    document->modified = true;
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
