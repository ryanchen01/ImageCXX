#include "viewer/image_viewer.h"
#include "ui/document_widget.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QThread>
#include <QWheelEvent>
#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <cstring>
#include <cmath>
#include <numbers>
#include <functional>
#include <optional>
#include <stdexcept>

using namespace imagecxx;

namespace {

bool waitFor(const std::function<bool()> &condition)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 5000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    return condition();
}

QColor displayColor(ImageViewer &viewer, QPointF imagePoint)
{
    const auto pixmap = viewer.grab();
    const auto position = viewer.imageToWidget(imagePoint) * pixmap.devicePixelRatio();
    return pixmap.toImage().pixelColor(qRound(position.x()), qRound(position.y()));
}

void roiMouse(ImageViewer &viewer, QEvent::Type type, QPointF point, Qt::KeyboardModifiers modifiers = {})
{
    const auto position = viewer.imageToWidget(point);
    QMouseEvent event(type, position, position, type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
        type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, modifiers);
    QApplication::sendEvent(&viewer, &event);
}

void roiClick(ImageViewer &viewer, QPointF point)
{
    roiMouse(viewer, QEvent::MouseButtonPress, point);
    roiMouse(viewer, QEvent::MouseButtonRelease, point);
}

void roiDrag(ImageViewer &viewer, QPointF begin, QPointF end, Qt::KeyboardModifiers modifiers = {})
{
    roiMouse(viewer, QEvent::MouseButtonPress, begin, modifiers);
    roiMouse(viewer, QEvent::MouseMove, end, modifiers);
    roiMouse(viewer, QEvent::MouseButtonRelease, end, modifiers);
}

void roiKey(ImageViewer &viewer, Qt::Key key, QEvent::Type type = QEvent::KeyPress)
{
    QKeyEvent event(type, key, {});
    QApplication::sendEvent(&viewer, &event);
}

// Forces the viewer through the lazy read path and makes cancellation observable.
class SlowSlices final : public Storage {
public:
    std::size_t size() const override { return 16 * 16 * 6 * sizeof(std::uint16_t); }
    const std::byte *data() const override { return nullptr; }
    std::byte *mutableData() override { return nullptr; }
    bool readOnly() const override { return true; }
    QString kind() const override { return QStringLiteral("Test lazy slices"); }
    void read(std::size_t offset, std::span<std::byte> destination) const override
    {
        ++reads;
        QThread::usleep(50);
        const auto slice = offset / (16 * 16 * sizeof(std::uint16_t));
        const std::uint16_t value = std::uint16_t(slice * 100 + offset / sizeof(std::uint16_t) % 256);
        if (destination.size() != sizeof(value)) throw std::runtime_error("Unexpected sample size");
        std::memcpy(destination.data(), &value, sizeof(value));
    }
    mutable std::atomic<int> reads{0};
};

} // namespace

TEST(Viewer, ImageJNavigationUsesChannelsSlicesFramesAndZoom)
{
    ImageViewer viewer; viewer.setImage(Image::allocate(PixelType::UInt8, {16, 16, 3, 2, 4}));
    const auto key = [&](Qt::Key code, Qt::KeyboardModifiers modifiers = {}) {
        QKeyEvent event(QEvent::KeyPress, code, modifiers); QApplication::sendEvent(&viewer, &event);
    };
    key(Qt::Key_Right); EXPECT_EQ(viewer.channel(), 1u); EXPECT_EQ(viewer.slice(), 0u);
    key(Qt::Key_Greater, Qt::ControlModifier | Qt::ShiftModifier); EXPECT_EQ(viewer.slice(), 1u);
    key(Qt::Key_Right, Qt::AltModifier); EXPECT_EQ(viewer.time(), 1u);
    key(Qt::Key_Left, Qt::AltModifier); EXPECT_EQ(viewer.time(), 0u);
    key(Qt::Key_Left, Qt::AltModifier); EXPECT_EQ(viewer.time(), 0u);
    const auto zoom = viewer.zoom(); key(Qt::Key_Up); EXPECT_GT(viewer.zoom(), zoom);
    key(Qt::Key_Down); EXPECT_DOUBLE_EQ(viewer.zoom(), zoom);
    key(Qt::Key_5); EXPECT_DOUBLE_EQ(viewer.zoom(), 1);
    viewer.setImage(Image::allocate(PixelType::UInt8, {16, 16, 20}));
    key(Qt::Key_Right, Qt::AltModifier); EXPECT_EQ(viewer.slice(), 10u);
    key(Qt::Key_Right, Qt::AltModifier); EXPECT_EQ(viewer.slice(), 19u);
    key(Qt::Key_Left); EXPECT_EQ(viewer.slice(), 18u);
}

