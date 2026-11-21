#include "overlay_window.h"
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScopedValueRollback>
#include <QSet>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace imagecxx {
OverlayWindow::OverlayWindow(DocumentWidget *document, QWidget *parent)
    : ImageWindow(parent), document_(document), editor_(new QDialog(parent, Qt::Window)), revision_(document->revision)
{
    setObjectName("overlayViewWindow");
    viewer->setObjectName("overlayView");
    views_.append(viewer);
    spatial_ = document->spatialPosition();
    center_ = {double(document->image.width()) / 2, double(document->image.height()) / 2,
               double(document->image.depth()) / 2};
    addNavigationControls(this, 0);
    connectViewer(viewer, 0);
    viewer->setImage(document->image);
    viewer->setSlice(spatial_[2], document->viewer->channel(), document->viewer->time());
    viewer->setToolTip(tr("Wheel to browse. Pinch or Ctrl+wheel to zoom. Middle-button or Space + drag to pan."));

    editor_->setObjectName("overlayEditor");
    editor_->setWindowTitle(tr("Overlay view"));
    editor_->resize(600, 340);
    editor_->installEventFilter(this);
    auto *editorLayout = new QVBoxLayout(editor_);
    images_ = new QTreeWidget(editor_);
    images_->setObjectName("overlayImages");
    images_->setAccessibleName(tr("Overlay images"));
    images_->setHeaderHidden(true);
    images_->setRootIsDecorated(false);
    images_->setSelectionMode(QAbstractItemView::SingleSelection);
    images_->setDragDropMode(QAbstractItemView::InternalMove);
    images_->setDefaultDropAction(Qt::MoveAction);
    images_->setToolTip(tr("Drag images to reorder. The first image is the top layer."));
    auto *contents = new QHBoxLayout;
    contents->addWidget(images_, 1);
    auto *sidebar = new QGridLayout;
    sidebar->setAlignment(Qt::AlignTop);
    lut_ = new QComboBox(editor_);
    lut_->setObjectName("overlayLut");
    lut_->setAccessibleName(tr("LUT"));
    lut_->addItem(tr("grayscale"), "grayscale");
    lut_->addItem(tr("hot"), "hot");
    lut_->addItem(tr("viridis"), "viridis");
    opacity_ = new QSpinBox(editor_);
    opacity_->setObjectName("overlayOpacity");
    opacity_->setAccessibleName(tr("Opacity"));
    opacity_->setRange(0, 100);
    opacity_->setSuffix("%");
    opacitySlider_ = new QSlider(Qt::Horizontal, editor_);
    opacitySlider_->setObjectName("overlayOpacitySlider");
    opacitySlider_->setAccessibleName(tr("Opacity"));
    opacitySlider_->setRange(0, 100);
    auto *lutLabel = new QLabel(tr("LUT"), editor_);
    lutLabel->setContentsMargins(0, 2, 0, 0);
    sidebar->addWidget(lutLabel, 0, 0, Qt::AlignVCenter);
    sidebar->addWidget(lut_, 0, 1, Qt::AlignVCenter);
    sidebar->addWidget(new QLabel(tr("Opacity"), editor_), 1, 0, Qt::AlignVCenter);
    sidebar->addWidget(opacity_, 1, 1, Qt::AlignVCenter);
    sidebar->addWidget(opacitySlider_, 2, 0, 1, 2);
    contents->addLayout(sidebar);
    editorLayout->addLayout(contents);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, editor_);
    connect(buttons, &QDialogButtonBox::rejected, editor_, &QDialog::close);
    editorLayout->addWidget(buttons);
    connect(images_->model(), &QAbstractItemModel::rowsMoved, this, [this] { updateLayers(); });
    connect(images_->model(), &QAbstractItemModel::rowsInserted, this, [this] { updateLayers(); });
    connect(images_->model(), &QAbstractItemModel::rowsRemoved, this, [this] { updateLayers(); });
    connect(images_, &QTreeWidget::itemChanged, this, [this] { updateLayers(); });
    connect(images_, &QTreeWidget::currentItemChanged, this, [this] { refreshEditor(); });
    connect(lut_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (auto *item = images_->currentItem()) item->setData(0, Qt::UserRole + 1, lut_->currentData());
    });
    connect(opacity_, &QSpinBox::valueChanged, this, [this](int value) {
        if (auto *item = images_->currentItem()) item->setData(0, Qt::UserRole + 2, value);
    });
    connect(opacitySlider_, &QSlider::valueChanged, opacity_, &QSpinBox::setValue);
    connect(opacity_, &QSpinBox::valueChanged, opacitySlider_, &QSlider::setValue);
    connect(document, &DocumentWidget::closing, this, [this] {
        hide(); editor_->hide();
        for (int i = 1; i < views_.size(); ++i) views_[i]->window()->hide();
        deleteLater();
    });
    connect(document, &QObject::destroyed, this, [this] {
        hide(); editor_->hide();
        for (int i = 1; i < views_.size(); ++i) views_[i]->window()->hide();
        deleteLater();
    });
    resizeToImage();
}

