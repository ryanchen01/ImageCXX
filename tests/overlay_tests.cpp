#include "mainwindow.h"
#include "ui/overlay_window.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QLabel>
#include <QMenu>
#include <QSlider>
#include <QSpinBox>
#include <QThread>
#include <QTreeWidget>
#include <gtest/gtest.h>
#include <functional>

using namespace imagecxx;

namespace {
bool waitForOverlay(const std::function<bool()> &condition)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 5000) {
        QApplication::processEvents();
        QThread::msleep(2);
    }
    return condition();
}

QColor overlayColor(ImageViewer *viewer, QPointF point = {1.5, 1.5})
{
    const auto frame = viewer->grab();
    const auto position = viewer->imageToWidget(point) * frame.devicePixelRatio();
    return frame.toImage().pixelColor(qRound(position.x()), qRound(position.y()));
}
}

TEST(Overlay, EditorKeepsTheCurrentImageCheckedAndEditsTheSelectedLayerFromTheRightSidebar)
{
    MainWindow window;
    auto *action = window.findChild<QAction *>("view.overlay");
    ASSERT_NE(action, nullptr);
    EXPECT_FALSE(action->isEnabled());
    ImageMetadata firstName; firstName.name = "First image";
    auto *first = window.addDocument(Image::allocate(PixelType::UInt8, {8, 8}, firstName));
    EXPECT_TRUE(action->isEnabled());
    ImageMetadata currentName; currentName.name = "Current image";
    auto *current = window.addDocument(Image::allocate(PixelType::UInt8, {8, 8}, currentName));
    window.commands().execute("view.overlay");
    auto *overlay = window.findChild<OverlayWindow *>();
    auto *editor = window.findChild<QDialog *>("overlayEditor");
    ASSERT_NE(overlay, nullptr); ASSERT_NE(editor, nullptr);
    EXPECT_TRUE(overlay->isWindow()); EXPECT_TRUE(editor->isWindow());
    EXPECT_EQ(overlay->parentWidget(), &window);
    EXPECT_EQ(editor->parentWidget(), &window);
    EXPECT_NE(overlay->window(), editor->window());
    EXPECT_EQ(QApplication::activeModalWidget(), nullptr);
    auto *images = editor->findChild<QTreeWidget *>("overlayImages");
    auto *lut = editor->findChild<QComboBox *>("overlayLut");
    auto *opacity = editor->findChild<QSpinBox *>("overlayOpacity");
    ASSERT_NE(images, nullptr); ASSERT_NE(lut, nullptr); ASSERT_NE(opacity, nullptr);
    ASSERT_EQ(images->topLevelItemCount(), 2);
    EXPECT_EQ(images->dragDropMode(), QAbstractItemView::InternalMove);
    EXPECT_GT(lut->mapTo(editor, QPoint{}).x(), images->mapTo(editor, images->rect().topRight()).x());
    auto *reference = images->topLevelItem(1);
    auto *other = images->topLevelItem(0);
    EXPECT_EQ(reference->text(0), current->title());
    EXPECT_EQ(reference->checkState(0), Qt::Checked);
    EXPECT_FALSE(reference->flags().testFlag(Qt::ItemIsUserCheckable));
    EXPECT_TRUE(reference->flags().testFlag(Qt::ItemIsDragEnabled));
    EXPECT_EQ(images->currentItem(), reference);
    reference->setCheckState(0, Qt::Unchecked);
    EXPECT_EQ(reference->checkState(0), Qt::Checked);
    lut->setCurrentText("viridis"); opacity->setValue(37);
    images->setCurrentItem(other);
    EXPECT_EQ(lut->currentText(), "grayscale"); EXPECT_EQ(opacity->value(), 50);
    lut->setCurrentText("hot"); opacity->setValue(63);
    other->setCheckState(0, Qt::Checked);
    images->setCurrentItem(reference);
    EXPECT_EQ(lut->currentText(), "viridis"); EXPECT_EQ(opacity->value(), 37);
    EXPECT_EQ(current->viewer->lut(), "grayscale"); EXPECT_EQ(first->viewer->lut(), "grayscale");
    EXPECT_FALSE(current->modified); EXPECT_FALSE(first->modified);

    editor->close();
    EXPECT_FALSE(editor->isVisible()); EXPECT_TRUE(overlay->isVisible());
    window.addDocument(Image::allocate(PixelType::UInt8, {8, 8}));
    EXPECT_EQ(images->topLevelItemCount(), 3);
    QEvent activated(QEvent::WindowActivate); QApplication::sendEvent(current, &activated);
    window.commands().execute("view.overlay");
    EXPECT_EQ(window.findChildren<OverlayWindow *>().size(), 1);
    EXPECT_EQ(window.findChild<QDialog *>("overlayEditor"), editor);
    EXPECT_TRUE(editor->isVisible());
    EXPECT_EQ(lut->currentText(), "viridis"); EXPECT_EQ(opacity->value(), 37);
    images->setCurrentItem(other);
    EXPECT_EQ(lut->currentText(), "hot"); EXPECT_EQ(opacity->value(), 63);
    EXPECT_EQ(other->checkState(0), Qt::Checked);
    overlay->close();
    EXPECT_FALSE(editor->isVisible());
    window.commands().execute("view.overlay");
    EXPECT_TRUE(overlay->isVisible()); EXPECT_TRUE(editor->isVisible());

    if (const auto capture = qEnvironmentVariable("IMAGECXX_CAPTURE_DIR"); !capture.isEmpty()) {
        QDir().mkpath(capture);
        EXPECT_TRUE(editor->grab().save(capture + "/overlay-editor.png"));
    }
    first->close();
    EXPECT_EQ(images->topLevelItemCount(), 2);
    QPointer<OverlayWindow> retired(overlay);
    QPointer<QDialog> retiredEditor(editor);
    current->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_TRUE(retired.isNull()); EXPECT_TRUE(retiredEditor.isNull());
}