TEST(Viewer, MapsCoordinatesAndRejectsInvalidDisplayValues)
{
    ImageViewer viewer;
    viewer.resize(640, 480);
    viewer.setImage(Image::allocate(PixelType::UInt16, {64, 48}));
    viewer.setZoom(3.25);
    const QPointF pixel(17.5, 29.25);
    const QPointF roundTrip = viewer.widgetToImage(viewer.imageToWidget(pixel));
    EXPECT_NEAR(roundTrip.x(), pixel.x(), 1e-12);
    EXPECT_NEAR(roundTrip.y(), pixel.y(), 1e-12);
    viewer.setWindow(-10, 300);
    viewer.setWindow(100, 100);
    EXPECT_EQ(viewer.windowMinimum(), -10);
    EXPECT_EQ(viewer.windowMaximum(), 300);
    viewer.setGamma(1.5);
    viewer.setGamma(0);
    EXPECT_EQ(viewer.gamma(), 1.5);
    viewer.setLut("VIRIDIS");
    EXPECT_EQ(viewer.lut(), "viridis");
    viewer.setLinearInterpolation(true);
    EXPECT_TRUE(viewer.linearInterpolation());
}

TEST(Viewer, UnchangedManualWindowDisablesAutoRangeWithoutNotifying)
{
    ImageViewer viewer;
    auto image = Image::allocate(PixelType::UInt16, {2, 2, 2});
    auto *pixels = reinterpret_cast<std::uint16_t *>(image.mutableData());
    const std::array<std::uint16_t, 8> values{0, 10, 20, 30, 100, 110, 120, 130};
    std::copy(values.begin(), values.end(), pixels);
    int ready = 0, changes = 0;
    QObject::connect(&viewer, &ImageViewer::sliceReady, [&] { ++ready; });
    viewer.setImage(image);
    ASSERT_TRUE(waitFor([&] { return ready == 1; }));
    QObject::connect(&viewer, &ImageViewer::displayRangeChanged, [&](double, double) { ++changes; });
    viewer.setWindow(0, 30);
    viewer.setWindow(0, 30);
    EXPECT_EQ(changes, 0);
    viewer.setSlice(1);
    ASSERT_TRUE(waitFor([&] { return ready == 2; }));
    EXPECT_EQ(changes, 0);
    EXPECT_DOUBLE_EQ(viewer.windowMinimum(), 0);
    EXPECT_DOUBLE_EQ(viewer.windowMaximum(), 30);
    viewer.setWindow(5, 25);
    viewer.setWindow(5, 25);
    EXPECT_EQ(changes, 1);
}

TEST(Viewer, ScrollingPreservesInitialAndExplicitAutoWindow)
{
    ImageViewer viewer;
    auto image = Image::allocate(PixelType::UInt16, {2, 2, 2});
    const std::array<std::uint16_t, 8> values{0, 10, 20, 30, 100, 110, 120, 130};
    std::copy(values.begin(), values.end(), reinterpret_cast<std::uint16_t *>(image.mutableData()));
    int ready = 0, changes = 0;
    QObject::connect(&viewer, &ImageViewer::sliceReady, [&] { ++ready; });
    viewer.setImage(image);
    ASSERT_TRUE(waitFor([&] { return ready == 1; }));
    EXPECT_DOUBLE_EQ(viewer.windowMinimum(), 0);
    EXPECT_DOUBLE_EQ(viewer.windowMaximum(), 30);
    QObject::connect(&viewer, &ImageViewer::displayRangeChanged, [&](double, double) { ++changes; });

    QWheelEvent forward(QPointF(10, 10), QPointF(10, 10), QPoint(), QPoint(0, -120),
        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&viewer, &forward);
    ASSERT_TRUE(waitFor([&] { return ready == 2; }));
    EXPECT_EQ(viewer.slice(), 1u);
    EXPECT_DOUBLE_EQ(viewer.sliceRange().first, 100);
    EXPECT_DOUBLE_EQ(viewer.sliceRange().second, 130);
    EXPECT_DOUBLE_EQ(viewer.windowMinimum(), 0);
    EXPECT_DOUBLE_EQ(viewer.windowMaximum(), 30);
    EXPECT_EQ(changes, 0);

    viewer.autoWindow();
    EXPECT_DOUBLE_EQ(viewer.windowMinimum(), 100);
    EXPECT_DOUBLE_EQ(viewer.windowMaximum(), 130);
    EXPECT_EQ(changes, 1);
    QWheelEvent backward(QPointF(10, 10), QPointF(10, 10), QPoint(), QPoint(0, 120),
        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&viewer, &backward);
    ASSERT_TRUE(waitFor([&] { return ready == 3; }));
    EXPECT_EQ(viewer.slice(), 0u);
    EXPECT_DOUBLE_EQ(viewer.windowMinimum(), 100);
    EXPECT_DOUBLE_EQ(viewer.windowMaximum(), 130);
    EXPECT_EQ(changes, 1);

    viewer.setImage(image);
    ASSERT_TRUE(waitFor([&] { return ready == 4; }));
    EXPECT_DOUBLE_EQ(viewer.windowMinimum(), 0);
    EXPECT_DOUBLE_EQ(viewer.windowMaximum(), 30);
}