OverlayWindow::~OverlayWindow()
{
    synchronizing_ = true;
    while (views_.size() > 1) {
        navigation_.removeLast();
        delete views_.takeLast()->window();
    }
    viewer->setOverlayLayers({});
    for (auto &[doc, entry] : layerPlanes_) disconnect(entry.displayConnection);
    layerPlanes_.clear();
    delete editor_;
}

QString OverlayWindow::baseTitle() const
{
    return document_ ? tr("%1 · Overlay").arg(document_->title()) : QString{};
}

void OverlayWindow::addNavigationControls(ImageWindow *window, int index)
{
    const auto controls = addSliceControls(window);
    connect(controls.slice, &QSlider::valueChanged, this, [this, index](int value) {
        auto position = spatial_;
        position[orthogonalAxes(orthogonal_, index)[2]] = std::size_t(value);
        setPosition(position[0], position[1], position[2], viewer->channel(), viewer->time());
    });
    navigation_.append(controls);
}

void OverlayWindow::connectViewer(ImageViewer *view, int index)
{
    connect(view, &ImageViewer::loadFailed, this, &OverlayWindow::viewError);
    connect(view, &ImageViewer::sliceChanged, this, [this, index](std::size_t z, std::size_t c, std::size_t t) {
        if (synchronizing_ || !document_ || !isVisible()) return;
        auto position = spatial_;
        position[orthogonalAxes(orthogonal_, index)[2]] = z;
        setPosition(position[0], position[1], position[2], c, t);
    });
    connect(view, &ImageViewer::positionPicked, this, [this, index](QPointF point) {
        if (!document_ || document_->image.empty()) return;
        const auto axes = orthogonalAxes(orthogonal_, index);
        const auto coordinate = [this](double value, std::size_t axis) {
            return std::size_t(std::clamp(std::floor(value), 0.0, double(document_->image.dimension(axis) - 1)));
        };
        auto position = spatial_;
        position[axes[0]] = coordinate(point.x(), axes[0]);
        position[axes[1]] = coordinate(point.y(), axes[1]);
        setPosition(position[0], position[1], position[2], viewer->channel(), viewer->time());
    });
    connect(view, &ImageViewer::viewTransformChanged, this, [this, view, index] {
        if (synchronizing_ || !document_) return;
        QScopedValueRollback guard(synchronizing_, true);
        synchronizeOrthogonalTransform(document_->image, orthogonal_, views_, view, index, center_);
    });
    connect(view, &ImageViewer::overlaysChanged, this, [this, view](bool crosshair, bool scaleBar) {
        if (synchronizing_) return;
        QScopedValueRollback guard(synchronizing_, true);
        for (auto *other : views_) if (other != view) {
            other->setCrosshairVisible(crosshair);
            other->setScaleBarVisible(scaleBar);
        }
    });
}

void OverlayWindow::updateNavigation(std::size_t channel, std::size_t time)
{
    if (!document_) return;
    updateSliceControls(document_->image, orthogonal_, views_, navigation_, spatial_, channel, time, baseTitle());
}