TEST(Overlay, ClosingRestoresSingleViewAndStopsSynchronizingSourceImages)
{
    MainWindow window;
    auto *other = window.addDocument(Image::allocate(PixelType::UInt8, {8, 8, 6}));
    auto *current = window.addDocument(Image::allocate(PixelType::UInt8, {8, 8, 6}));
    auto *singleAction = window.findChild<QAction *>("view.single");
    auto *overlayAction = window.findChild<QAction *>("view.overlay");
    ASSERT_NE(singleAction, nullptr); ASSERT_NE(overlayAction, nullptr);
    EXPECT_TRUE(singleAction->isChecked());
    window.commands().execute("view.overlay");
    auto *overlay = window.findChild<OverlayWindow *>();
    auto *editor = window.findChild<QDialog *>("overlayEditor");
    ASSERT_NE(overlay, nullptr); ASSERT_NE(editor, nullptr);
    editor->findChild<QTreeWidget *>("overlayImages")->topLevelItem(0)->setCheckState(0, Qt::Checked);
    EXPECT_FALSE(singleAction->isChecked());
    EXPECT_TRUE(overlayAction->isChecked());
    overlay->viewer->setSlice(2);
    EXPECT_EQ(current->viewer->slice(), 2u);
    EXPECT_EQ(other->viewer->slice(), 2u);

    // Closing just the editor leaves the overlay and its navigation active.
    editor->close();
    EXPECT_TRUE(overlayAction->isChecked());
    overlay->close();
    EXPECT_TRUE(singleAction->isChecked());
    EXPECT_FALSE(overlayAction->isChecked());
    EXPECT_FALSE(current->isLinked());
    EXPECT_FALSE(current->orthogonalViews());
    EXPECT_FALSE(other->isLinked());
    EXPECT_FALSE(other->orthogonalViews());
    other->viewer->setSlice(4);
    EXPECT_EQ(current->viewer->slice(), 2u);
    current->viewer->setSlice(1);
    EXPECT_EQ(other->viewer->slice(), 4u);
    current->refresh();
    window.addDocument(Image::allocate(PixelType::UInt8, {8, 8, 6}));
    EXPECT_EQ(other->viewer->slice(), 4u);
    EXPECT_EQ(current->viewer->slice(), 1u);

    overlay->showEditor();
    EXPECT_EQ(overlay->viewer->slice(), 1u);
    EXPECT_EQ(other->viewer->slice(), 1u);
    other->viewer->setSlice(3);
    EXPECT_EQ(current->viewer->slice(), 3u);
}

