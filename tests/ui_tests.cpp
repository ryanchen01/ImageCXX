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

TEST(Desktop, HiddenPanelsDeferRefreshAndPropertiesOnlyTrackMetadataChanges)
{
    MainWindow window;
    auto *document = window.addDocument(Image::allocate(PixelType::UInt8, {8, 8, 3}));
    for (const auto *id : {"display", "properties", "rois", "history", "measurements"})
        EXPECT_EQ(window.panel(id), nullptr);
    window.showPanel("properties");
    window.showPanel("rois");
    window.showPanel("history");
    auto *properties = window.panel("properties")->findChild<QTreeWidget *>();
    auto *rois = window.panel("rois")->findChild<QListWidget *>();
    auto *history = window.panel("history")->findChild<QListWidget *>();
    ASSERT_NE(properties, nullptr); ASSERT_NE(rois, nullptr); ASSERT_NE(history, nullptr);
    QPersistentModelIndex propertyIndex(properties->model()->index(0, 0));
    Roi roi; roi.name = "Patch"; roi.points = {{1, 1}, {3, 3}};
    document->rois.push_back(roi); document->setSelection(roi);
    EXPECT_TRUE(propertyIndex.isValid());
    EXPECT_EQ(rois->count(), 1);
    for (const auto *id : {"properties", "rois", "history"}) window.panel(id)->hide();
    QPersistentModelIndex roiIndex(rois->model()->index(0, 0));
    QPersistentModelIndex historyIndex(history->model()->index(0, 0));
    document->image.metadata().name = "Renamed";
    document->rois.push_back(roi);
    document->history.push(document->image, "Snapshot");
    document->changed();
    EXPECT_TRUE(propertyIndex.isValid());
    EXPECT_TRUE(roiIndex.isValid()); EXPECT_TRUE(historyIndex.isValid());
    window.showPanel("properties");
    EXPECT_FALSE(propertyIndex.isValid());
    EXPECT_EQ(properties->topLevelItem(0)->text(1), "Renamed");
    window.showPanel("rois");
    window.showPanel("history");
    EXPECT_EQ(rois->count(), 2);
    EXPECT_EQ(history->item(0)->text(), "Snapshot");
    window.showPanel("display");
    auto *display = window.panel("display");
    display->hide();
    document->viewer->setGamma(2);
    window.showPanel("display");
    EXPECT_EQ(window.panel("display"), display);
    EXPECT_DOUBLE_EQ(display->findChild<QDoubleSpinBox *>("displayGamma")->value(), 2);
}

TEST(Desktop, ActivatingAnotherDocumentTrimsPreviousViews)
{
    MainWindow window;
    auto *first = window.addDocument(Image::allocate(PixelType::UInt8, {16, 16, 6}));
    // Keep rendering out of this cache test, including platforms without QRhi.
    QObject::disconnect(first->viewer, &ImageViewer::sliceReady, first, nullptr);
    ASSERT_TRUE(waitFor([&] { return first->viewer->decodedCacheBytes() == 2 * 16 * 16 * sizeof(float); }));
    auto *second = window.addDocument(Image::allocate(PixelType::UInt8, {16, 16, 6}));
    QObject::disconnect(second->viewer, &ImageViewer::sliceReady, second, nullptr);
    EXPECT_EQ(first->viewer->decodedCacheBytes(), 0u);
    ASSERT_TRUE(waitFor([&] { return second->viewer->decodedCacheBytes() > 0; }));
    QEvent activated(QEvent::WindowActivate);
    QCoreApplication::sendEvent(first, &activated);
    EXPECT_EQ(window.activeDocument(), first);
    EXPECT_EQ(second->viewer->decodedCacheBytes(), 0u);
    first->viewer->setSlice(3);
    ASSERT_TRUE(waitFor([&] { return first->viewer->decodedCacheBytes() == 3 * 16 * 16 * sizeof(float); }));
}