void OverlayWindow::setOrthogonalViews(bool enabled)
{
    if (!document_) return;
    const auto &reference = document_->image;
    if (enabled && reference.depth() < 2) return;
    if (enabled == orthogonal_) {
        showViews();
        emit visibilityChanged();
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
            auto *window = new ImageWindow(parentWidget());
            window->setObjectName("synchronizedViewWindow");
            window->setAttribute(Qt::WA_ShowWithoutActivating);
            window->installEventFilter(this);
            auto *view = window->viewer;
            addNavigationControls(window, i);
            view->setObjectName(i == 1 ? "overlayXzView" : "overlayYzView");
            view->setImage(reference.permutedSpatialAxes(orthogonalAxes(true, i)));
            view->setCrosshairVisible(viewer->crosshairVisible());
            view->setScaleBarVisible(viewer->scaleBarVisible());
            views_.append(view);
            connectViewer(view, i);
        }
        spatial_ = document_->spatialPosition();
        center_ = {double(reference.width()) / 2, double(reference.height()) / 2, double(reference.depth()) / 2};
        updateNavigation(viewer->channel(), viewer->time());
        for (int i = 0; i < views_.size(); ++i) {
            const auto axes = orthogonalAxes(orthogonal_, i);
            const auto &metadata = reference.metadata().axes;
            const double aspect = metadata[axes[0]].unit == metadata[axes[1]].unit
                ? metadata[axes[1]].spacing / metadata[axes[0]].spacing : 1.0;
            views_[i]->setPixelAspectRatio(aspect);
            views_[i]->setToolTip(enabled
                ? tr("Pointer tool: click or drag to move the intersection. Middle-button or Space + drag to pan. Wheel to browse. Pinch or Ctrl+wheel to zoom.")
                : tr("Wheel to browse. Pinch or Ctrl+wheel to zoom. Middle-button or Space + drag to pan."));
            if (i > 0) {
                auto *window = static_cast<ImageWindow *>(views_[i]->window());
                window->setScreen(screen());
                window->resizeToImage(aspect);
                window->move(frameGeometry().topLeft() + QPoint(28 * i, 28 * i));
                if (isVisible()) window->show();
            }
        }
        for (auto *view : views_) view->fitToWindow();
    }
    updateLayers();
    emit visibilityChanged();
}

void OverlayWindow::showViews()
{
    showNormal();
    for (int i = 1; i < views_.size(); ++i) views_[i]->window()->showNormal();
}

void OverlayWindow::setPosition(std::size_t x, std::size_t y, std::size_t z, std::size_t channel, std::size_t time)
{
    if (!document_ || document_->image.empty() || synchronizing_) return;
    const auto &reference = document_->image;
    {
        QScopedValueRollback guard(synchronizing_, true);
        spatial_ = {std::min(x, reference.width() - 1), std::min(y, reference.height() - 1),
                    std::min(z, reference.depth() - 1)};
        const auto clampedC = std::min(channel, reference.channels() - 1);
        const auto clampedT = std::min(time, reference.frames() - 1);
        updateNavigation(clampedC, clampedT);
        document_->setPosition(spatial_[0], spatial_[1], spatial_[2], clampedC, clampedT);
    }
    updateLayers();
}

void OverlayWindow::syncLayerDisplay(DocumentWidget *document)
{
    const auto found = layerPlanes_.find(document);
    if (found == layerPlanes_.end()) return;
    for (auto &plane : found->second.planes) if (plane) {
        plane->setWindow(document->viewer->windowMinimum(), document->viewer->windowMaximum());
        plane->setGamma(document->viewer->gamma());
        plane->setInverted(document->viewer->isInverted());
        plane->setLinearInterpolation(document->viewer->linearInterpolation());
        plane->setComposite(document->viewer->composite());
    }
}