TEST(Overlay, BrowsesCalibratedStacksAndFollowsSourceEdits)
{
    MainWindow window;
    ImageMetadata metadata;
    metadata.axes = {{"X", 1, 0, "mm"}, {"Y", 1, 0, "mm"}, {"Z", 1, 0, "mm"}};
    auto *other = window.addDocument(Image::allocate(PixelType::UInt8, {10, 8, 9, 2, 3}, metadata));
    metadata.axes[2].spacing = 2;
    auto *current = window.addDocument(Image::allocate(PixelType::UInt8, {10, 8, 5, 2, 3}, metadata));
    window.commands().execute("view.overlay");
    auto *overlay = window.findChild<OverlayWindow *>();
    auto *editor = window.findChild<QDialog *>("overlayEditor");
    ASSERT_NE(overlay, nullptr); ASSERT_NE(editor, nullptr);
    editor->findChild<QTreeWidget *>("overlayImages")->topLevelItem(0)->setCheckState(0, Qt::Checked);
    overlay->viewer->setSlice(3, 1, 2);
    EXPECT_EQ(current->viewer->slice(), 3u);
    EXPECT_EQ(other->viewer->slice(), 6u);
    EXPECT_EQ(other->viewer->channel(), 1u); EXPECT_EQ(other->viewer->time(), 2u);
    EXPECT_EQ(overlay->findChild<QSlider *>("sliceSlider")->value(), 3);
    other->viewer->setSlice(2, 0, 1);
    EXPECT_EQ(current->viewer->slice(), 1u); EXPECT_EQ(overlay->viewer->slice(), 1u);
    EXPECT_EQ(overlay->viewer->channel(), 0u); EXPECT_EQ(overlay->viewer->time(), 1u);
    current->replaceImage(Image::allocate(PixelType::UInt16, {12, 8, 7}, metadata), "Resize");
    EXPECT_EQ(overlay->viewer->image().width(), 12u);
    EXPECT_EQ(overlay->viewer->image().pixelType(), PixelType::UInt16);
    EXPECT_EQ(overlay->findChild<QSlider *>("sliceSlider")->maximum(), 6);
    EXPECT_EQ(window.documents().size(), 2);
    current->modified = false;
}

