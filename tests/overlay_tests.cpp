#include "mainwindow.h"
#include "ui/overlay_window.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QLabel>
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
    auto *other = window.addDocument(foreground);
    auto *current = window.addDocument(background);
    ASSERT_TRUE(waitForOverlay([&] { return other->viewer->decodedCacheBytes() && current->viewer->decodedCacheBytes(); }));
    // Allow the decoded frames to reach the GUI before setting manual windows.
    QApplication::processEvents();
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