void OverlayWindow::refreshDocuments(const QList<DocumentWidget *> &documents)
{
    if (!document_) return;
    {
        QScopedValueRollback guard(synchronizing_, true);
        QSignalBlocker block(images_);
        // The reference image starts at the bottom, with new images above it.
        auto candidates = documents;
        candidates.removeAll(document_);
        candidates.append(document_);
        for (auto *document : candidates) {
            bool exists = false;
            for (int row = 0; row < images_->topLevelItemCount(); ++row)
                exists |= images_->topLevelItem(row)->data(0, Qt::UserRole).value<DocumentWidget *>() == document;
            if (exists) continue;
            auto *item = new QTreeWidgetItem;
            item->setFlags((item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsDragEnabled) & ~Qt::ItemIsDropEnabled);
            item->setData(0, Qt::UserRole, QVariant::fromValue(document));
            item->setData(0, Qt::UserRole + 1, document->viewer->lut());
            item->setData(0, Qt::UserRole + 2, document == document_ ? 100 : 50);
            item->setCheckState(0, document == document_ || document_->linkedDocuments().contains(document)
                ? Qt::Checked : Qt::Unchecked);
            if (document == document_) item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
            images_->insertTopLevelItem(document == document_ ? images_->topLevelItemCount() : 0, item);
            if (document == document_ && !images_->currentItem()) images_->setCurrentItem(item);
            connect(document, &DocumentWidget::closing, this, [this, document] { removeDocument(document); });
            connect(document, &QObject::destroyed, this, [this, document] { removeDocument(document); });
            connect(document, &DocumentWidget::changed, this, [this] { refreshEditor(); updateLayers(); });
            connect(document, &DocumentWidget::positionChanged, this, [this, document] {
                if (synchronizing_ || !document_ || !isVisible()) return;
                bool selected = false;
                for (int row = 0; row < images_->topLevelItemCount(); ++row) {
                    const auto *entry = images_->topLevelItem(row);
                    if (entry->data(0, Qt::UserRole).value<DocumentWidget *>() == document)
                        selected = entry->checkState(0) == Qt::Checked;
                }
                if (document != document_ && !selected) return;
                const auto sourcePos = document->spatialPosition();
                const std::array<double, 3> point = orthogonal_
                    ? std::array<double, 3>{double(sourcePos[0]), double(sourcePos[1]), double(sourcePos[2])}
                    : std::array<double, 3>{double(document->image.width()) / 2, double(document->image.height()) / 2,
                                            double(document->viewer->slice())};
                const auto mapped = mapImagePosition(document->image, document_->image, point);
                if (!mapped) return;
                auto target = spatial_;
                if (orthogonal_) {
                    for (std::size_t axis = 0; axis < 3; ++axis)
                        target[axis] = std::size_t(std::clamp(std::round((*mapped)[axis]), 0.0,
                            double(document_->image.dimension(axis) - 1)));
                } else {
                    target[2] = std::size_t(std::clamp(std::round((*mapped)[2]), 0.0,
                        double(document_->image.depth() - 1)));
                }
                setPosition(target[0], target[1], target[2], document->viewer->channel(), document->viewer->time());
            });
        }
    }
    refreshEditor();
    updateLayers();
}

void OverlayWindow::refreshEditor()
{
    QSignalBlocker block(images_);
    for (int row = 0; row < images_->topLevelItemCount(); ++row) {
        auto *item = images_->topLevelItem(row);
        auto *document = item->data(0, Qt::UserRole).value<DocumentWidget *>();
        item->setText(0, document->title());
        item->setToolTip(0, document->image.metadata().sourcePath);
    }
    const auto *item = images_->currentItem();
    const QSignalBlocker lutBlock(lut_), opacityBlock(opacity_), opacitySliderBlock(opacitySlider_);
    lut_->setEnabled(item != nullptr);
    opacity_->setEnabled(item != nullptr);
    opacitySlider_->setEnabled(item != nullptr);
    if (item) {
        lut_->setCurrentIndex(lut_->findData(item->data(0, Qt::UserRole + 1)));
        opacity_->setValue(item->data(0, Qt::UserRole + 2).toInt());
        opacitySlider_->setValue(opacity_->value());
    }
}

void OverlayWindow::removeDocument(DocumentWidget *document)
{
    QSignalBlocker block(images_);
    for (int row = images_->topLevelItemCount() - 1; row >= 0; --row)
        if (images_->topLevelItem(row)->data(0, Qt::UserRole).value<DocumentWidget *>() == document)
            delete images_->takeTopLevelItem(row);
    refreshEditor();
    if (document != document_) updateLayers();
    if (const auto found = layerPlanes_.find(document); found != layerPlanes_.end()) {
        disconnect(found->second.displayConnection);
        layerPlanes_.erase(found);
    }
}

