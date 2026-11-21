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
    addNavigationControls(this, 0);
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

void DocumentWidget::addNavigationControls(ImageWindow *window, int index)
{
    const auto controls = addSliceControls(window);
    connect(controls.slice, &QSlider::valueChanged, this, [this, index](int value) {
        auto position = spatial_;
        position[orthogonalAxes(orthogonal_, index)[2]] = std::size_t(value);
        setPosition(position[0], position[1], position[2], viewer->channel(), viewer->time());
    });
    navigation_.append(controls);
}

void DocumentWidget::connectViewer(ImageViewer *view, int index)
{
    connect(view, &ImageViewer::toolDimensionsChanged, this, [this, view, index](const QString &text) {
        if (index == 0 || view->underMouse()) emit toolDimensionsChanged(text);
    });
    connect(view, &ImageViewer::pixelHovered, this, [this, index, view](int x, int y, double value) {
        auto point = spatial_;
        const auto axes = orthogonalAxes(orthogonal_, index);
        point[axes[0]] = std::size_t(x); point[axes[1]] = std::size_t(y);
        emit toolDimensionsChanged(view->toolDimensions());
        emit pixelHovered(point[0], point[1], point[2], value);
    });
    if (index > 0) connect(view, &ImageViewer::loadFailed, this, &DocumentWidget::viewError);
    connect(view, &ImageViewer::sliceChanged, this, [this, index](std::size_t z, std::size_t c, std::size_t t) {
        if (synchronizing_) return;
        auto position = spatial_;
        position[orthogonalAxes(orthogonal_, index)[2]] = z;
        setPosition(position[0], position[1], position[2], c, t);
    });
    connect(view, &ImageViewer::positionPicked, this, [this, index](QPointF point) {
        const auto axes = orthogonalAxes(orthogonal_, index);
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
            other->setTool(orthogonal_ && other != viewer ? ImageViewer::Tool::Pointer : tool);
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
    synchronizeOrthogonalTransform(image, orthogonal_, views_, source, index, center_);
    if (std::none_of(linkedDocuments_.begin(), linkedDocuments_.end(), [](const auto &document) {
        return document && document->synchronizing_;
    })) for (auto &document : linkedDocuments_) if (document) {
        const auto axes = orthogonalAxes(orthogonal_, index);
        const auto center = source->viewCenter();
        std::array<double, 3> point{};
        point[axes[0]] = center.x() - .5;
        point[axes[1]] = center.y() - .5;
        point[axes[2]] = orthogonal_ ? center_[axes[2]] - .5 : double(source->slice());
        auto nextPoint = point;
        nextPoint[axes[0]] += 1.0;
        const auto mapped = mapImagePosition(image, document->image, point);
        const auto next = mapImagePosition(image, document->image, nextPoint);
        if (mapped && next) {
            const auto &metadata = document->image.metadata().axes;
            const auto spacing = [&metadata](std::size_t axis) {
                return axis < metadata.size() && metadata[axis].unit == metadata[0].unit
                    ? metadata[axis].spacing / metadata[0].spacing : 1.0;
            };
            const double step = std::hypot((*next)[0] - (*mapped)[0],
                ((*next)[1] - (*mapped)[1]) * spacing(1), ((*next)[2] - (*mapped)[2]) * spacing(2));
            if (document->orthogonal_) {
                document->center_[2] = (*mapped)[2] + .5;
            } else if (!orthogonal_) {
                const auto slice = std::size_t(std::clamp(std::round((*mapped)[2]), 0.0, double(document->image.depth() - 1)));
                document->setPosition(document->spatial_[0], document->spatial_[1], slice,
                    document->viewer->channel(), document->viewer->time());
            }
            document->viewer->setViewTransform(source->zoom() / step, {(*mapped)[0] + .5, (*mapped)[1] + .5});
        }
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
    unlink();
    for (auto *document : documents) document->unlink();
    if (documents.isEmpty()) return;
    documents.append(this);
    for (auto *document : documents) {
        document->linkedDocuments_.clear();
        for (auto *other : documents)
            if (other != document && !document->linkedDocuments_.contains(other)) document->linkedDocuments_.append(other);
    }
    setPosition(spatial_[0], spatial_[1], spatial_[2], viewer->channel(), viewer->time());
    synchronizeTransform(viewer, 0);
    for (auto *document : documents) {
        document->showViews();
        emit document->changed();
    }
}

void DocumentWidget::setOrthogonalViews(bool enabled)
{
    if (enabled && image.depth() < 2) return;
    if (enabled == orthogonal_) {
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
        orthogonal_ = enabled;
        const int count = enabled ? 3 : 1;
        for (int i = 1; i < count; ++i) {
            // Sibling windows can stack independently of the document's XY view.
            auto *window = new ImageWindow(parentWidget());
            window->setObjectName("synchronizedViewWindow");
            window->setAttribute(Qt::WA_ShowWithoutActivating);
            window->installEventFilter(this);
            auto *view = window->viewer;
            addNavigationControls(window, i);
            view->setObjectName(i == 1 ? "xzView" : "yzView");
            const auto axes = orthogonalAxes(orthogonal_, i);
            view->setImage(image.permutedSpatialAxes(axes));
            view->setTool(ImageViewer::Tool::Pointer);
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
            const auto axes = orthogonalAxes(orthogonal_, i);
            const auto &metadata = image.metadata().axes;
            const double aspect = metadata[axes[0]].unit == metadata[axes[1]].unit
                ? metadata[axes[1]].spacing / metadata[axes[0]].spacing : 1.0;
            views_[i]->setPixelAspectRatio(aspect);
            views_[i]->setToolTip(enabled
                ? tr("Pointer tool: click or drag to move the intersection. Middle-button or Space + drag to pan. Wheel to browse. Pinch or Ctrl+wheel to zoom. Edit selections in XY.")
                : tr("Wheel to browse. Pinch or Ctrl+wheel to zoom. Middle-button or Space + drag to pan."));
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
    if (!linkedDocuments_.isEmpty()) viewer->fitToWindow();
    emit changed();
}

void DocumentWidget::unlink()
{
    if (linkedDocuments_.isEmpty()) return;
    const auto documents = linkedDocuments_;
    linkedDocuments_.clear();
    for (auto &other : documents) if (other) {
        other->linkedDocuments_.clear();
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

void DocumentWidget::setPosition(std::size_t x, std::size_t y, std::size_t z, std::size_t channel, std::size_t time, bool propagate)
{
    if (image.empty() || synchronizing_) return;
    const bool sliceChanged = spatial_[2] != std::min(z, image.depth() - 1);
    {
        QScopedValueRollback guard(synchronizing_, true);
        spatial_ = {std::min(x, image.width() - 1), std::min(y, image.height() - 1), std::min(z, image.depth() - 1)};
        updateNavigation(std::min(channel, image.channels() - 1), std::min(time, image.frames() - 1));
        if (propagate && std::none_of(linkedDocuments_.begin(), linkedDocuments_.end(), [](const auto &document) {
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
    if (propagate && sliceChanged && !orthogonal_ && !linkedDocuments_.isEmpty()) synchronizeTransform(viewer, 0);
    emit positionChanged();
    emit displayChanged();
}

void DocumentWidget::updateNavigation(std::size_t channel, std::size_t time)
{
    updateSliceControls(image, orthogonal_, views_, navigation_, spatial_, channel, time, title());
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
    auto outlines = rois;
    outlines.insert(outlines.end(), gelLanes.begin(), gelLanes.end());
    for (int i = 0; i < views_.size(); ++i) {
        const bool xy = orthogonalAxes(orthogonal_, i)[2] == 2;
        views_[i]->setAnnotations(xy && annotationsVisible ? annotations : std::vector<Annotation>{});
        views_[i]->setRois(xy ? outlines : std::vector<Roi>{});
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
    if (orthogonal_ && image.depth() < 2) setOrthogonalViews(false);
    {
        QScopedValueRollback guard(synchronizing_, true);
        center_ = {double(image.width()) / 2, double(image.height()) / 2, double(image.depth()) / 2};
        for (std::size_t axis = 0; axis < 3; ++axis)
            spatial_[axis] = std::min(spatial_[axis], image.dimension(axis) - 1);
        for (int i = 0; i < views_.size(); ++i) {
            auto *view = views_[i];
            const auto axes = orthogonalAxes(orthogonal_, i);
            view->setImage(orthogonal_ && i > 0 ? image.permutedSpatialAxes(axes) : image, !view->image().empty());
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