TEST(Viewer, AutoWindowDuringLoadingAppliesOnceToRequestedSlice)
{
    ImageViewer viewer;
    auto image = Image::allocate(PixelType::UInt16, {2, 2, 2});
    const std::array<std::uint16_t, 8> values{0, 10, 20, 30, 100, 110, 120, 130};
    std::copy(values.begin(), values.end(), reinterpret_cast<std::uint16_t *>(image.mutableData()));
    int ready = 0;
    QObject::connect(&viewer, &ImageViewer::sliceReady, [&] { ++ready; });
    viewer.setImage(image);
    viewer.setWindow(5, 25);
    ASSERT_TRUE(waitFor([&] { return ready == 1; }));
    EXPECT_DOUBLE_EQ(viewer.windowMinimum(), 5);
    EXPECT_DOUBLE_EQ(viewer.windowMaximum(), 25);

    viewer.setSlice(1);
    viewer.autoWindow();
    ASSERT_TRUE(waitFor([&] { return ready == 2; }));
    EXPECT_DOUBLE_EQ(viewer.windowMinimum(), 100);
    EXPECT_DOUBLE_EQ(viewer.windowMaximum(), 130);
    viewer.setSlice(0);
    ASSERT_TRUE(waitFor([&] { return ready == 3; }));
    EXPECT_DOUBLE_EQ(viewer.windowMinimum(), 100);
    EXPECT_DOUBLE_EQ(viewer.windowMaximum(), 130);
}

TEST(Viewer, InitializesDicomPhotometricInversionWithoutChangingSamples)
{
    ImageMetadata metadata;
    metadata.sourcePath = QStringLiteral("monochrome1.dcm");
    metadata.attributes.insert(QStringLiteral("DICOM.0028,0004"), QStringLiteral("MONOCHROME1"));
    auto image = Image::allocate(PixelType::UInt16, {4, 4, 2}, metadata);
    const std::array<std::size_t, 3> index{1, 2, 0};
    image.setValue(index, 1234);
    ImageViewer viewer;
    viewer.setImage(image);
    EXPECT_TRUE(viewer.isInverted());
    EXPECT_DOUBLE_EQ(viewer.image().value(index), 1234);
    EXPECT_DOUBLE_EQ(image.value(index), 1234);

    viewer.setInverted(false);
    viewer.setSlice(1);
    EXPECT_FALSE(viewer.isInverted());
    auto processed = image.clone();
    processed.metadata().sourcePath.clear();
    viewer.setImage(processed, true);
    EXPECT_FALSE(viewer.isInverted());
    // Undo/reload in the same document also keeps the user's display choice.
    viewer.setImage(image, true);
    EXPECT_FALSE(viewer.isInverted());
    EXPECT_DOUBLE_EQ(viewer.image().value(index), 1234);

    viewer.setImage(image);
    EXPECT_TRUE(viewer.isInverted());
    metadata.sourcePath = QStringLiteral("monochrome2.dcm");
    metadata.attributes.insert(QStringLiteral("DICOM.0028,0004"), QStringLiteral("MONOCHROME2"));
    viewer.setImage(Image::allocate(PixelType::UInt16, {4, 4}, metadata));
    EXPECT_FALSE(viewer.isInverted());
    viewer.setInverted(true);
    viewer.setImage(Image::allocate(PixelType::UInt8, {4, 4}));
    EXPECT_FALSE(viewer.isInverted());
}

TEST(Viewer, PublishesNewestLazySliceAndBoundsDecodedCache)
{
    auto storage = std::make_shared<SlowSlices>();
    ImageViewer viewer;
    constexpr std::size_t oneSlice = 16 * 16 * sizeof(float);
    viewer.setCacheBudgets(oneSlice * 2, oneSlice * 2);
    bool loaded = false;
    QString error;
    QObject::connect(&viewer, &ImageViewer::loadingChanged, [&loaded](bool busy) { loaded = !busy; });
    QObject::connect(&viewer, &ImageViewer::loadFailed, [&error](const QString &message) { error = message; });
    viewer.setImage(Image::fromStorage(PixelType::UInt16, {16, 16, 6}, storage));
    ASSERT_TRUE(waitFor([&storage] { return storage->reads.load() > 20; }));
    viewer.setSlice(1);
    viewer.setSlice(2);
    viewer.setSlice(5);
    ASSERT_TRUE(waitFor([&loaded, &error] { return loaded || !error.isEmpty(); }));
    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    EXPECT_EQ(viewer.slice(), 5);
    EXPECT_DOUBLE_EQ(viewer.windowMinimum(), 500);
    EXPECT_DOUBLE_EQ(viewer.windowMaximum(), 755);
    EXPECT_LE(viewer.decodedCacheBytes(), oneSlice * 2);
    // A complete traversal would read 1536 samples before adjacent prefetching.
    EXPECT_LT(storage->reads.load(), 1000);
}