void OverlayWindow::updateLayers()
{
    if (synchronizing_ || !document_ || !isVisible()) return;
    if (orthogonal_ && document_->image.depth() < 2) {
        setOrthogonalViews(false);
        return;
    }
    QScopedValueRollback guard(synchronizing_, true);
    const auto &reference = document_->image;
    const auto &refAxesMeta = reference.metadata().axes;
    const auto aspectFor = [&refAxesMeta](std::array<std::size_t, 3> axes) {
        return refAxesMeta[axes[0]].unit == refAxesMeta[axes[1]].unit
            ? refAxesMeta[axes[1]].spacing / refAxesMeta[axes[0]].spacing : 1.0;
    };
    if (revision_ != document_->revision) {
        revision_ = document_->revision;
        for (std::size_t axis = 0; axis < 3; ++axis)
            spatial_[axis] = std::min(spatial_[axis], reference.dimension(axis) - 1);
        center_ = {double(reference.width()) / 2, double(reference.height()) / 2, double(reference.depth()) / 2};
        for (int i = 0; i < views_.size(); ++i) {
            const auto scale = views_[i]->zoom();
            const auto center = views_[i]->viewCenter();
            const bool fitted = views_[i]->fitsWindow();
            const auto axes = orthogonalAxes(orthogonal_, i);
            views_[i]->setImage(orthogonal_ && i > 0 ? reference.permutedSpatialAxes(axes) : reference);
            views_[i]->setSlice(spatial_[axes[2]], document_->viewer->channel(), document_->viewer->time());
            views_[i]->setPixelAspectRatio(aspectFor(axes));
            if (fitted) {
                views_[i]->fitToWindow();
            } else {
                views_[i]->setViewTransform(scale, center);
                center_[axes[0]] = center.x();
                center_[axes[1]] = center.y();
            }
        }
    }
    for (int i = 0; i < views_.size(); ++i)
        views_[i]->setPixelAspectRatio(aspectFor(orthogonalAxes(orthogonal_, i)));
    setWindowTitle(baseTitle());
    updateNavigation(viewer->channel(), viewer->time());

    struct CheckedLayer {
        DocumentWidget *document;
        QString lut;
        double opacity;
    };
    QList<CheckedLayer> checked;
    for (int row = images_->topLevelItemCount() - 1; row >= 0; --row) {
        auto *item = images_->topLevelItem(row);
        auto *document = item->data(0, Qt::UserRole).value<DocumentWidget *>();
        if (document == document_) {
            QSignalBlocker block(images_);
            item->setCheckState(0, Qt::Checked);
        }
        if (item->checkState(0) != Qt::Checked) continue;
        checked.append({document, item->data(0, Qt::UserRole + 1).toString(),
                        item->data(0, Qt::UserRole + 2).toInt() / 100.0});
    }

    QStringList incompatible;
    const auto markIncompatible = [&incompatible](const QString &name) {
        if (!incompatible.contains(name)) incompatible.append(name);
    };
    QSet<DocumentWidget *> checkedDocuments;
    for (const auto &entry : checked) checkedDocuments.insert(entry.document);
    QSet<DocumentWidget *> propagated{document_};
    document_->setPosition(spatial_[0], spatial_[1], spatial_[2], viewer->channel(), viewer->time());
    for (const auto &linked : document_->linkedDocuments()) if (linked) propagated.insert(linked);
    const auto mappedPosition = [&](DocumentWidget *document) -> std::optional<std::array<std::size_t, 3>> {
        const std::array<double, 3> refPoint = orthogonal_
            ? std::array<double, 3>{double(spatial_[0]), double(spatial_[1]), double(spatial_[2])}
            : std::array<double, 3>{double(reference.width()) / 2, double(reference.height()) / 2, double(spatial_[2])};
        const auto mapped = mapImagePosition(reference, document->image, refPoint);
        if (!mapped) { markIncompatible(document->title()); return std::nullopt; }
        auto position = document->spatialPosition();
        if (orthogonal_) {
            for (std::size_t axis = 0; axis < 3; ++axis)
                position[axis] = std::size_t(std::clamp(std::round((*mapped)[axis]), 0.0,
                    double(document->image.dimension(axis) - 1)));
        } else {
            position[2] = std::size_t(std::clamp(std::round((*mapped)[2]), 0.0,
                double(document->image.depth() - 1)));
        }
        return position;
    };
    // Propagate once per independent linked group that has unchecked peers, then pin each checked layer without bounce-back.
    for (const auto &entry : checked) {
        if (propagated.contains(entry.document)) continue;
        const bool hasUncheckedPeer = std::any_of(entry.document->linkedDocuments().begin(),
            entry.document->linkedDocuments().end(),
            [&checkedDocuments](const auto &linked) { return linked && !checkedDocuments.contains(linked); });
        if (!hasUncheckedPeer) continue;
        if (const auto position = mappedPosition(entry.document)) {
            entry.document->setPosition((*position)[0], (*position)[1], (*position)[2], viewer->channel(), viewer->time(), true);
            propagated.insert(entry.document);
            for (const auto &linked : entry.document->linkedDocuments()) if (linked) propagated.insert(linked);
        }
    }
    for (const auto &entry : checked) {
        if (entry.document == document_) continue;
        if (const auto position = mappedPosition(entry.document))
            entry.document->setPosition((*position)[0], (*position)[1], (*position)[2], viewer->channel(), viewer->time(), false);
    }

    QSet<ImageViewer *> usedPlanes;
    for (int i = 0; i < views_.size(); ++i) {
        const auto refAxes = orthogonalAxes(orthogonal_, i);
        QList<ImageViewer::OverlayLayer> layers;
        for (const auto &entry : checked) {
            auto *document = entry.document;
            const std::array<double, 3> refPoint = orthogonal_
                ? std::array<double, 3>{double(spatial_[0]), double(spatial_[1]), double(spatial_[2])}
                : std::array<double, 3>{double(reference.width()) / 2, double(reference.height()) / 2, double(spatial_[2])};
            auto refNext = refPoint;
            refNext[refAxes[2]] += 1.0;
            const auto mapped = mapImagePosition(reference, document->image, refPoint);
            const auto mappedNext = mapImagePosition(reference, document->image, refNext);
            if (!mapped || !mappedNext) { markIncompatible(document->title()); continue; }
            std::size_t docSliceAxis = 2;
            double bestNormal = -1;
            const auto &docMeta = document->image.metadata().axes;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const double spacing = axis < docMeta.size() ? docMeta[axis].spacing : 1.0;
                const double weight = std::abs((*mappedNext)[axis] - (*mapped)[axis]) * spacing;
                if (weight > bestNormal) { bestNormal = weight; docSliceAxis = axis; }
            }
            const std::array<std::size_t, 3> docAxes = docSliceAxis == 2 ? std::array<std::size_t, 3>{0, 1, 2}
                : docSliceAxis == 1 ? std::array<std::size_t, 3>{0, 2, 1} : std::array<std::size_t, 3>{1, 2, 0};
            if (docSliceAxis != 2 && document->image.shape().size() < 3) continue;
            const double mappedPlane = (*mapped)[docSliceAxis];
            const auto planeExtent = document->image.dimension(docSliceAxis);
            if (orthogonal_ && (mappedPlane < -0.5 || mappedPlane > double(planeExtent) - 0.5)) continue;
            const auto sliceIndex = std::size_t(std::clamp(std::round(mappedPlane), 0.0,
                double(planeExtent - 1)));
            const auto docPoint = [&](double u, double v) {
                std::array<double, 3> p{};
                p[docAxes[0]] = u;
                p[docAxes[1]] = v;
                p[docAxes[2]] = double(sliceIndex);
                return p;
            };
            const auto origin = mapImagePosition(document->image, reference, docPoint(0, 0));
            const auto x = mapImagePosition(document->image, reference, docPoint(1, 0));
            const auto y = mapImagePosition(document->image, reference, docPoint(0, 1));
            if (!origin || !x || !y) { markIncompatible(document->title()); continue; }
            // Calibration describes pixel centers; the viewer draws pixel edges.
            const double xx = (*x)[refAxes[0]] - (*origin)[refAxes[0]];
            const double xy = (*x)[refAxes[1]] - (*origin)[refAxes[1]];
            const double yx = (*y)[refAxes[0]] - (*origin)[refAxes[0]];
            const double yy = (*y)[refAxes[1]] - (*origin)[refAxes[1]];
            const QTransform transform(xx, xy, yx, yy,
                (*origin)[refAxes[0]] + .5 - .5 * (xx + yx),
                (*origin)[refAxes[1]] + .5 - .5 * (xy + yy));
            ImageViewer *layerViewer = document->viewer;
            if (docSliceAxis != 2) {
                const int planeIndex = docAxes == std::array<std::size_t, 3>{0, 2, 1} ? 0 : 1;
                auto &planeEntry = layerPlanes_[document];
                const bool updated = planeEntry.revision != document->revision;
                if (updated) {
                    planeEntry.revision = document->revision;
                    for (int p = 0; p < 2; ++p) if (planeEntry.planes[p])
                        planeEntry.planes[p]->setImage(document->image.permutedSpatialAxes(orthogonalAxes(true, p + 1)), true);
                }
                if (!planeEntry.planes[planeIndex]) {
                    planeEntry.planes[planeIndex] = std::make_unique<ImageViewer>();
                    planeEntry.planes[planeIndex]->setImage(document->image.permutedSpatialAxes(docAxes));
                    if (!planeEntry.displayConnection) {
                        planeEntry.displayConnection = connect(document->viewer, &ImageViewer::displaySettingsChanged, this,
                            [this, document] { syncLayerDisplay(document); });
                    }
                }
                if (updated || planeEntry.planes[planeIndex]) syncLayerDisplay(document);
                layerViewer = planeEntry.planes[planeIndex].get();
                layerViewer->setSlice(sliceIndex, viewer->channel(), viewer->time());
                usedPlanes.insert(layerViewer);
            }
            layers.append({layerViewer, entry.lut, entry.opacity, transform});
        }
        views_[i]->setOverlayLayers(layers);
    }

    for (auto it = layerPlanes_.begin(); it != layerPlanes_.end();) {
        for (auto &plane : it->second.planes)
            if (plane && !usedPlanes.contains(plane.get())) plane.reset();
        if (!it->second.planes[0] && !it->second.planes[1]) {
            disconnect(it->second.displayConnection);
            it = layerPlanes_.erase(it);
        } else {
            ++it;
        }
    }
    if (!incompatible.isEmpty())
        emit viewError(tr("Cannot overlay images with incompatible spatial units or invalid directions: %1").arg(incompatible.join(", ")));
}