TEST(Overlay, GpuUpdatesLutsOpacityOrderVisibilityAndSourcePixelsLive)
{
    if (QApplication::platformName() == "offscreen" || QApplication::platformName() == "minimal")
        GTEST_SKIP() << "Qt RHI requires a graphics-capable platform plugin";
    MainWindow window;
    auto foreground = Image::allocate(PixelType::UInt8, {4, 4});
    auto background = Image::allocate(PixelType::UInt8, {4, 4});
    for (std::size_t y = 0; y < 4; ++y) for (std::size_t x = 0; x < 4; ++x) {
        foreground.setValue(std::array<std::size_t, 2>{x, y}, 20);
        background.setValue(std::array<std::size_t, 2>{x, y}, 100);
    }
    bool otherReady = false, currentReady = false;
    auto *other = window.addDocument(foreground);
    QObject::connect(other->viewer, &ImageViewer::sliceReady, &window, [&] { otherReady = true; });
    auto *current = window.addDocument(background);
    QObject::connect(current->viewer, &ImageViewer::sliceReady, &window, [&] { currentReady = true; });
    ASSERT_TRUE(waitForOverlay([&] { return otherReady && currentReady; }));
    other->viewer->setWindow(0, 100); current->viewer->setWindow(0, 100);
    window.commands().execute("view.overlay");
    auto *overlay = window.findChild<OverlayWindow *>();
    auto *editor = window.findChild<QDialog *>("overlayEditor");
    ASSERT_NE(overlay, nullptr); ASSERT_NE(editor, nullptr);
    QString error;
    QObject::connect(overlay, &OverlayWindow::viewError, [&](const QString &message) { error = message; });
    auto *images = editor->findChild<QTreeWidget *>("overlayImages");
    auto *lut = editor->findChild<QComboBox *>("overlayLut");
    auto *opacity = editor->findChild<QSpinBox *>("overlayOpacity");
    auto *reference = images->topLevelItem(1);
    auto *otherItem = images->topLevelItem(0);
    auto *opacitySlider = editor->findChild<QSlider *>("overlayOpacitySlider");
    ASSERT_NE(opacitySlider, nullptr);
    images->setCurrentItem(otherItem);
    lut->setCurrentText("hot"); otherItem->setCheckState(0, Qt::Checked);
    ASSERT_TRUE(waitForOverlay([&] {
        const auto color = overlayColor(overlay->viewer);
        return std::abs(color.red() - 204) < 4 && std::abs(color.green() - 128) < 4 && std::abs(color.blue() - 128) < 4;
    })) << error.toStdString();
    opacitySlider->setValue(100);
    ASSERT_TRUE(waitForOverlay([&] {
        const auto color = overlayColor(overlay->viewer);
        return std::abs(color.red() - 153) < 4 && color.green() < 4 && color.blue() < 4;
    }));
    auto *moved = images->takeTopLevelItem(images->indexOfTopLevelItem(reference));
    images->insertTopLevelItem(0, moved);
    ASSERT_TRUE(waitForOverlay([&] { return overlayColor(overlay->viewer).green() > 250; }));
    moved = images->takeTopLevelItem(0);
    images->addTopLevelItem(moved);
    ASSERT_TRUE(waitForOverlay([&] { return overlayColor(overlay->viewer).green() < 4; }));
    otherItem->setCheckState(0, Qt::Unchecked);
    ASSERT_TRUE(waitForOverlay([&] { return overlayColor(overlay->viewer).green() > 250; }));
    images->setCurrentItem(reference); lut->setCurrentText("viridis");
    ASSERT_TRUE(waitForOverlay([&] {
        const auto color = overlayColor(overlay->viewer);
        return std::abs(color.red() - 253) < 4 && std::abs(color.green() - 231) < 4 && std::abs(color.blue() - 37) < 4;
    }));
    opacity->setValue(0);
    ASSERT_TRUE(waitForOverlay([&] { return overlayColor(overlay->viewer) == QColor(Qt::black); }));
    otherItem->setCheckState(0, Qt::Checked);
    images->setCurrentItem(otherItem); lut->setCurrentText("grayscale");
    ASSERT_TRUE(waitForOverlay([&] { return std::abs(overlayColor(overlay->viewer).red() - 51) < 4; }));
    auto replacement = foreground.clone();
    for (std::size_t y = 0; y < 4; ++y) for (std::size_t x = 0; x < 4; ++x)
        replacement.setValue(std::array<std::size_t, 2>{x, y}, 80);
    bool ready = false;
    QObject::connect(other->viewer, &ImageViewer::sliceReady, [&] { ready = true; });
    other->replaceImage(replacement, "Edit pixels");
    ASSERT_TRUE(waitForOverlay([&] { return ready; }));
    other->viewer->setWindow(0, 100);
    ASSERT_TRUE(waitForOverlay([&] { return std::abs(overlayColor(overlay->viewer).red() - 204) < 4; }));
    EXPECT_TRUE(error.isEmpty()) << error.toStdString();
    EXPECT_EQ(current->viewer->lut(), "grayscale");
    other->modified = false;
}

TEST(Overlay, GpuAlignsCalibratedPixelsAndPreservesNativeAlpha)
{
    if (QApplication::platformName() == "offscreen" || QApplication::platformName() == "minimal")
        GTEST_SKIP() << "Qt RHI requires a graphics-capable platform plugin";
    MainWindow window;
    ImageMetadata metadata;
    metadata.axes = {{"X", 2, 1.5, "mm"}, {"Y", 2, 1.5, "mm"}};
    auto foreground = Image::allocate(PixelType::RGBA8, {2, 2}, metadata);
    foreground.setValue(std::array<std::size_t, 2>{0, 0}, 255, 0);
    foreground.setValue(std::array<std::size_t, 2>{0, 0}, 128, 3);
    auto *other = window.addDocument(foreground);
    metadata.axes = {{"X", 1, 0, "mm"}, {"Y", 1, 0, "mm"}};
    auto *current = window.addDocument(Image::allocate(PixelType::UInt8, {6, 6}, metadata));
    other->viewer->setWindow(0, 255); current->viewer->setWindow(0, 255);
    window.commands().execute("view.overlay");
    auto *overlay = window.findChild<OverlayWindow *>();
    auto *editor = window.findChild<QDialog *>("overlayEditor");
    ASSERT_NE(overlay, nullptr); ASSERT_NE(editor, nullptr);
    auto *images = editor->findChild<QTreeWidget *>("overlayImages");
    images->setCurrentItem(images->topLevelItem(0));
    editor->findChild<QSpinBox *>("overlayOpacity")->setValue(100);
    images->topLevelItem(0)->setCheckState(0, Qt::Checked);
    ASSERT_TRUE(waitForOverlay([&] {
        return std::abs(overlayColor(overlay->viewer, {1.5, 1.5}).red() - 128) < 4;
    }));
    EXPECT_EQ(overlayColor(overlay->viewer, {.5, .5}), QColor(Qt::black));
    EXPECT_EQ(overlayColor(overlay->viewer, {3.5, 3.5}), QColor(Qt::black));
    EXPECT_NEAR(overlayColor(overlay->viewer, {2.5, 2.5}).red(), 128, 3);
    EXPECT_EQ(overlayColor(overlay->viewer, {1.5, 1.5}).green(), 0);
}

