#include "mainwindow.h"
#include "plots/plot_widget.h"
#include "io/image_io.h"
#include <gtest/gtest.h>
#include <limits>
#include <QApplication>
#include <QAction>
#include <QKeyEvent>
#include <QMenu>
#include <QSet>
#include <QSettings>
#include <QCheckBox>
#include <QToolButton>
#include <QClipboard>
#include <QImage>
#include <QCoreApplication>
#include <QDockWidget>
#include <QTabWidget>
#include <QToolBar>
#include <QEvent>
#include <QIcon>
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
}

TEST(Shortcuts, ImageJBindingsHaveNoConflictsAndSaveTargetsPixels)
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
    EXPECT_EQ(used.value(QKeySequence("S")), "file.save");
    EXPECT_EQ(used.value(QKeySequence("Ctrl+S")), "file.save");
    EXPECT_EQ(used.value(QKeySequence("Ctrl+Shift+P")), "view.properties");
    EXPECT_FALSE(used.contains(QKeySequence("Ctrl+Alt+S")));
    for (const auto &command : window.commands().commands())
        EXPECT_FALSE(command.id.startsWith("session."));
    EXPECT_EQ(used.value(QKeySequence("F")), "edit.fill");
    EXPECT_EQ(used.value(QKeySequence("L")), "commands.find");
    EXPECT_EQ(used.value(QKeySequence("Ctrl+Shift+Z")), "view.channels");
    EXPECT_EQ(used.value(QKeySequence("Ctrl+J")), "view.display");
    sendKey(&window, Qt::Key_J, Qt::ControlModifier);
    EXPECT_TRUE(window.panel("display")->isVisible());
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
    sendKey(doc->viewer, Qt::Key_V);
    EXPECT_DOUBLE_EQ(doc->image.sample(1, 1), 1200);
    sendKey(doc->viewer, Qt::Key_I, Qt::ShiftModifier); ASSERT_TRUE(finishJobs(window));
    EXPECT_DOUBLE_EQ(doc->image.sample(1, 1), 64335);
    sendKey(doc->viewer, Qt::Key_Z, Qt::ControlModifier);
    EXPECT_DOUBLE_EQ(doc->image.sample(1, 1), 1200);
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
    image.metadata().name = "float";
    writeImage(image, directory.filePath("input.tif"));
    MainWindow window; window.openPaths({directory.filePath("input.tif")});
    QElapsedTimer timer; timer.start();
    while ((window.jobs().busy() || !window.activeDocument()) && timer.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(2); }
    ASSERT_NE(window.activeDocument(), nullptr);
    EXPECT_EQ(window.activeDocument()->image.pixelType(), PixelType::Float32);
    EXPECT_EQ(window.activeDocument()->image.depth(), 3u);
    EXPECT_EQ(window.activeDocument()->thread(), QCoreApplication::instance()->thread());
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
        EXPECT_EQ(type->count(), 7);
        type->setCurrentIndex(type->findData("uint16"));
        EXPECT_TRUE(width->suffix().isEmpty());
        EXPECT_TRUE(height->suffix().isEmpty());
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
    // Two header bytes, three complete 2x2 uint16 images, then an incomplete image.
    file.write(QByteArray(2 + 3 * 8 + 3, char(0)));
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
        EXPECT_EQ(image.byteCount(), std::size_t(std::min(maximum, 3) * 8));
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
    EXPECT_EQ(settings.value("rawImport/v1/maximumImages").toInt(), 9999);
    EXPECT_EQ(settings.status(), QSettings::NoError);
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
    writeImage(Image::allocate(PixelType::UInt8, {4, 4}), imagePath);
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
    EXPECT_EQ(window.activeDocument()->image.metadata().sourcePath, imagePath);
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