TEST(Viewer, OversizedDecodedSliceReportsFailure)
{
    ImageViewer viewer;
    viewer.setCacheBudgets(32, 32);
    QString error;
    QObject::connect(&viewer, &ImageViewer::loadFailed, [&error](const QString &message) { error = message; });
    viewer.setImage(Image::allocate(PixelType::Float64, {32, 32}));
    ASSERT_TRUE(waitFor([&error] { return !error.isEmpty(); }));
    EXPECT_TRUE(error.contains("memory limit"));
    EXPECT_EQ(viewer.decodedCacheBytes(), 0);
}

TEST(Viewer, DragCreatesRoiInOriginalImageCoordinates)
{
    ImageViewer viewer;
    viewer.resize(640, 480);
    viewer.setImage(Image::allocate(PixelType::UInt8, {64, 48, 3}));
    viewer.setSlice(2);
    viewer.setZoom(4);
    viewer.setTool(ImageViewer::Tool::Rectangle);
    std::optional<Roi> roi;
    QObject::connect(&viewer, &ImageViewer::roiCreated, [&roi](const Roi &created) { roi = created; });
    const QPointF begin = viewer.imageToWidget({4, 5});
    const QPointF end = viewer.imageToWidget({20, 24});
    QMouseEvent press(QEvent::MouseButtonPress, begin, begin, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&viewer, &press);
    QMouseEvent move(QEvent::MouseMove, end, end, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&viewer, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, end, end, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&viewer, &release);
    ASSERT_TRUE(roi.has_value());
    EXPECT_EQ(roi->type, RoiType::Rectangle);
    EXPECT_EQ(roi->z, 2);
    ASSERT_EQ(roi->points.size(), 2);
    EXPECT_EQ(roi->points[0], QPointF(4, 5));
    EXPECT_EQ(roi->points[1], QPointF(20, 24));
}

TEST(Viewer, RoiClicksAndEscapeExitEditingBeforeClearingSelection)
{
    DocumentWidget document(Image::allocate(PixelType::UInt8, {100, 100}));
    auto &viewer = *document.viewer;
    viewer.resize(400, 400); viewer.setZoom(4); viewer.setTool(ImageViewer::Tool::Rectangle);
    roiDrag(viewer, {10, 10}, {40, 40});
    ASSERT_TRUE(document.selection);
    EXPECT_EQ(document.selection->color, 0xffffd573u);
    const auto id = document.selection->id;
    roiClick(viewer, {20, 20});
    roiClick(viewer, {80, 80});
    ASSERT_TRUE(document.selection); EXPECT_EQ(document.selection->id, id);
    roiClick(viewer, {80, 80});
    EXPECT_FALSE(document.selection); EXPECT_TRUE(document.selectionCleared);
    ASSERT_TRUE(document.previousSelection);
    document.setSelection(document.previousSelection);
    roiClick(viewer, {10, 25}); // The outline also enters edit mode.
    roiKey(viewer, Qt::Key_Escape);
    ASSERT_TRUE(document.selection);
    roiKey(viewer, Qt::Key_Escape);
    EXPECT_FALSE(document.selection);
    // A freshly drawn ROI starts outside edit mode.
    roiDrag(viewer, {10, 10}, {40, 40});
    ASSERT_TRUE(document.selection);
    roiKey(viewer, Qt::Key_Escape);
    EXPECT_FALSE(document.selection);
}