TEST(Overlay, OrthogonalViewsSynchronizePlanesSourcesMenuStateAndLifecycle)
{
    MainWindow window;
    ImageMetadata metadata;
    metadata.axes = {{"X", 1, 0, "mm"}, {"Y", 1, 0, "mm"}, {"Z", 1, 0, "mm"}};
    auto *other = window.addDocument(Image::allocate(PixelType::UInt8, {12, 10, 10}, metadata));
    metadata.axes[2].spacing = 2;
    auto *current = window.addDocument(Image::allocate(PixelType::UInt8, {12, 10, 5}, metadata));
    auto *singleAction = window.findChild<QAction *>("view.single");
    auto *overlayAction = window.findChild<QAction *>("view.overlay");
    auto *orthogonalAction = window.findChild<QAction *>("view.orthogonal");
    ASSERT_NE(singleAction, nullptr);
    ASSERT_NE(overlayAction, nullptr);
    ASSERT_NE(orthogonalAction, nullptr);

    window.commands().execute("view.overlay");
    auto *overlay = window.findChild<OverlayWindow *>();
    auto *editor = window.findChild<QDialog *>("overlayEditor");
    ASSERT_NE(overlay, nullptr); ASSERT_NE(editor, nullptr);
    editor->findChild<QTreeWidget *>("overlayImages")->topLevelItem(0)->setCheckState(0, Qt::Checked);
    EXPECT_TRUE(overlayAction->isChecked());
    EXPECT_FALSE(orthogonalAction->isChecked());
    EXPECT_TRUE(orthogonalAction->isEnabled());

    // Open orthogonal views for the active overlay view.
    orthogonalAction->trigger();
    EXPECT_TRUE(overlay->orthogonalViews());
    EXPECT_FALSE(current->orthogonalViews());
    ASSERT_EQ(overlay->views().size(), 3);
    EXPECT_TRUE(overlayAction->isChecked());
    EXPECT_TRUE(orthogonalAction->isChecked());
    EXPECT_FALSE(singleAction->isChecked());

    const std::array<QString, 3> axes{"Z", "Y", "X"};
    const std::array<int, 3> maxima{4, 9, 11};
    for (int i = 0; i < 3; ++i) {
        auto *viewWindow = overlay->views()[i]->window();
        auto *slider = viewWindow->findChild<QSlider *>("sliceSlider", Qt::FindDirectChildrenOnly);
        ASSERT_NE(slider, nullptr);
        EXPECT_EQ(slider->maximum(), maxima[i]);
        EXPECT_EQ(slider->accessibleName(), axes[i] + " slice");
        EXPECT_EQ(viewWindow->findChild<QLabel *>("sliceAxisLabel", Qt::FindDirectChildrenOnly)->text(), axes[i]);
    }

    // Navigating in overlay XZ updates all three overlay planes and both source documents.
    overlay->views()[1]->positionPicked({4.5, 3.5});
    EXPECT_EQ(overlay->spatialPosition(), (std::array<std::size_t, 3>{4, 5, 3}));
    EXPECT_EQ(overlay->views()[0]->slice(), 3u);
    EXPECT_EQ(overlay->views()[1]->slice(), 5u);
    EXPECT_EQ(overlay->views()[2]->slice(), 4u);
    EXPECT_EQ(current->spatialPosition(), (std::array<std::size_t, 3>{4, 5, 3}));
    EXPECT_EQ(other->spatialPosition(), (std::array<std::size_t, 3>{4, 5, 6}));

    // Zooming overlay XZ synchronizes physical scale across all three overlay views, and a live reference edit with changed Z spacing keeps manual XZ/YZ transforms.
    overlay->views()[1]->setViewTransform(3.0, {5.5, 1.25});
    EXPECT_DOUBLE_EQ(overlay->views()[0]->zoom(), 3.0);
    EXPECT_DOUBLE_EQ(overlay->views()[2]->zoom(), 3.0);
    auto editedMeta = metadata;
    editedMeta.axes[2].spacing = 3;
    current->replaceImage(Image::allocate(PixelType::UInt8, {12, 10, 5}, editedMeta), "Live edit");
    EXPECT_DOUBLE_EQ(overlay->views()[1]->zoom(), 3.0);
    EXPECT_NEAR(overlay->views()[1]->viewCenter().y(), 1.25, 1e-12);
    overlay->views()[0]->setViewTransform(3.0, {6.5, 4.5});
    EXPECT_NEAR(overlay->views()[1]->viewCenter().y(), 1.25, 1e-12);
    EXPECT_NEAR(overlay->views()[2]->viewCenter().y(), 1.25, 1e-12);
    current->modified = false;

    // Activating the source document switches menu state back to the document; activating overlay XZ restores overlay state.
    QEvent activated(QEvent::WindowActivate);
    QCoreApplication::sendEvent(current, &activated);
    EXPECT_FALSE(overlayAction->isChecked());
    EXPECT_FALSE(orthogonalAction->isChecked());
    EXPECT_TRUE(singleAction->isChecked());
    QCoreApplication::sendEvent(overlay->views()[1]->window(), &activated);
    EXPECT_TRUE(overlayAction->isChecked());
    EXPECT_TRUE(orthogonalAction->isChecked());

    // Closing one orthogonal pane lets Orthogonal views re-show it; triggering again when all are visible toggles orthogonal off.
    auto *xzWindow = overlay->views()[1]->window();
    xzWindow->close();
    EXPECT_FALSE(xzWindow->isVisible());
    orthogonalAction->trigger();
    EXPECT_TRUE(overlay->orthogonalViews());
    EXPECT_TRUE(xzWindow->isVisible());
    orthogonalAction->trigger();
    EXPECT_FALSE(overlay->orthogonalViews());
    EXPECT_EQ(overlay->views().size(), 1);
    EXPECT_TRUE(overlay->isVisible());
    EXPECT_TRUE(overlayAction->isChecked());
    EXPECT_FALSE(orthogonalAction->isChecked());

    // Re-enable orthogonal views, then verify closing/reopening via Window menu and flattening to 2D.
    orthogonalAction->trigger();
    ASSERT_EQ(overlay->views().size(), 3);
    QPointer<QWidget> overlayXz(overlay->views()[1]->window());
    overlay->close();
    EXPECT_FALSE(overlayXz->isVisible());
    auto *windowMenu = window.findChild<QMenu *>("windowMenu");
    ASSERT_NE(windowMenu, nullptr);
    QMetaObject::invokeMethod(windowMenu, "aboutToShow");
    QAction *xzMenuAction = nullptr;
    for (auto *action : windowMenu->actions())
        if (action->text() == overlayXz->windowTitle()) xzMenuAction = action;
    ASSERT_NE(xzMenuAction, nullptr);
    xzMenuAction->trigger();
    EXPECT_TRUE(overlay->isVisible());
    EXPECT_TRUE(overlayXz->isVisible());
    EXPECT_TRUE(orthogonalAction->isChecked());

    current->replaceImage(Image::allocate(PixelType::UInt8, {12, 10}), "Flatten");
    EXPECT_FALSE(overlay->orthogonalViews());
    EXPECT_EQ(overlay->views().size(), 1);
    EXPECT_TRUE(overlayXz.isNull());
    EXPECT_FALSE(orthogonalAction->isEnabled());
    current->modified = false;

    // Linked overlay layers with coarser spacing do not bounce rounded Z slices back into the reference or peer layers.
    ImageMetadata fineMeta, coarseMeta;
    fineMeta.axes = {{"X", 1, 0, "mm"}, {"Y", 1, 0, "mm"}, {"Z", 1, 0, "mm"}};
    coarseMeta.axes = {{"X", 1, 0, "mm"}, {"Y", 1, 0, "mm"}, {"Z", 2, 0, "mm"}};
    auto *coarse = window.addDocument(Image::allocate(PixelType::UInt8, {8, 8, 5}, coarseMeta));
    auto *peer = window.addDocument(Image::allocate(PixelType::UInt8, {8, 8, 10}, fineMeta));
    auto *fine = window.addDocument(Image::allocate(PixelType::UInt8, {8, 8, 10}, fineMeta));
    ASSERT_TRUE(waitForOverlay([&] { return coarse->isVisible() && peer->isVisible() && fine->isVisible(); }));
    coarse->linkTo({peer});
    QCoreApplication::sendEvent(fine, &activated);
    window.commands().execute("view.overlay");
    OverlayWindow *linkedOverlay = nullptr;
    for (auto *candidate : window.findChildren<OverlayWindow *>(QString{}, Qt::FindDirectChildrenOnly))
        if (candidate->document() == fine) linkedOverlay = candidate;
    ASSERT_NE(linkedOverlay, nullptr);
    auto *linkedEditor = window.findChildren<QDialog *>("overlayEditor").back();
    auto *linkedImages = linkedEditor->findChild<QTreeWidget *>("overlayImages");
    for (int row = 0; row < linkedImages->topLevelItemCount(); ++row)
        linkedImages->topLevelItem(row)->setCheckState(0, Qt::Checked);
    linkedOverlay->viewer->setSlice(1);
    EXPECT_EQ(fine->viewer->slice(), 1u);
    EXPECT_EQ(coarse->viewer->slice(), 1u);
    EXPECT_EQ(peer->viewer->slice(), 1u);

    // Unchecking peer still allows checked coarse to drive its unchecked linked peer.
    for (int row = 0; row < linkedImages->topLevelItemCount(); ++row)
        if (linkedImages->topLevelItem(row)->data(0, Qt::UserRole).value<DocumentWidget *>() == peer)
            linkedImages->topLevelItem(row)->setCheckState(0, Qt::Unchecked);
    linkedOverlay->viewer->setSlice(4);
    EXPECT_EQ(fine->viewer->slice(), 4u);
    EXPECT_EQ(coarse->viewer->slice(), 2u);
    EXPECT_EQ(peer->viewer->slice(), 4u);
}