TEST(Desktop, GeneratedFilterFormRunsThroughRegistryAndPreservesUndo)
{
    MainWindow window;
    auto image = Image::allocate(PixelType::UInt16, {8, 8});
    std::fill_n(reinterpret_cast<std::uint16_t *>(image.mutableData()), 64, std::uint16_t{1200});
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
    EXPECT_NEAR(document->image.sample(3, 3), 1200, 1);
    EXPECT_TRUE(document->modified);
    window.executeCommand("edit.undo");
    EXPECT_DOUBLE_EQ(document->image.sample(3, 3), 1200);
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
            EXPECT_EQ(csv.count('\n'), 2);
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
    auto *document = window.addDocument(Image::allocate(PixelType::UInt8, {8, 8}));
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
    window.executeCommand("file.save");
    QElapsedTimer timer; timer.start();
    while (window.jobs().busy() && timer.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(2); }
    ASSERT_FALSE(window.jobs().busy());
    EXPECT_TRUE(QFileInfo::exists(output)) << document->image.metadata().sourcePath.toStdString();
    EXPECT_FALSE(document->modified);
    EXPECT_FALSE(document->title().endsWith(" *"));
    EXPECT_EQ(document->rois.size(), 1u);
    EXPECT_EQ(document->image.metadata().sourcePath.toStdString(), output.toStdString());
}

TEST(Desktop, RoiRenameSurvivesAnAsynchronousPanelRefresh)
{
    MainWindow window;
    auto *document = window.addDocument(Image::allocate(PixelType::UInt8, {8, 8}));
    Roi roi; roi.name = "Before"; roi.points = {{0, 0}, {4, 4}};
    document->rois.push_back(roi); document->changed();
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
}

TEST(Plots, LoadsQtGraphsOnDemandAndAcceptsNumericalData)
{
    PlotWidget plot;
    plot.setData({{0, 10}, {1, 15}, {2, 12}}, "Distance", "Intensity");
    QCoreApplication::processEvents();
    EXPECT_TRUE(plot.ready());
}

TEST(Desktop, RendersSampleRoiMeasurementAndQtGraphsTogether)
{
    if (QApplication::platformName() == "offscreen") GTEST_SKIP() << "Requires a native GPU window";
    MainWindow window; window.show();
    window.executeCommand("file.sample");
    auto *document = window.activeDocument(); ASSERT_NE(document, nullptr);
    Roi roi; roi.name = "Cell group"; roi.type = RoiType::Ellipse; roi.points = {{100, 60}, {250, 220}};
    document->rois.push_back(roi); document->viewer->setRois(document->rois); document->changed();
    window.executeCommand("analyze.measure"); window.executeCommand("analyze.histogram");
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < 700 || window.jobs().busy()) {
        QCoreApplication::processEvents(); QThread::msleep(3);
        ASSERT_LT(timer.elapsed(), 10000);
    }
    EXPECT_FALSE(QIcon(":/icons/open.svg").isNull());
    ASSERT_NE(window.panel("histogram"), nullptr);
    EXPECT_TRUE(window.panel("histogram")->isWindow());
    EXPECT_TRUE(window.panel("histogram")->isVisible());
    document->viewer->setSlice(3);
    timer.restart();
    while (timer.elapsed() < 100) { QCoreApplication::processEvents(); QThread::msleep(2); }
    EXPECT_EQ(document->viewer->slice(), 3u);
    document->viewer->setSlice(0);
    timer.restart();
    while (timer.elapsed() < 100) { QCoreApplication::processEvents(); QThread::msleep(2); }
    const auto capture = qEnvironmentVariable("IMAGECXX_CAPTURE_PATH");
    if (!capture.isEmpty()) {
        EXPECT_TRUE(window.grab().save(capture));
        EXPECT_TRUE(document->grab().save(capture + "-image.png"));
        EXPECT_TRUE(window.panel("histogram")->grab().save(capture + "-histogram.png"));
    }
}

int main(int argc, char **argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName("ImageCXXTests");
    QCoreApplication::setApplicationName("ImageCXXTests");
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