void OverlayWindow::showEditor()
{
    showViews();
    raise();
    editor_->showNormal();
    keepWindowOnScreen(editor_);
    editor_->raise();
    editor_->activateWindow();
}

bool OverlayWindow::event(QEvent *event)
{
    const bool handled = ImageWindow::event(event);
    if (event->type() == QEvent::WindowActivate) emit activated();
    return handled;
}

bool OverlayWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::WindowActivate) emit activated();
    return ImageWindow::eventFilter(watched, event);
}

void OverlayWindow::showEvent(QShowEvent *event)
{
    ImageWindow::showEvent(event);
    if (document_) {
        QScopedValueRollback guard(synchronizing_, true);
        spatial_ = document_->spatialPosition();
        updateNavigation(document_->viewer->channel(), document_->viewer->time());
    }
    updateLayers();
    for (int i = 1; i < views_.size(); ++i) views_[i]->window()->show();
    emit visibilityChanged();
}

void OverlayWindow::hideEvent(QHideEvent *event)
{
    ImageWindow::hideEvent(event);
    emit visibilityChanged();
}

void OverlayWindow::closeEvent(QCloseEvent *event)
{
    editor_->hide();
    for (int i = 1; i < views_.size(); ++i) views_[i]->window()->hide();
    event->accept();
}
} // namespace imagecxx