TEST(Overlay, GpuOrthogonalViewsCompositeCalibratedLayersAndLiveEdits)
{
    if (QApplication::platformName() == "offscreen" || QApplication::platformName() == "minimal")
        GTEST_SKIP() << "Qt RHI requires a graphics-capable platform plugin";
    MainWindow window;
    ImageMetadata metadata;
    metadata.axes = {{"X", 1, 0, "mm"}, {"Y", 1, 0, "mm"}, {"Z", 1, 0, "mm"}};
    auto background = Image::allocate(PixelType::UInt8, {6, 6, 6}, metadata);
    auto foreground = Image::allocate(PixelType::UInt8, {6, 6, 6}, metadata);
    for (std::size_t z = 0; z < 6; ++z)
        for (std::size_t y = 0; y < 6; ++y)
            for (std::size_t x = 0; x < 6; ++x) {
                background.setValue(std::array<std::size_t, 3>{x, y, z}, 100);
                foreground.setValue(std::array<std::size_t, 3>{x, y, z}, z >= 3 ? 20 : 80);
            }
    bool otherReady = false, currentReady = false;
    auto *other = window.addDocument(foreground);
    QObject::connect(other->viewer, &ImageViewer::sliceReady, &window, [&] { otherReady = true; });
    auto *current = window.addDocument(background);
    QObject::connect(current->viewer, &ImageViewer::sliceReady, &window, [&] { currentReady = true; });
    ASSERT_TRUE(waitForOverlay([&] { return otherReady && currentReady; }));
    other->viewer->setWindow(0, 100);
    current->viewer->setWindow(0, 100);

    window.commands().execute("view.overlay");
    auto *overlay = window.findChild<OverlayWindow *>();
    auto *editor = window.findChild<QDialog *>("overlayEditor");
    ASSERT_NE(overlay, nullptr); ASSERT_NE(editor, nullptr);
    auto *images = editor->findChild<QTreeWidget *>("overlayImages");
    auto *lut = editor->findChild<QComboBox *>("overlayLut");
    auto *otherItem = images->topLevelItem(0);
    images->setCurrentItem(otherItem);
    lut->setCurrentText("hot");
    otherItem->setCheckState(0, Qt::Checked);

    window.commands().execute("view.orthogonal");
    ASSERT_EQ(overlay->views().size(), 3);
    for (auto *view : overlay->views()) {
        view->setNavigationCrosshair({});
        view->setViewLabel({});
    }
    // In XZ (x, z) and YZ (y, z): z=4 has foreground=20 (hot: R=0.6,G=0,B=0) over background=100 (white) at 50% -> (204, 128, 128).
    // z=1 has foreground=80 (hot: R=1,G=1,B=0.4) over background=100 (white) at 50% -> (255, 255, 179).
    ASSERT_TRUE(waitForOverlay([&] {
        const auto xzLow = overlayColor(overlay->views()[1], {2.5, 4.5});
        const auto xzHigh = overlayColor(overlay->views()[1], {2.5, 1.5});
        const auto yzLow = overlayColor(overlay->views()[2], {2.5, 4.5});
        return std::abs(xzLow.red() - 204) < 5 && std::abs(xzLow.green() - 128) < 5 && std::abs(xzLow.blue() - 128) < 5
            && std::abs(xzHigh.red() - 255) < 5 && std::abs(xzHigh.green() - 255) < 5 && std::abs(xzHigh.blue() - 179) < 5
            && std::abs(yzLow.red() - 204) < 5 && std::abs(yzLow.green() - 128) < 5 && std::abs(yzLow.blue() - 128) < 5;
    }));

    // Live edits to source pixels and display window propagate to overlay XZ and YZ.
    auto replacement = foreground.clone();
    for (std::size_t z = 0; z < 6; ++z)
        for (std::size_t y = 0; y < 6; ++y)
            for (std::size_t x = 0; x < 6; ++x)
                replacement.setValue(std::array<std::size_t, 3>{x, y, z}, 20);
    other->replaceImage(replacement, "Edit 3D pixels");
    other->viewer->setWindow(0, 100);
    ASSERT_TRUE(waitForOverlay([&] {
        const auto xzUpdated = overlayColor(overlay->views()[1], {2.5, 1.5});
        const auto yzUpdated = overlayColor(overlay->views()[2], {2.5, 1.5});
        return std::abs(xzUpdated.red() - 204) < 5 && std::abs(xzUpdated.green() - 128) < 5 && std::abs(xzUpdated.blue() - 128) < 5
            && std::abs(yzUpdated.red() - 204) < 5 && std::abs(yzUpdated.green() - 128) < 5 && std::abs(yzUpdated.blue() - 128) < 5;
    }));

    // Orthogonal XY and XZ skip a layer whose calibrated extent does not cover the selected plane.
    auto partialMeta = metadata;
    partialMeta.axes[2].origin = 4;
    auto partial = Image::allocate(PixelType::UInt8, {6, 2, 2}, partialMeta);
    for (std::size_t z = 0; z < 2; ++z)
        for (std::size_t y = 0; y < 2; ++y)
            for (std::size_t x = 0; x < 6; ++x)
                partial.setValue(std::array<std::size_t, 3>{x, y, z}, 20);
    other->replaceImage(partial, "Shrink Y and Z extent");
    other->viewer->setWindow(0, 100);
    overlay->setPosition(2, 4, 0);
    for (auto *view : overlay->views()) { view->setNavigationCrosshair({}); view->setViewLabel({}); }
    ASSERT_TRUE(waitForOverlay([&] {
        const auto xyOutside = overlayColor(overlay->views()[0], {2.5, 0.5});
        const auto xzOutside = overlayColor(overlay->views()[1], {2.5, 4.5});
        return xyOutside.red() > 250 && xyOutside.green() > 250 && xyOutside.blue() > 250
            && xzOutside.red() > 250 && xzOutside.green() > 250 && xzOutside.blue() > 250;
    }));
    other->modified = false;
}