TEST(Viewer, RoiHandlesResizeMoveAndUpdateManagedRois)
{
    DocumentWidget document(Image::allocate(PixelType::UInt8, {100, 100}));
    auto &viewer = *document.viewer;
    viewer.resize(400, 400); viewer.setZoom(4); viewer.setTool(ImageViewer::Tool::Rectangle);
    roiDrag(viewer, {10, 10}, {40, 30});
    ASSERT_TRUE(document.selection);
    document.rois.push_back(*document.selection); document.refreshRois();
    const auto id = document.selection->id;
    roiClick(viewer, {20, 20});
    roiDrag(viewer, {40, 30}, {60, 45});
    ASSERT_TRUE(document.selection);
    EXPECT_EQ(document.selection->bounds(), QRectF(10, 10, 50, 35));
    EXPECT_EQ(document.selection->id, id); EXPECT_EQ(document.rois.front().bounds(), document.selection->bounds());
    EXPECT_FALSE(document.modified);
    EXPECT_FALSE(document.title().endsWith(" *")); EXPECT_EQ(document.revision, 0u);
    roiDrag(viewer, {35, 10}, {35, 5}); // Top edge handle.
    EXPECT_EQ(document.selection->bounds(), QRectF(10, 5, 50, 40));
    roiDrag(viewer, {30, 20}, {40, 30});
    EXPECT_EQ(document.selection->bounds(), QRectF(20, 15, 50, 40));
    roiKey(viewer, Qt::Key_Escape); roiKey(viewer, Qt::Key_Escape);
    EXPECT_FALSE(document.selection); ASSERT_EQ(document.rois.size(), 1u);
    EXPECT_EQ(document.rois.front().bounds(), QRectF(20, 15, 50, 40));
}

TEST(Viewer, ShiftKeepsDrawnAndResizedRectanglesSquareAndEllipsesCircular)
{
    for (const auto tool : {ImageViewer::Tool::Rectangle, ImageViewer::Tool::Ellipse}) {
        DocumentWidget document(Image::allocate(PixelType::UInt8, {100, 100}));
        auto &viewer = *document.viewer;
        viewer.resize(400, 400); viewer.setZoom(4); viewer.setTool(tool);
        roiDrag(viewer, {20, 20}, {50, 40}, Qt::ShiftModifier);
        ASSERT_TRUE(document.selection);
        EXPECT_EQ(document.selection->bounds(), QRectF(20, 20, 30, 30));
        roiClick(viewer, {35, 35});
        roiDrag(viewer, {50, 50}, {70, 60}, Qt::ShiftModifier);
        EXPECT_EQ(document.selection->bounds(), QRectF(20, 20, 50, 50));
        roiDrag(viewer, {70, 45}, {80, 45}, Qt::ShiftModifier); // Right edge handle.
        EXPECT_EQ(document.selection->bounds(), QRectF(20, 15, 60, 60));
        roiDrag(viewer, {80, 75}, {110, 95}, Qt::ShiftModifier);
        const auto bounds = document.selection->bounds();
        EXPECT_DOUBLE_EQ(bounds.width(), bounds.height());
        EXPECT_LE(bounds.right(), 100); EXPECT_LE(bounds.bottom(), 100);
    }
}

TEST(Viewer, ShiftLineDrawingAndEndpointEditingSnapInAllEightDirections)
{
    for (int direction = 0; direction < 8; ++direction) {
        DocumentWidget document(Image::allocate(PixelType::UInt8, {100, 100}));
        auto &viewer = *document.viewer;
        viewer.resize(400, 400); viewer.setZoom(4); viewer.setTool(ImageViewer::Tool::Line);
        const double angle = direction * std::numbers::pi / 4;
        const QPointF anchor(50, 50);
        const QPointF end = anchor + QPointF(20 * std::cos(angle + .15), 20 * std::sin(angle + .15));
        roiDrag(viewer, anchor, end, Qt::ShiftModifier);
        ASSERT_TRUE(document.selection);
        auto delta = document.selection->points[1] - anchor;
        EXPECT_NEAR(delta.x(), 20 * std::cos(angle), 1e-10);
        EXPECT_NEAR(delta.y(), 20 * std::sin(angle), 1e-10);
        const auto tip = document.selection->points[1];
        roiClick(viewer, (anchor + tip) / 2 + QPointF(0, .75)); // Hit tolerance is in screen pixels.
        roiDrag(viewer, tip, anchor + QPointF(30 * std::cos(angle - .1), 30 * std::sin(angle - .1)), Qt::ShiftModifier);
        delta = document.selection->points[1] - anchor;
        EXPECT_NEAR(delta.x(), 30 * std::cos(angle), 1e-10);
        EXPECT_NEAR(delta.y(), 30 * std::sin(angle), 1e-10);
    }
}

TEST(Viewer, ShiftLinesAtImageEdgesKeepHorizontalAndVerticalDirections)
{
    ImageViewer viewer;
    viewer.resize(400, 400); viewer.setImage(Image::allocate(PixelType::UInt8, {100, 100}));
    viewer.setZoom(4); viewer.setTool(ImageViewer::Tool::Line);
    std::optional<Roi> created;
    QObject::connect(&viewer, &ImageViewer::roiCreated, [&created](const Roi &roi) { created = roi; });
    for (const auto &points : {std::pair{QPointF(0, 20), QPointF(0, 70)},
             std::pair{QPointF(0, 70), QPointF(0, 20)}, std::pair{QPointF(20, 0), QPointF(70, 0)},
             std::pair{QPointF(70, 0), QPointF(20, 0)}}) {
        viewer.setSelection({}); created.reset();
        roiDrag(viewer, points.first, points.second, Qt::ShiftModifier);
        ASSERT_TRUE(created);
        EXPECT_EQ(created->points[0], points.first); EXPECT_EQ(created->points[1], points.second);
    }
}