TEST(Desktop, StartsCompactAndOpensIndependentImageAndToolWindows)
{
    MainWindow window; window.show(); QCoreApplication::processEvents();
    EXPECT_LE(window.width(), 720); EXPECT_LE(window.height(), 160);
    EXPECT_TRUE(window.documents().empty());
    EXPECT_EQ(window.findChildren<QTabWidget *>().size(), 0);
    EXPECT_EQ(window.findChildren<QDockWidget *>().size(), 0);
    for (const auto *id : {"display", "properties", "rois", "history", "measurements"}) {
        EXPECT_EQ(window.panel(id), nullptr);
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

TEST(Desktop, PointerStartsSelectedBeforeSelectionToolsAndAppliesToEveryPlane)
{
    MainWindow window;
    auto *toolbar = window.findChild<QToolBar *>("imageTools");
    ASSERT_NE(toolbar, nullptr);
    const auto actions = toolbar->actions();
    auto pointer = std::find_if(actions.begin(), actions.end(), [](auto *action) { return action->text() == "Pointer"; });
    ASSERT_NE(pointer, actions.end());
    auto rectangle = std::next(pointer);
    ASSERT_NE(rectangle, actions.end());
    EXPECT_EQ((*rectangle)->text(), "Rectangle");
    EXPECT_TRUE((*pointer)->isChecked()); EXPECT_FALSE((*rectangle)->isChecked());
    EXPECT_FALSE((*pointer)->icon().pixmap(24, 24).isNull());
    auto *document = window.addDocument(Image::allocate(PixelType::UInt8, {20, 16, 8}));
    EXPECT_EQ(document->viewer->tool(), ImageViewer::Tool::Pointer);
    document->setOrthogonalViews(true);
    (*rectangle)->trigger();
    EXPECT_EQ(document->viewer->tool(), ImageViewer::Tool::Rectangle);
    for (auto *view : document->views()) if (view != document->viewer)
        EXPECT_EQ(view->tool(), ImageViewer::Tool::Pointer);
    (*pointer)->trigger();
    for (auto *view : document->views()) EXPECT_EQ(view->tool(), ImageViewer::Tool::Pointer);
    EXPECT_TRUE((*pointer)->isChecked()); EXPECT_FALSE((*rectangle)->isChecked());
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
    EXPECT_TRUE(document->orthogonalViews());
    EXPECT_TRUE(window.findChild<QAction *>("view.orthogonal")->isChecked());
    for (auto *view : document->views())
        EXPECT_TRUE(view->window()->findChildren<QComboBox *>(QString{}, Qt::FindDirectChildrenOnly).isEmpty());
    EXPECT_EQ(window.documents().size(), 2);
    EXPECT_FALSE(other->orthogonalViews());
    EXPECT_FALSE(other->isLinked());
    QPointer<QWidget> previousXzWindow(document->views()[1]->window());
    QPointer<QWidget> previousYzWindow(document->views()[2]->window());
    window.findChild<QAction *>("view.single")->trigger();
    EXPECT_FALSE(document->orthogonalViews());
    EXPECT_FALSE(document->isLinked());
    EXPECT_FALSE(orthogonal->isChecked());
    EXPECT_TRUE(window.findChild<QAction *>("view.single")->isChecked());
    ASSERT_EQ(document->views().size(), 1);
    EXPECT_TRUE(previousXzWindow.isNull()); EXPECT_TRUE(previousYzWindow.isNull());
    EXPECT_EQ(window.documents().size(), 2);
    EXPECT_FALSE(other->orthogonalViews());
    EXPECT_FALSE(other->isLinked());
    orthogonal->trigger();
    EXPECT_TRUE(document->orthogonalViews());
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
    EXPECT_EQ(document->views().size(), 3);
    EXPECT_TRUE(document->orthogonalViews());
    EXPECT_TRUE(document->isLinked());
    EXPECT_TRUE(other->isLinked());
    EXPECT_TRUE(window.findChild<QAction *>("view.linked")->isChecked());
    EXPECT_TRUE(window.findChild<QAction *>("view.orthogonal")->isChecked());
    EXPECT_EQ(window.documents().size(), 2);
    document->findChild<QSlider *>("sliceSlider", Qt::FindDirectChildrenOnly)->setValue(28);
    window.showPanel("display");
    window.panel("display")->findChild<QSpinBox *>("displayChannel")->setValue(2);
    window.panel("display")->findChild<QSpinBox *>("displayTime")->setValue(2);
    EXPECT_EQ(document->viewer->slice(), 28u);
    EXPECT_EQ(other->viewer->slice(), 0u); EXPECT_DOUBLE_EQ(other->viewer->gamma(), 1);
    screenshot("linked-view-menu.png");
    window.commands().execute("view.single");
    EXPECT_EQ(document->views().size(), 1);
    auto *flat = window.addDocument(Image::allocate(PixelType::UInt8, {8, 8}));
    EXPECT_FALSE(window.findChild<QAction *>("view.orthogonal")->isEnabled());
    EXPECT_FALSE(flat->orthogonalViews());
    EXPECT_FALSE(flat->isLinked());
    document->setOrthogonalViews(true);
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
    document->setOrthogonalViews(true);
    ASSERT_EQ(document->views().size(), 3);
    EXPECT_EQ(document->views()[1]->window()->size(), xzSize);
    EXPECT_EQ(document->views()[2]->window()->size(), yzSize);
    for (auto *view : document->views())
        EXPECT_EQ(view->window()->minimumSize(), document->minimumSize());
    document->setOrthogonalViews(false);
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
    // Let initial decoding show the windows before opening a modal picker. Otherwise
    // offscreen QRhi failure can close a window still waiting for its first slice.
    ASSERT_TRUE(waitFor([&] { return first->isVisible() && second->isVisible() && third->isVisible(); }));
    EXPECT_TRUE(action->isEnabled());
    QTimer::singleShot(0, &window, [] {
        auto *picker = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_NE(picker, nullptr); picker->reject();
    });
    window.commands().execute("view.linked");
    EXPECT_FALSE(third->isLinked());
    QTimer::singleShot(0, &window, [] {
        auto *picker = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_NE(picker, nullptr);
        auto *images = picker->findChild<QListWidget *>("linkedImages");
        ASSERT_NE(images, nullptr); ASSERT_EQ(images->count(), 2);
        EXPECT_NE(images->item(0)->text(), images->item(1)->text());
        images->item(1)->setCheckState(Qt::Checked); picker->accept();
    });
    window.commands().execute("view.linked");
    EXPECT_FALSE(first->isLinked());
    EXPECT_TRUE(second->isLinked());
    EXPECT_TRUE(third->isLinked());
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
    ASSERT_EQ(window.documents().size(), 3);
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
    EXPECT_FALSE(third->isLinked());
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
    EXPECT_FALSE(first->isLinked());
    EXPECT_FALSE(second->isLinked());
}

TEST(Desktop, OrthogonalViewsWorkIndependentlyWithLinkedViews)
{
    MainWindow window;
    auto *singleAction = window.findChild<QAction *>("view.single");
    auto *linkedAction = window.findChild<QAction *>("view.linked");
    auto *orthogonalAction = window.findChild<QAction *>("view.orthogonal");
    ASSERT_NE(singleAction, nullptr);
    ASSERT_NE(linkedAction, nullptr);
    ASSERT_NE(orthogonalAction, nullptr);

    ImageMetadata flatMeta, coarseMeta, fineMeta;
    flatMeta.axes = {{"X", 2, 0, "mm"}, {"Y", 2, 0, "mm"}};
    coarseMeta.axes = {{"X", 2, 0, "mm"}, {"Y", 2, 0, "mm"}, {"Z", 2, 0, "mm"}};
    fineMeta.axes = {{"X", 1, 0, "mm"}, {"Y", 1, 0, "mm"}, {"Z", 1, 0, "mm"}};
    auto *flat = window.addDocument(Image::allocate(PixelType::UInt8, {10, 8}, flatMeta));
    auto *first = window.addDocument(Image::allocate(PixelType::UInt8, {10, 8, 6}, coarseMeta));
    auto *second = window.addDocument(Image::allocate(PixelType::UInt8, {40, 32, 12}, fineMeta));
    ASSERT_TRUE(waitFor([&] { return flat->isVisible() && first->isVisible() && second->isVisible(); }));
    first->linkTo({second, flat});
    QEvent activated(QEvent::WindowActivate);
    QCoreApplication::sendEvent(first, &activated);
    EXPECT_TRUE(linkedAction->isChecked());
    EXPECT_FALSE(orthogonalAction->isChecked());
    EXPECT_FALSE(singleAction->isChecked());

    // Opening orthogonal views on a linked group opens them for every 3D member and keeps them linked.
    orthogonalAction->trigger();
    EXPECT_TRUE(first->isLinked());
    EXPECT_TRUE(second->isLinked());
    EXPECT_TRUE(flat->isLinked());
    EXPECT_TRUE(first->orthogonalViews());
    EXPECT_TRUE(second->orthogonalViews());
    EXPECT_FALSE(flat->orthogonalViews());
    ASSERT_EQ(first->views().size(), 3);
    ASSERT_EQ(second->views().size(), 3);
    EXPECT_EQ(flat->views().size(), 1);
    EXPECT_TRUE(linkedAction->isChecked());
    EXPECT_TRUE(orthogonalAction->isChecked());
    EXPECT_FALSE(singleAction->isChecked());

    // Navigating in first's XZ updates second's orthogonal views.
    first->views()[1]->positionPicked({4.5, 3.5});
    EXPECT_EQ(second->spatialPosition()[0], 8u);
    EXPECT_EQ(second->spatialPosition()[2], 6u);
    EXPECT_EQ(second->views()[0]->slice(), 6u);
    EXPECT_EQ(second->views()[2]->slice(), 8u);

    // Toggling orthogonal views from a 2D member of a mixed linked group works on the group's 3D members.
    QCoreApplication::sendEvent(flat, &activated);
    EXPECT_TRUE(orthogonalAction->isEnabled());
    EXPECT_TRUE(orthogonalAction->isChecked());

    // Toggling orthogonal views off closes XZ/YZ while keeping the linked group intact and physically synchronized.
    ASSERT_TRUE(waitFor([&] {
        return first->views()[1]->window()->isVisible() && second->views()[1]->window()->isVisible();
    }));
    orthogonalAction->trigger();
    EXPECT_FALSE(first->orthogonalViews());
    EXPECT_FALSE(second->orthogonalViews());
    EXPECT_TRUE(first->isLinked());
    EXPECT_TRUE(second->isLinked());
    EXPECT_TRUE(linkedAction->isChecked());
    EXPECT_FALSE(orthogonalAction->isChecked());
    EXPECT_TRUE(orthogonalAction->isEnabled());
    EXPECT_NEAR(first->viewer->zoom(), second->viewer->zoom() * 2.0, 1e-12);
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
    document->setOrthogonalViews(true);
    for (auto *view : document->views()) {
        auto *imageWindow = view->window();
        imageWindow->hide(); imageWindow->move(5000, -5000); imageWindow->show();
        QCoreApplication::processEvents();
        EXPECT_TRUE(imageWindow->screen()->availableGeometry().contains(imageWindow->frameGeometry()));
    }
    window.showPanel("display");
    auto *panel = window.panel("display");
    panel->hide(); panel->move(5000, -5000); panel->show();
    QCoreApplication::processEvents();
    EXPECT_TRUE(panel->screen()->availableGeometry().contains(panel->frameGeometry()));
}

TEST(Desktop, DisplayChannelTimeSelectorsSynchronizeViewsAndFollowTheActiveDocument)
{
    MainWindow window;
    auto *document = window.addDocument(Image::allocate(PixelType::UInt16, {10, 8, 6, 2, 4}));
    document->setOrthogonalViews(true);
    window.showPanel("display");
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
    window.showPanel("display");
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
    document->setOrthogonalViews(true);
    Roi roi; roi.name = "Saved selection"; roi.points = {{1, 1}, {4, 4}};
    document->rois.push_back(roi); document->setSelection(roi);
    window.showPanel("properties");
    auto *properties = window.panel("properties")->findChild<QTreeWidget *>();
    window.showPanel("rois");
    auto *rois = window.panel("rois")->findChild<QListWidget *>();
    window.showPanel("history");
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
    window.showPanel("display");
    EXPECT_EQ(window.panel("display")->findChild<QSpinBox *>("displayChannel")->value(), 2);
    EXPECT_EQ(window.panel("display")->findChild<QSpinBox *>("displayTime")->value(), 3);
    for (auto *view : document->views()) { EXPECT_EQ(view->channel(), 1u); EXPECT_EQ(view->time(), 2u); }
}

TEST(Desktop, DisplaySlidersTrackValuesAutoRangeAndActiveDocument)
{
    MainWindow window;
    window.showPanel("display");
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
    window.showPanel("properties");
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
    window.showPanel("properties");
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
        writeImageWithMetadata(Image::allocate(PixelType::UInt16, {4, 4, 2}, metadata), path);
        auto image = readImage(path);
        ASSERT_TRUE(image.metadata().attributes.contains("nifti_type"));
        image.metadata().attributes.insert("Experiment", "Unrelated metadata");
        if (extension == "nii.gz") image.metadata().sourcePath.clear();
        const auto attributes = image.metadata().attributes;

        MainWindow window;
        auto *document = window.addDocument(image);
        window.showPanel("properties");
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
    window.showPanel("properties");
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
    window.showPanel("properties");
    window.panel("properties")->resize(640, 400);
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
    window.showPanel("properties");
    auto *properties = window.panel("properties")->findChild<QTreeWidget *>();
    window.showPanel("rois");
    auto *rois = window.panel("rois")->findChild<QListWidget *>();
    window.showPanel("history");
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
    EXPECT_TRUE(propertyIndex.isValid()); // ROI edits leave unchanged metadata intact.
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

TEST(Desktop, MenusHideIconsKeepToolbarIconsAndGroupCommands)
{
    MainWindow window;
    auto *doc = window.addDocument(Image::allocate(PixelType::UInt8, {16, 16, 4}));
    window.show();
    EXPECT_TRUE(QApplication::testAttribute(Qt::AA_DontShowIconsInMenus));

    const auto topActions = window.menuBar()->actions();
    ASSERT_EQ(topActions.size(), 7);
    auto menuByTitle = [&](const QString &title) -> QMenu * {
        for (auto *action : topActions)
            if (action->text() == title) return action->menu();
        return nullptr;
    };
    auto *fileMenu = menuByTitle("File"); ASSERT_NE(fileMenu, nullptr);
    auto *editMenu = menuByTitle("Edit"); ASSERT_NE(editMenu, nullptr);
    auto *processMenu = menuByTitle("Process"); ASSERT_NE(processMenu, nullptr);
    auto *analyzeMenu = menuByTitle("Analyze"); ASSERT_NE(analyzeMenu, nullptr);
    auto *viewMenu = menuByTitle("View"); ASSERT_NE(viewMenu, nullptr);
    auto *windowMenu = menuByTitle("Window"); ASSERT_NE(windowMenu, nullptr);
    auto *helpMenu = menuByTitle("Help"); ASSERT_NE(helpMenu, nullptr);

    // Trigger dynamic Window menu twice to ensure shared window.* actions survive clear().
    QMetaObject::invokeMethod(windowMenu, "aboutToShow");
    QMetaObject::invokeMethod(windowMenu, "aboutToShow");

    auto checkNoMenuIcons = [](auto &&self, QMenu *menu) -> void {
        for (auto *action : menu->actions()) {
            EXPECT_FALSE(action->isIconVisibleInMenu()) << action->objectName().toStdString();
            if (auto *sub = action->menu()) self(self, sub);
        }
    };
    for (auto *menu : {fileMenu, editMenu, processMenu, analyzeMenu, viewMenu, windowMenu, helpMenu,
            window.findChild<QMenu *>("lutMenu")}) {
        ASSERT_NE(menu, nullptr);
        checkNoMenuIcons(checkNoMenuIcons, menu);
    }

    // Toolbar actions keep their icons, and Stk stays compact on the toolbar.
    for (const char *id : {"file.open", "analyze.measure", "analyze.histogram", "edit.undo", "edit.color"}) {
        auto *action = window.findChild<QAction *>(id);
        ASSERT_NE(action, nullptr) << id;
        EXPECT_FALSE(action->icon().pixmap(24, 24).isNull()) << id;
    }
    auto *findAction = window.findChild<QAction *>("commands.find");
    ASSERT_NE(findAction, nullptr);
    EXPECT_TRUE(findAction->icon().isNull());
    auto *stacksButton = window.findChild<QToolButton *>("stacksButton");
    ASSERT_NE(stacksButton, nullptr);
    EXPECT_EQ(stacksButton->text(), "Stk");

    // File: New first, Recent files before Quit, Quit last.
    const auto fileActions = fileMenu->actions();
    ASSERT_FALSE(fileActions.isEmpty());
    EXPECT_EQ(fileActions.front()->objectName(), "file.new");
    EXPECT_EQ(fileActions.back()->text(), "Quit");
    auto *recentMenu = window.findChild<QMenu *>("recentMenu");
    ASSERT_NE(recentMenu, nullptr);
    const int recentIdx = int(fileActions.indexOf(recentMenu->menuAction()));
    EXPECT_GT(recentIdx, 0);
    EXPECT_LT(recentIdx, fileActions.size() - 1);

    // Edit: Cut before Copy, Selection & Overlay submenus, Find command last.
    const auto editActions = editMenu->actions();
    EXPECT_LT(editActions.indexOf(window.findChild<QAction *>("edit.cut")),
              editActions.indexOf(window.findChild<QAction *>("edit.copy")));
    auto *selectionMenu = window.findChild<QMenu *>("selectionMenu");
    auto *overlayMenu = window.findChild<QMenu *>("overlayMenu");
    ASSERT_NE(selectionMenu, nullptr);
    ASSERT_NE(overlayMenu, nullptr);
    EXPECT_TRUE(editActions.contains(selectionMenu->menuAction()));
    EXPECT_TRUE(editActions.contains(overlayMenu->menuAction()));
    EXPECT_EQ(editActions.back(), findAction);

    // Process: Repeat first, Stacks submenu included, Batch last.
    const auto processActions = processMenu->actions();
    auto *stacksMenu = window.findChild<QMenu *>("stacksMenu");
    ASSERT_NE(stacksMenu, nullptr);
    EXPECT_EQ(processActions.front()->objectName(), "process.repeat");
    EXPECT_TRUE(processActions.contains(stacksMenu->menuAction()));
    EXPECT_EQ(processActions.back()->objectName(), "process.batch");

    // Analyze: Measure first, histogram variants grouped, Gels submenu last with options at end.
    const auto analyzeActions = analyzeMenu->actions();
    auto *gelsMenu = window.findChild<QMenu *>("gelsMenu");
    ASSERT_NE(gelsMenu, nullptr);
    EXPECT_EQ(analyzeActions.front()->objectName(), "analyze.measure");
    const int histIdx = int(analyzeActions.indexOf(window.findChild<QAction *>("analyze.histogram")));
    EXPECT_EQ(analyzeActions[histIdx + 1]->objectName(), "analyze.histogram.log");
    EXPECT_EQ(analyzeActions[histIdx + 2]->objectName(), "analyze.histogram.options");
    EXPECT_EQ(analyzeActions.back(), gelsMenu->menuAction());
    EXPECT_EQ(gelsMenu->actions().back()->objectName(), "gel.options");

    // View omits duplicate panel aliases & window commands; Window holds window.* actions.
    const auto viewActions = viewMenu->actions();
    for (const char *omitted : {"view.channels", "view.info", "window.close", "window.showAll", "window.next", "window.launcher"}) {
        auto *action = window.findChild<QAction *>(omitted);
        ASSERT_NE(action, nullptr) << omitted;
        EXPECT_FALSE(viewActions.contains(action)) << omitted;
    }
    EXPECT_TRUE(window.actions().contains(window.findChild<QAction *>("view.channels")));
    EXPECT_TRUE(window.actions().contains(window.findChild<QAction *>("view.info")));
    const auto windowActions = windowMenu->actions();
    for (const char *id : {"window.launcher", "window.showAll", "window.next", "window.close"})
        EXPECT_TRUE(windowActions.contains(window.findChild<QAction *>(id))) << id;
    EXPECT_EQ(windowActions.back()->objectName(), "window.close");

    // Shortcut aliases still open their panels via keyboard dispatch, and menus do not reserve an empty icon gutter.
    EXPECT_EQ(fileMenu->style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, fileMenu), 0);
    sendKey(doc->viewer, Qt::Key_I);
    ASSERT_NE(window.panel("properties"), nullptr);
    EXPECT_TRUE(window.panel("properties")->isVisible());
    sendKey(doc->viewer, Qt::Key_Z, Qt::ShiftModifier);
    ASSERT_NE(window.panel("display"), nullptr);
    EXPECT_TRUE(window.panel("display")->isVisible());
    // Border sits flush on the popup edge with no corner slivers from border-radius.
    fileMenu->popup(window.mapToGlobal(QPoint(20, 40)));
    QCoreApplication::processEvents();
    const auto menuImage = fileMenu->grab().toImage();
    EXPECT_EQ(menuImage.pixelColor(0, 0), QColor("#D5CEC2"));
    fileMenu->hide();
}

TEST(Desktop, OpeningImageFocusesOpenedWindowWithoutLeavingNoActivateAttribute)
{
    MainWindow window;
    activateApplicationWindow(&window, "wayland-token", "launcher-1_TIME12345");
    activateApplicationWindow(&window);
    EXPECT_TRUE(window.isVisible());
    EXPECT_TRUE(qEnvironmentVariableIsEmpty("XDG_ACTIVATION_TOKEN"));
    QCoreApplication::processEvents();

    auto *first = window.addDocument(Image::allocate(PixelType::UInt8, {16, 16}));
    ASSERT_TRUE(waitFor([&] { return first->isVisible(); }));
    EXPECT_FALSE(first->testAttribute(Qt::WA_ShowWithoutActivating));
    EXPECT_EQ(window.activeDocument(), first);
    EXPECT_TRUE(waitFor([&] { return QApplication::activeWindow() == first && first->viewer->hasFocus(); }));

    auto *second = window.addDocument(Image::allocate(PixelType::UInt8, {16, 16}));
    ASSERT_TRUE(waitFor([&] { return second->isVisible(); }));
    EXPECT_FALSE(second->testAttribute(Qt::WA_ShowWithoutActivating));
    EXPECT_EQ(window.activeDocument(), second);
    EXPECT_TRUE(waitFor([&] { return QApplication::activeWindow() == second && second->viewer->hasFocus(); }));

    // A document that finishes showing after the user switches back to an earlier window
    // does not steal focus from the earlier window's controls and does not leave
    // WA_ShowWithoutActivating stuck on itself.
    auto *slider = first->findChild<QSlider *>("sliceSlider");
    ASSERT_NE(slider, nullptr);
    slider->setFocusPolicy(Qt::StrongFocus);
    slider->setEnabled(true);
    auto *third = window.addDocument(Image::allocate(PixelType::UInt8, {16, 16}));
    QEvent activated(QEvent::WindowActivate);
    QCoreApplication::sendEvent(first, &activated);
    first->activateWindow();
    slider->setFocus();
    ASSERT_EQ(window.activeDocument(), first);
    ASSERT_TRUE(waitFor([&] { return third->isVisible(); }));
    EXPECT_FALSE(third->testAttribute(Qt::WA_ShowWithoutActivating));
    EXPECT_EQ(window.activeDocument(), first);
    EXPECT_TRUE(waitFor([&] { return QApplication::activeWindow() == first && slider->hasFocus(); }));

    // A deferred activation token passed to openPaths survives a scan error when a valid
    // image is also in the request, and is consumed when the opened image shows.
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto imagePath = directory.filePath("opened.png");
    const auto missingPath = directory.filePath("missing.png");
    auto sample = Image::allocate(PixelType::UInt8, {8, 8});
    writeImageWithMetadata(sample, imagePath, {}, {}, false);
    QString tokenOnShow;
    struct TokenSpy : QObject {
        QString *captured;
        bool eventFilter(QObject *watched, QEvent *event) override
        {
            if (event->type() == QEvent::Show && qobject_cast<DocumentWidget *>(watched))
                *captured = qEnvironmentVariable("XDG_ACTIVATION_TOKEN");
            return false;
        }
    } spy;
    spy.captured = &tokenOnShow;
    qApp->installEventFilter(&spy);
    QTimer dismissWarning;
    dismissWarning.setInterval(10);
    QObject::connect(&dismissWarning, &QTimer::timeout, &window, [] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) box->accept();
    });
    dismissWarning.start();
    window.openPaths({imagePath, missingPath}, "deferred-wayland-token");
    ASSERT_TRUE(finishJobs(window));
    dismissWarning.stop();
    auto *opened = window.activeDocument();
    ASSERT_NE(opened, nullptr);
    ASSERT_NE(opened, first);
    ASSERT_TRUE(waitFor([&] { return opened->isVisible(); }));
    qApp->removeEventFilter(&spy);
    EXPECT_EQ(tokenOnShow, "deferred-wayland-token");
    EXPECT_TRUE(qEnvironmentVariableIsEmpty("XDG_ACTIVATION_TOKEN"));
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

