#include "document_widget.h"
#include "io/io_detail.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QHBoxLayout>
#include <QScopedValueRollback>
#include <cmath>
#include <QLabel>
#include <QSlider>
#include <QVBoxLayout>
#include <QSignalBlocker>
#include <QCloseEvent>
#include <QEvent>
#include <QMessageBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <algorithm>
#include <limits>

namespace imagecxx {
std::optional<std::array<double, 3>> mapImagePosition(const Image &source, const Image &target,
                                                   std::array<double, 3> position)
{
    const auto calibration = [](const Image &image) {
        std::array<std::array<double, 4>, 3> matrix{};
        const auto &metadata = image.metadata();
        const auto dimensions = std::size_t(std::sqrt(metadata.direction.size()));
        const auto embedded = QJsonDocument::fromJson(metadata.attributes.value(
            QStringLiteral("ImageCXX.EmbeddedPlaneOrigin")).toUtf8()).array();
        for (std::size_t row = 0; row < 3; ++row) {
            matrix[row][3] = row < metadata.axes.size() ? metadata.axes[row].origin
                : embedded.size() == 3 ? embedded[int(row)].toDouble() : 0;
            for (std::size_t column = 0; column < 3; ++column) {
                const double direction = row < dimensions && column < dimensions
                    ? metadata.direction[row * dimensions + column] : double(row == column);
                matrix[row][column] = direction * (column < metadata.axes.size() ? metadata.axes[column].spacing : 1);
            }
        }
        return matrix;
    };
    const auto from = calibration(source);
    auto system = calibration(target);
    for (std::size_t row = 0; row < 3; ++row) {
        const auto &sourceMetadata = source.metadata();
        const auto &targetMetadata = target.metadata();
        const QString sourceUnit = row < sourceMetadata.axes.size() ? sourceMetadata.axes[row].unit
            : sourceMetadata.attributes.value("ImageCXX.EmbeddedPlaneZUnit", sourceMetadata.axes[0].unit);
        const QString targetUnit = row < targetMetadata.axes.size() ? targetMetadata.axes[row].unit
            : targetMetadata.attributes.value("ImageCXX.EmbeddedPlaneZUnit", targetMetadata.axes[0].unit);
        double factor = 1;
        if (sourceUnit != targetUnit) {
            try { factor = detail::spatialUnitToMillimeters(sourceUnit) / detail::spatialUnitToMillimeters(targetUnit); }
            catch (const std::exception &) { return std::nullopt; }
        }
        double world = from[row][3];
        for (std::size_t column = 0; column < 3; ++column) world += from[row][column] * position[column];
        system[row][3] = world * factor - system[row][3];
    }
    // Solve in doubles so patient positions retain precision at small pixel sizes.
    for (std::size_t column = 0; column < 3; ++column) {
        auto pivot = column;
        for (auto row = column + 1; row < 3; ++row)
            if (std::abs(system[row][column]) > std::abs(system[pivot][column])) pivot = row;
        if (std::abs(system[pivot][column]) < 1e-15) return std::nullopt;
        std::swap(system[column], system[pivot]);
        const double divisor = system[column][column];
        for (auto &value : system[column]) value /= divisor;
        for (std::size_t row = 0; row < 3; ++row) if (row != column) {
            const double multiplier = system[row][column];
            for (std::size_t entry = 0; entry < 4; ++entry)
                system[row][entry] -= multiplier * system[column][entry];
        }
    }
    std::array<double, 3> result{system[0][3], system[1][3], system[2][3]};
    if (!std::all_of(result.begin(), result.end(), [](double value) { return std::isfinite(value); })) return std::nullopt;
    return result;
}

DocumentWidget::DocumentWidget(Image source, QWidget *parent) : ImageWindow(parent),
    image(std::move(source))
{
    viewer->setObjectName("xyView");
    views_.append(viewer);
    connectViewer(viewer, 0);
    spatial_ = {image.width() / 2, image.height() / 2, 0};
    addNavigationControls(viewer, 0, static_cast<QVBoxLayout *>(layout()));
    connect(this, &DocumentWidget::changed, this, [this] {
        setWindowTitle(title());
        for (int i = 1; i < views_.size(); ++i) {
            auto *window = views_[i]->window();
            window->setWindowTitle(title() + " · " + window->property("viewLabel").toString());
        }
    });
    refresh();
}

DocumentWidget::~DocumentWidget()
{
    unlink();
    synchronizing_ = true;
    while (views_.size() > 1) delete views_.takeLast()->window();
}

void DocumentWidget::addNavigationControls(ImageViewer *view, int index, QVBoxLayout *layout)
{
    auto *window = view->window();
    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(14, 10, 14, 10);
    NavigationControls controls{};
    controls.axis = new QLabel(window);
    controls.axis->setObjectName("sliceAxisLabel");
    controls.slice = new QSlider(Qt::Horizontal, window);
    controls.slice->setObjectName("sliceSlider");
    controls.axis->setBuddy(controls.slice);
    bar->addWidget(controls.axis);
    bar->addWidget(controls.slice, 1);
    connect(controls.slice, &QSlider::valueChanged, this, [this, index](int value) {
        auto position = spatial_;
        position[viewAxes(index)[2]] = std::size_t(value);
        setPosition(position[0], position[1], position[2], viewer->channel(), viewer->time());
    });
    controls.position = new QLabel(window);
    controls.position->setObjectName("slicePosition");
    bar->addWidget(controls.position);
    navigation_.append(controls);
    layout->addLayout(bar);
}

std::array<std::size_t, 3> DocumentWidget::viewAxes(int index) const
{
    if (viewMode_ == ViewMode::Orthogonal) {
        if (index == 1) return {0, 2, 1};
        if (index == 2) return {1, 2, 0};
    }
    return {0, 1, 2};
}

void DocumentWidget::connectViewer(ImageViewer *view, int index)
{
    connect(view, &ImageViewer::pixelHovered, this, [this, index](int x, int y, double value) {
        auto point = spatial_;
        const auto axes = viewAxes(index);
        point[axes[0]] = std::size_t(x); point[axes[1]] = std::size_t(y);
        emit pixelHovered(point[0], point[1], point[2], value);
    });
    if (index > 0) connect(view, &ImageViewer::loadFailed, this, &DocumentWidget::viewError);
    connect(view, &ImageViewer::sliceChanged, this, [this, index](std::size_t z, std::size_t c, std::size_t t) {
        if (synchronizing_) return;
        auto position = spatial_;
        position[viewAxes(index)[2]] = z;
        setPosition(position[0], position[1], position[2], c, t);
    });
    connect(view, &ImageViewer::positionPicked, this, [this, index](QPointF point) {
        const auto axes = viewAxes(index);
        const auto coordinate = [this](double value, std::size_t axis) {
            return std::size_t(std::clamp(std::floor(value), 0.0, double(image.dimension(axis) - 1)));
        };
        spatial_[axes[0]] = coordinate(point.x(), axes[0]);
        spatial_[axes[1]] = coordinate(point.y(), axes[1]);
        setPosition(spatial_[0], spatial_[1], spatial_[2], viewer->channel(), viewer->time());
    });
    connect(view, &ImageViewer::displaySettingsChanged, this, [this, view] { synchronizeDisplay(view); });
    connect(view, &ImageViewer::viewTransformChanged, this, [this, view, index] { synchronizeTransform(view, index); });
    connect(view, &ImageViewer::toolChanged, this, [this, view] {
        if (synchronizing_) return;
        QScopedValueRollback guard(synchronizing_, true);
        for (auto *other : views_) if (other != view) {
            const auto tool = view->tool();
            other->setTool(viewMode_ == ViewMode::Orthogonal && other != viewer
                && tool != ImageViewer::Tool::Pointer && tool != ImageViewer::Tool::Pan
                ? ImageViewer::Tool::Pointer : tool);
        }
    });
    connect(view, &ImageViewer::overlaysChanged, this, [this, view](bool crosshair, bool scaleBar) {
        if (synchronizing_) return;
        QScopedValueRollback guard(synchronizing_, true);
        for (auto *other : views_) if (other != view) {
            other->setCrosshairVisible(crosshair); other->setScaleBarVisible(scaleBar);
        }
    });
    connect(view, &ImageViewer::roiCreated, this, [this](const Roi &roi) { setSelection(roi); });
    connect(view, &ImageViewer::roiSelected, this, [this](const Roi &roi) { setSelection(roi); });
    connect(view, &ImageViewer::selectionCleared, this, [this] { setSelection({}); });
    connect(view, &ImageViewer::roiEdited, this, &DocumentWidget::editRoi);
}

void DocumentWidget::synchronizeDisplay(ImageViewer *source)
{
    if (synchronizing_) return;
    QScopedValueRollback guard(synchronizing_, true);
    for (auto *view : views_) if (view != source) {
        view->setWindow(source->windowMinimum(), source->windowMaximum());
        view->setGamma(source->gamma());
        view->setInverted(source->isInverted());
        view->setLut(source->lut());
        view->setLinearInterpolation(source->linearInterpolation());
        view->setComposite(source->composite());
    }
    emit displayChanged();
}

void DocumentWidget::synchronizeTransform(ImageViewer *source, int index)
{
    if (synchronizing_) return;
    QScopedValueRollback guard(synchronizing_, true);
    if (std::none_of(linkedDocuments_.begin(), linkedDocuments_.end(), [](const auto &document) {
        return document && document->synchronizing_;
    })) for (auto &document : linkedDocuments_) if (document) {
        const auto center = source->viewCenter();
        const auto mapped = mapImagePosition(image, document->image,
            {center.x() - .5, center.y() - .5, double(source->slice())});
        const auto next = mapImagePosition(image, document->image,
            {center.x() + .5, center.y() - .5, double(source->slice())});
        if (mapped && next) {
            const auto &metadata = document->image.metadata().axes;
            const auto spacing = [&metadata](std::size_t axis) {
                return axis < metadata.size() && metadata[axis].unit == metadata[0].unit
                    ? metadata[axis].spacing / metadata[0].spacing : 1.0;
            };
            const double step = std::hypot((*next)[0] - (*mapped)[0],
                ((*next)[1] - (*mapped)[1]) * spacing(1), ((*next)[2] - (*mapped)[2]) * spacing(2));
            const auto slice = std::size_t(std::clamp(std::round((*mapped)[2]), 0.0, double(document->image.depth() - 1)));
            document->setPosition(document->spatial_[0], document->spatial_[1], slice,
                document->viewer->channel(), document->viewer->time());
            document->viewer->setViewTransform(source->zoom() / step, {(*mapped)[0] + .5, (*mapped)[1] + .5});
        }
    }
    const auto axes = viewAxes(index);
    center_[axes[0]] = source->viewCenter().x();
    center_[axes[1]] = source->viewCenter().y();
    if (source->fitsWindow()) {
        for (auto *view : views_) if (view != source) view->fitToWindow();
        return;
    }
    const auto spacing = [this](std::size_t axis) {
        const auto &metadata = image.metadata().axes;
        if (viewMode_ != ViewMode::Orthogonal || metadata.size() < 3
            || metadata[axis].unit != metadata[0].unit) return 1.0;
        return metadata[axis].spacing / metadata[0].spacing;
    };
    // Clamp one physical scale for the group before updating any window.
    double minimum = ImageViewer::minimumZoom, maximum = ImageViewer::maximumZoom;
    for (int i = 0; i < views_.size(); ++i) {
        const auto factor = spacing(viewAxes(i)[0]);
        minimum = std::max(minimum, ImageViewer::minimumZoom / factor);
        maximum = std::min(maximum, ImageViewer::maximumZoom / factor);
    }
    const auto scale = std::max(minimum, std::min(source->zoom() / spacing(axes[0]), maximum));
    for (int i = 0; i < views_.size(); ++i) {
        const auto target = viewAxes(i);
        const auto zoom = scale * spacing(target[0]);
        if (views_[i] != source || zoom != source->zoom())
            views_[i]->setViewTransform(zoom,
                {center_[target[0]], center_[target[1]]});
    }
}

void DocumentWidget::linkTo(QList<DocumentWidget *> documents)
{
    documents.removeAll(this);
    documents.removeAll(nullptr);
    for (auto *document : documents) {
        if (!mapImagePosition(image, document->image, {}) || !mapImagePosition(document->image, image, {})) {
            emit viewError(tr("These images have incompatible spatial units or an invalid spatial direction."));
            return;
        }
    }
    setViewMode(ViewMode::Single);
    for (auto *document : documents) document->setViewMode(ViewMode::Single);
    if (documents.isEmpty()) return;
    documents.append(this);
    for (auto *document : documents) {
        document->linkedDocuments_.clear();
        for (auto *other : documents)
            if (other != document && !document->linkedDocuments_.contains(other)) document->linkedDocuments_.append(other);
        document->viewMode_ = ViewMode::Linked;
    }
    setPosition(spatial_[0], spatial_[1], spatial_[2], viewer->channel(), viewer->time());
    synchronizeTransform(viewer, 0);
    for (auto *document : documents) {
        document->showViews();
        emit document->changed();
    }
}

void DocumentWidget::setViewMode(ViewMode mode)
{
    if (mode == ViewMode::Linked) return;
    if (mode == ViewMode::Orthogonal && image.depth() < 2) return;
    unlink();
    if (mode == viewMode_) {
        showViews();
        emit changed();
        return;
    }
    {
        QScopedValueRollback guard(synchronizing_, true);
        while (views_.size() > 1) {
            navigation_.removeLast();
            delete views_.takeLast()->window();
        }
        viewMode_ = mode;
        const int count = mode == ViewMode::Single ? 1 : 3;
        for (int i = 1; i < count; ++i) {
            // Sibling windows can stack independently of the document's XY view.
            auto *window = new ImageWindow(parentWidget());
            window->setObjectName("synchronizedViewWindow");
            window->setAttribute(Qt::WA_ShowWithoutActivating);
            window->installEventFilter(this);
            auto *view = window->viewer;
            addNavigationControls(view, i, static_cast<QVBoxLayout *>(window->layout()));
            view->setObjectName(i == 1 ? "xzView" : "yzView");
            const auto axes = viewAxes(i);
            view->setImage(mode == ViewMode::Orthogonal ? image.permutedSpatialAxes(axes) : image);
            view->setTool(mode == ViewMode::Orthogonal && viewer->tool() != ImageViewer::Tool::Pan
                ? ImageViewer::Tool::Pointer : viewer->tool());
            view->setCrosshairVisible(viewer->crosshairVisible());
            view->setScaleBarVisible(viewer->scaleBarVisible());
            views_.append(view);
            connectViewer(view, i);
            connect(view, &ImageViewer::sliceReady, window, [this, window] {
                window->setProperty("sliceReady", true);
                if (isVisible()) window->show();
            }, Qt::SingleShotConnection);
        }
        center_ = {double(image.width()) / 2, double(image.height()) / 2, double(image.depth()) / 2};
        updateNavigation(viewer->channel(), viewer->time());
        for (int i = 0; i < views_.size(); ++i) {
            const auto axes = viewAxes(i);
            const auto &metadata = image.metadata().axes;
            const double aspect = metadata[axes[0]].unit == metadata[axes[1]].unit
                ? metadata[axes[1]].spacing / metadata[axes[0]].spacing : 1.0;
            views_[i]->setPixelAspectRatio(aspect);
            views_[i]->setToolTip(mode == ViewMode::Orthogonal
                ? tr("Pointer tool: click or drag to move the intersection. Pan tool: drag to pan. Wheel to browse. Ctrl+wheel to zoom. Edit selections in XY.")
                : tr("Wheel to browse. Ctrl+wheel to zoom. Drag with the Pan tool to pan."));
            if (i > 0) {
                auto *window = static_cast<ImageWindow *>(views_[i]->window());
                window->setScreen(screen());
                window->resizeToImage(aspect);
                window->move(frameGeometry().topLeft() + QPoint(28 * i, 28 * i));
            }
        }
        for (auto *view : views_) view->fitToWindow();
    }
    synchronizeDisplay(viewer);
    refreshRois();
    emit changed();
}

void DocumentWidget::unlink()
{
    if (linkedDocuments_.isEmpty()) return;
    const auto documents = linkedDocuments_;
    linkedDocuments_.clear();
    viewMode_ = ViewMode::Single;
    for (auto &other : documents) if (other) {
        other->linkedDocuments_.clear();
        other->viewMode_ = ViewMode::Single;
        emit other->changed();
    }
    emit changed();
}

void DocumentWidget::showViews()
{
    showNormal();
    for (int i = 1; i < views_.size(); ++i) {
        auto *window = views_[i]->window();
        if (window->property("sliceReady").toBool()) window->showNormal();
    }
}

void DocumentWidget::setPosition(std::size_t x, std::size_t y, std::size_t z, std::size_t channel, std::size_t time)
{
    if (image.empty() || synchronizing_) return;
    const bool sliceChanged = spatial_[2] != std::min(z, image.depth() - 1);
    {
        QScopedValueRollback guard(synchronizing_, true);
        spatial_ = {std::min(x, image.width() - 1), std::min(y, image.height() - 1), std::min(z, image.depth() - 1)};
        updateNavigation(std::min(channel, image.channels() - 1), std::min(time, image.frames() - 1));
        if (std::none_of(linkedDocuments_.begin(), linkedDocuments_.end(), [](const auto &document) {
            return document && document->synchronizing_;
        })) for (auto &document : linkedDocuments_) if (document) {
            if (const auto mapped = mapImagePosition(image, document->image,
                {double(spatial_[0]), double(spatial_[1]), double(spatial_[2])})) {
                std::array<std::size_t, 3> position;
                for (std::size_t axis = 0; axis < 3; ++axis)
                    position[axis] = std::size_t(std::clamp(std::round((*mapped)[axis]), 0.0,
                        double(document->image.dimension(axis) - 1)));
                document->setPosition(position[0], position[1], position[2], viewer->channel(), viewer->time());
            }
        }
    }
    if (sliceChanged && !linkedDocuments_.isEmpty()) synchronizeTransform(viewer, 0);
    emit displayChanged();
}

void DocumentWidget::updateNavigation(std::size_t channel, std::size_t time)
{
    for (int i = 0; i < views_.size(); ++i) {
        const auto axes = viewAxes(i);
        views_[i]->setSlice(spatial_[axes[2]], channel, time);
        const bool orthogonal = viewMode_ == ViewMode::Orthogonal;
        views_[i]->setNavigationCrosshair(orthogonal
            ? std::optional<QPointF>{{double(spatial_[axes[0]]) + .5, double(spatial_[axes[1]]) + .5}} : std::nullopt);
        const QStringList names{QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z")};
        const QString label = orthogonal ? tr("%1%2  ·  %3 %4/%5").arg(names[int(axes[0])], names[int(axes[1])], names[int(axes[2])])
            .arg(spatial_[axes[2]] + 1).arg(image.dimension(axes[2])) : QString{};
        views_[i]->setViewLabel(orthogonal ? label : QString{});
        if (i > 0) {
            views_[i]->window()->setProperty("viewLabel", label);
            views_[i]->window()->setWindowTitle(title() + " · " + label);
        }

        auto &controls = navigation_[i];
        const auto length = image.dimension(axes[2]);
        const auto position = spatial_[axes[2]];
        const auto axis = names[int(axes[2])];
        const QSignalBlocker blocker(controls.slice);
        const auto maximum = std::min<std::size_t>(length - 1, std::numeric_limits<int>::max());
        controls.slice->setRange(0, int(maximum));
        controls.slice->setValue(int(std::min(position, maximum)));
        controls.slice->setEnabled(length > 1);
        controls.slice->setAccessibleName(tr("%1 slice").arg(axis));
        controls.axis->setText(axis);
        controls.position->setText(tr("%1/%2").arg(position + 1).arg(length));
    }
}

void DocumentWidget::dragEnterEvent(QDragEnterEvent *event)
{
    for (const auto &url : event->mimeData()->urls())
        if (url.isLocalFile()) { event->acceptProposedAction(); return; }
}

void DocumentWidget::dropEvent(QDropEvent *event)
{
    QStringList paths;
    for (const auto &url : event->mimeData()->urls())
        if (url.isLocalFile()) paths.append(url.toLocalFile());
    if (!paths.isEmpty()) { emit pathsDropped(paths); event->acceptProposedAction(); }
}

void DocumentWidget::setSelection(std::optional<Roi> roi)
{
    if (selection) previousSelection = selection;
    selection = std::move(roi);
    selectionCleared = !selection.has_value();
    refreshRois();
    emit changed();
}

void DocumentWidget::refreshRois()
{
    for (int i = 0; i < views_.size(); ++i) {
        const bool xy = viewAxes(i)[2] == 2;
        views_[i]->setRois(xy ? rois : std::vector<Roi>{});
        views_[i]->setSelection(xy ? selection : std::nullopt);
    }
}

void DocumentWidget::editRoi(Roi roi)
{
    const auto saved = std::find_if(rois.begin(), rois.end(), [&roi](const Roi &entry) { return entry.id == roi.id; });
    if (saved != rois.end()) {
        // A selection may project saved geometry onto the currently viewed slice.
        const auto z = saved->z, channel = saved->channel, time = saved->time;
        *saved = roi;
        saved->z = z; saved->channel = channel; saved->time = time;
    }
    if (saved == rois.end() || (selection && selection->id == roi.id)) {
        if (selection && selection->id == roi.id) {
            roi.z = selection->z; roi.channel = selection->channel; roi.time = selection->time;
        }
        setSelection(std::move(roi));
    } else {
        refreshRois();
        emit changed();
    }
}

QString DocumentWidget::title() const
{
    return (image.metadata().name.isEmpty() ? tr("Untitled") : image.metadata().name) + (modified ? " *" : "");
}

void DocumentWidget::replaceImage(Image replacement, const QString &label)
{
    history.push(image, label);
    image = std::move(replacement);
    modified = true;
    ++revision;
    // Geometry-changing operations invalidate coordinates. Other filters preserve them.
    std::erase_if(rois, [this](const Roi &roi) { return roi.z >= image.depth()
        || roi.channel >= image.channels() || roi.time >= image.frames()
        || roi.bounds().right() > image.width() || roi.bounds().bottom() > image.height(); });
    if (selection && (selection->bounds().right() > image.width() || selection->bounds().bottom() > image.height()
        || selection->z >= image.depth() || selection->channel >= image.channels() || selection->time >= image.frames())) {
        previousSelection = selection; selection.reset(); selectionCleared = true;
    }
    refresh();
    emit changed();
}

void DocumentWidget::refresh()
{
    setWindowTitle(title());
    const auto oldC = viewer->channel(), oldT = viewer->time();
    if (viewMode_ == ViewMode::Orthogonal && image.depth() < 2) setViewMode(ViewMode::Single);
    {
        QScopedValueRollback guard(synchronizing_, true);
        center_ = {double(image.width()) / 2, double(image.height()) / 2, double(image.depth()) / 2};
        for (std::size_t axis = 0; axis < 3; ++axis)
            spatial_[axis] = std::min(spatial_[axis], image.dimension(axis) - 1);
        for (int i = 0; i < views_.size(); ++i) {
            auto *view = views_[i];
            view->setImage(viewMode_ == ViewMode::Orthogonal && i > 0
                ? image.permutedSpatialAxes(viewAxes(i)) : image, !view->image().empty());
            const auto axes = viewAxes(i);
            const auto &metadata = image.metadata().axes;
            view->setPixelAspectRatio(metadata[axes[0]].unit == metadata[axes[1]].unit
                ? metadata[axes[1]].spacing / metadata[axes[0]].spacing : 1.0);
        }
        updateNavigation(std::min(oldC, image.channels() - 1), std::min(oldT, image.frames() - 1));
    }
    synchronizeDisplay(viewer);
    refreshRois();
}

bool DocumentWidget::event(QEvent *event)
{
    const bool handled = ImageWindow::event(event);
    if (event->type() == QEvent::WindowActivate) emit activated();
    if (event->type() == QEvent::Show) {
        for (int i = 1; i < views_.size(); ++i) {
            auto *window = views_[i]->window();
            if (window->property("sliceReady").toBool()) window->show();
        }
    }
    return handled;
}

bool DocumentWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::WindowActivate) emit activated();
    if (event->type() == QEvent::DragEnter) { dragEnterEvent(static_cast<QDragEnterEvent *>(event)); return event->isAccepted(); }
    if (event->type() == QEvent::Drop) { dropEvent(static_cast<QDropEvent *>(event)); return event->isAccepted(); }
    return QWidget::eventFilter(watched, event);
}

void DocumentWidget::closeEvent(QCloseEvent *event)
{
    if (modified) {
        QMessageBox prompt(QMessageBox::Question, tr("Close image"),
            tr("%1 has unsaved pixel changes.\n\nClose without saving this image?")
                .arg(image.metadata().name.isEmpty() ? tr("Untitled") : image.metadata().name),
            QMessageBox::Discard | QMessageBox::Cancel, this);
        prompt.setTextFormat(Qt::PlainText);
        prompt.setDefaultButton(QMessageBox::Cancel);
        if (prompt.exec() != QMessageBox::Discard) { event->ignore(); return; }
    }
    for (int i = 1; i < views_.size(); ++i) views_[i]->window()->hide();
    unlink();
    emit closing();
    event->accept();
}

} // namespace imagecxx