TEST(Viewer, ShiftToggleUpdatesAnActiveDrawAndResizeWithoutMouseMotion)
{
    DocumentWidget document(Image::allocate(PixelType::UInt8, {100, 100}));
    auto &viewer = *document.viewer;
    viewer.resize(400, 400); viewer.setZoom(4); viewer.setTool(ImageViewer::Tool::Rectangle);
    roiMouse(viewer, QEvent::MouseButtonPress, {10, 10});
    roiMouse(viewer, QEvent::MouseMove, {40, 30});
    roiKey(viewer, Qt::Key_Shift);
    roiMouse(viewer, QEvent::MouseButtonRelease, {40, 30}, Qt::ShiftModifier);
    ASSERT_TRUE(document.selection); EXPECT_EQ(document.selection->bounds(), QRectF(10, 10, 30, 30));
    roiClick(viewer, {20, 20});
    roiMouse(viewer, QEvent::MouseButtonPress, {40, 40});
    roiMouse(viewer, QEvent::MouseMove, {60, 50}, Qt::ShiftModifier);
    roiKey(viewer, Qt::Key_Shift, QEvent::KeyRelease);
    roiMouse(viewer, QEvent::MouseButtonRelease, {60, 50});
    EXPECT_EQ(document.selection->bounds(), QRectF(10, 10, 50, 40));
}

TEST(Viewer, PolygonVerticesStayEditableAcrossSlicesAndHiddenRoisCannotBePicked)
{
    DocumentWidget document(Image::allocate(PixelType::UInt8, {100, 100, 2}));
    auto &viewer = *document.viewer;
    viewer.resize(400, 400); viewer.setZoom(4);
    Roi polygon; polygon.type = RoiType::Polygon; polygon.points = {{10, 10}, {50, 10}, {30, 50}};
    document.setSelection(polygon);
    roiClick(viewer, {30, 20});
    roiDrag(viewer, {30, 50}, {40, 60});
    ASSERT_TRUE(document.selection); EXPECT_EQ(document.selection->points[2], QPointF(40, 60));
    viewer.setSlice(1);
    roiDrag(viewer, {40, 60}, {45, 65});
    ASSERT_TRUE(document.selection); EXPECT_EQ(document.selection->points[2], QPointF(45, 65));
    viewer.setSlice(0);
    roiKey(viewer, Qt::Key_Escape);
    ASSERT_TRUE(document.selection); // Slice navigation preserves edit mode.
    roiKey(viewer, Qt::Key_Escape);
    EXPECT_FALSE(document.selection);
    // The same bounds on an invisible ROI must not enter edit mode.
    polygon.z = 1; polygon.visible = false; document.setSelection(polygon);
    roiClick(viewer, {30, 20});
    EXPECT_FALSE(document.selection);
}

TEST(Viewer, ManagedRoisCanBeSelectedAndResizedOnOtherSlices)
{
    DocumentWidget document(Image::allocate(PixelType::UInt8, {100, 100, 3}));
    auto &viewer = *document.viewer;
    viewer.resize(400, 400); viewer.setZoom(4);
    Roi roi; roi.z = 1; roi.points = {{20, 20}, {60, 60}};
    document.rois.push_back(roi); document.refreshRois();
    viewer.setSlice(2);
    roiClick(viewer, {40, 40});
    ASSERT_TRUE(document.selection); EXPECT_EQ(document.selection->id, roi.id);
    roiDrag(viewer, {60, 60}, {70, 65});
    EXPECT_EQ(document.rois.front().bounds(), QRectF(20, 20, 50, 45));
    viewer.setSlice(0);
    roiDrag(viewer, {70, 65}, {80, 75});
    EXPECT_EQ(document.rois.front().bounds(), QRectF(20, 20, 60, 55));
    EXPECT_EQ(document.rois.front().z, 1u);
}

TEST(Viewer, EscapeCancelsAnActiveResizeAndDegeneratePolygonEditsAreRejected)
{
    DocumentWidget document(Image::allocate(PixelType::UInt8, {100, 100}));
    auto &viewer = *document.viewer;
    viewer.resize(400, 400); viewer.setZoom(4);
    Roi roi; roi.points = {{20, 20}, {60, 60}};
    document.setSelection(roi);
    roiClick(viewer, {40, 40});
    roiMouse(viewer, QEvent::MouseButtonPress, {60, 60});
    roiMouse(viewer, QEvent::MouseMove, {80, 70});
    roiKey(viewer, Qt::Key_Escape);
    roiMouse(viewer, QEvent::MouseButtonRelease, {80, 70});
    ASSERT_TRUE(document.selection);
    EXPECT_EQ(document.selection->points, roi.points);
    roiKey(viewer, Qt::Key_Escape); EXPECT_FALSE(document.selection);
    roi.type = RoiType::Polygon; roi.points = {{10, 10}, {50, 50}, {10, 50}};
    document.setSelection(roi);
    roiClick(viewer, {20, 35});
    roiDrag(viewer, {10, 50}, {30, 30});
    ASSERT_TRUE(document.selection); EXPECT_EQ(document.selection->points, roi.points);
}

TEST(Viewer, RoiHandlesRemainSmallAndYellowAtDifferentZoomLevels)
{
    ImageViewer viewer;
    viewer.resize(400, 400); viewer.setImage(Image::allocate(PixelType::UInt8, {100, 100, 3}));
    Roi roi; roi.points = {{20, 20}, {60, 60}};
    viewer.setSelection(roi);
    auto *overlay = viewer.findChildren<QWidget *>().back();
    for (const auto zoom : {2., 4.}) {
        viewer.setZoom(zoom);
        roiClick(viewer, {40, 40});
        viewer.setSlice(viewer.slice() + 1);
        const auto pixmap = overlay->grab();
        const auto pixels = pixmap.toImage();
        const auto handle = viewer.imageToWidget({20, 20}) * pixmap.devicePixelRatio();
        EXPECT_EQ(pixels.pixelColor(qRound(handle.x()), qRound(handle.y())), QColor(255, 213, 115));
        const auto outside = handle + QPointF(5, -5) * pixmap.devicePixelRatio();
        EXPECT_EQ(pixels.pixelColor(qRound(outside.x()), qRound(outside.y())).alpha(), 0);
        roiKey(viewer, Qt::Key_Escape);
    }
}

TEST(Viewer, ClosingWhileLazyDecodeIsActiveCancelsAndJoins)
{
    auto storage = std::make_shared<SlowSlices>();
    auto viewer = std::make_unique<ImageViewer>();
    viewer->setImage(Image::fromStorage(PixelType::UInt16, {16, 16, 6}, storage));
    ASSERT_TRUE(waitFor([&storage] { return storage->reads.load() > 2; }));
    viewer.reset();
    const int afterClose = storage->reads.load();
    QApplication::processEvents();
    EXPECT_EQ(storage->reads.load(), afterClose);
    EXPECT_LT(afterClose, 256);
}

TEST(Viewer, GpuDisplaysColorCompositeFloat64AndMask)
{
    if (QApplication::platformName() == "offscreen" || QApplication::platformName() == "minimal")
        GTEST_SKIP() << "Qt RHI requires a graphics-capable platform plugin";
    ImageViewer viewer;
    viewer.resize(240, 240);
    viewer.setScaleBarVisible(false);
    bool loaded = false;
    QString error;
    QObject::connect(&viewer, &ImageViewer::loadingChanged, [&loaded](bool loading) { loaded = !loading; });
    QObject::connect(&viewer, &ImageViewer::loadFailed, [&error](const QString &message) { error = message; });
    viewer.show();
    auto rgb = Image::allocate(PixelType::RGB8, {4, 4});
    for (std::size_t y = 0; y < 4; ++y) for (std::size_t x = 0; x < 4; ++x) {
        const std::array<std::size_t, 2> index{x, y};
        rgb.setValue(index, x < 2 ? 255 : 0, 0);
        rgb.setValue(index, x >= 2 ? 255 : 0, 2);
    }
    viewer.setImage(rgb);
    ASSERT_TRUE(waitFor([&loaded, &error] { return loaded || !error.isEmpty(); }));
    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    ASSERT_TRUE(waitFor([&viewer] { return displayColor(viewer, {.5, .5}).red() > 250; }));
    EXPECT_GT(displayColor(viewer, {3.5, .5}).blue(), 250);

    ImageMetadata metadata;
    metadata.channels = {{"Red", 0xffff0000}, {"Green", 0xff00ff00}};
    auto composite = Image::allocate(PixelType::UInt16, {4, 4, 1, 2}, metadata);
    for (std::size_t y = 0; y < 4; ++y) for (std::size_t x = 0; x < 4; ++x) {
        const std::array<std::size_t, 4> index{x, y, 0, x < 2 ? 0u : 1u};
        composite.setValue(index, 100);
    }
    viewer.setImage(composite);
    viewer.setComposite(true);
    ASSERT_TRUE(waitFor([&loaded] { return loaded; }));
    ASSERT_TRUE(waitFor([&viewer] { return displayColor(viewer, {3.5, .5}).green() > 250; }));
    EXPECT_GT(displayColor(viewer, {.5, .5}).red(), 250);

    auto precise = Image::allocate(PixelType::Float64, {4, 4});
    for (std::size_t y = 0; y < 4; ++y) for (std::size_t x = 0; x < 4; ++x) {
        const std::array<std::size_t, 2> index{x, y};
        precise.setValue(index, 1e12 + double(x) * .125);
    }
    viewer.setComposite(false);
    viewer.setImage(precise);
    ASSERT_TRUE(waitFor([&loaded] { return loaded; }));
    ASSERT_TRUE(waitFor([&viewer] { return displayColor(viewer, {3.5, .5}).red() > 250; }));
    EXPECT_LT(displayColor(viewer, {.5, .5}).red(), 5);
    // The scientific data stays in float64 while the GPU receives an affine encoding.
    EXPECT_DOUBLE_EQ(viewer.image().sample(1, 0), 1e12 + .125);

    auto mask = Image::allocate(PixelType::UInt8, {4, 4});
    const std::array<std::size_t, 2> origin{0, 0};
    mask.setValue(origin, 255);
    viewer.setMask(mask);
    ASSERT_TRUE(waitFor([&viewer] { return displayColor(viewer, {.5, .5}).red() > 70; }));
    EXPECT_TRUE(error.isEmpty()) << error.toStdString();
}

TEST(Viewer, GpuHonorsDicomMonochrome1AndPreservesManualChoice)
{
    if (QApplication::platformName() == "offscreen" || QApplication::platformName() == "minimal")
        GTEST_SKIP() << "Qt RHI requires a graphics-capable platform plugin";
    ImageMetadata metadata;
    metadata.sourcePath = QStringLiteral("monochrome1.dcm");
    metadata.attributes.insert(QStringLiteral("DICOM.0028,0004"), QStringLiteral("MONOCHROME1"));
    auto image = Image::allocate(PixelType::UInt16, {4, 4, 2}, metadata);
    for (std::size_t z = 0; z < 2; ++z)
        for (std::size_t y = 0; y < 4; ++y)
            for (std::size_t x = 0; x < 4; ++x) {
                const std::array<std::size_t, 3> index{x, y, z};
                image.setValue(index, x < 2 ? 0 : 4095);
            }
    bool loaded = false;
    QString error;
    ImageViewer viewer;
    viewer.resize(240, 240);
    viewer.setScaleBarVisible(false);
    QObject::connect(&viewer, &ImageViewer::loadingChanged, [&loaded](bool loading) { loaded = !loading; });
    QObject::connect(&viewer, &ImageViewer::loadFailed, [&error](const QString &message) { error = message; });
    viewer.setImage(image);
    viewer.show();
    ASSERT_TRUE(waitFor([&loaded] { return loaded; }));
    ASSERT_TRUE(waitFor([&viewer] { return displayColor(viewer, {.5, .5}).red() > 250; }));
    EXPECT_LT(displayColor(viewer, {3.5, .5}).red(), 5);

    viewer.setInverted(false);
    viewer.setSlice(1);
    ASSERT_TRUE(waitFor([&loaded] { return loaded; }));
    ASSERT_TRUE(waitFor([&viewer] { return displayColor(viewer, {.5, .5}).red() < 5; }));
    EXPECT_GT(displayColor(viewer, {3.5, .5}).red(), 250);
    auto replacement = image.clone();
    replacement.metadata().sourcePath.clear();
    viewer.setImage(replacement, true);
    ASSERT_TRUE(waitFor([&loaded] { return loaded; }));
    EXPECT_FALSE(viewer.isInverted());
    EXPECT_LT(displayColor(viewer, {.5, .5}).red(), 5);

    replacement.metadata().sourcePath = QStringLiteral("monochrome2.dcm");
    replacement.metadata().attributes.insert(QStringLiteral("DICOM.0028,0004"), QStringLiteral("MONOCHROME2"));
    viewer.setInverted(true);
    viewer.setImage(replacement);
    ASSERT_TRUE(waitFor([&loaded] { return loaded; }));
    ASSERT_TRUE(waitFor([&viewer] { return displayColor(viewer, {.5, .5}).red() < 5; }));
    EXPECT_FALSE(viewer.isInverted());
    EXPECT_DOUBLE_EQ(image.sample(0, 0), 0);
    EXPECT_DOUBLE_EQ(image.sample(3, 0), 4095);
    EXPECT_TRUE(error.isEmpty()) << error.toStdString();
}
