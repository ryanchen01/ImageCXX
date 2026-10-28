#include "overlay_window.h"
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>

namespace imagecxx {
OverlayWindow::OverlayWindow(DocumentWidget *document, QWidget *parent)
    : ImageWindow(parent), document_(document), editor_(new QDialog(parent, Qt::Window)), revision_(document->revision)
{
    setObjectName("overlayViewWindow");
    viewer->setObjectName("overlayView");
    viewer->setTool(ImageViewer::Tool::Pan);
    viewer->setImage(document->image);
    viewer->setSlice(document->viewer->slice(), document->viewer->channel(), document->viewer->time());
    viewer->setToolTip(tr("Wheel to browse. Ctrl+wheel to zoom. Drag to pan."));
    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(14, 10, 14, 10);
    auto *label = new QLabel(tr("Z"), this);
    slice_ = new QSlider(Qt::Horizontal, this);
    slice_->setObjectName("sliceSlider");
    slice_->setAccessibleName(tr("Z slice"));
    label->setBuddy(slice_);
    position_ = new QLabel(this);
    bar->addWidget(label);
    bar->addWidget(slice_, 1);
    bar->addWidget(position_);
    static_cast<QVBoxLayout *>(layout())->addLayout(bar);

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
    lut_->addItems({"grayscale", "hot", "viridis"});
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
    connect(lut_, &QComboBox::currentTextChanged, this, [this](const QString &name) {
        if (auto *item = images_->currentItem()) item->setData(0, Qt::UserRole + 1, name);
    });
    connect(opacity_, &QSpinBox::valueChanged, this, [this](int value) {
        if (auto *item = images_->currentItem()) item->setData(0, Qt::UserRole + 2, value);
    });
    connect(opacitySlider_, &QSlider::valueChanged, opacity_, &QSpinBox::setValue);
    connect(opacity_, &QSpinBox::valueChanged, opacitySlider_, &QSlider::setValue);
    connect(slice_, &QSlider::valueChanged, this, [this](int slice) {
        viewer->setSlice(std::size_t(slice), viewer->channel(), viewer->time());
    });
    connect(viewer, &ImageViewer::sliceChanged, this, [this] {
        if (synchronizing_ || !document_) return;
        {
            QScopedValueRollback guard(synchronizing_, true);
            const auto position = document_->spatialPosition();
            document_->setPosition(position[0], position[1], viewer->slice(), viewer->channel(), viewer->time());
        }
        updateLayers();
    });
    connect(document, &DocumentWidget::closing, this, [this] {
        hide(); editor_->hide(); deleteLater();
    });
    connect(document, &QObject::destroyed, this, [this] {
        hide(); editor_->hide(); deleteLater();
    });
    connect(viewer, &ImageViewer::loadFailed, this, &OverlayWindow::viewError);
    resizeToImage();
}

OverlayWindow::~OverlayWindow()
{
    delete editor_;
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
            connect(document->viewer, &ImageViewer::sliceChanged, this, [this, document] {
                if (synchronizing_ || !document_) return;
                bool selected = false;
                for (int row = 0; row < images_->topLevelItemCount(); ++row) {
                    const auto *item = images_->topLevelItem(row);
                    if (item->data(0, Qt::UserRole).value<DocumentWidget *>() == document)
                        selected = item->checkState(0) == Qt::Checked;
                }
                if (document != document_ && !selected) return;
                const auto mapped = mapImagePosition(document->image, document_->image,
                    {double(document->image.width()) / 2, double(document->image.height()) / 2, double(document->viewer->slice())});
                if (!mapped) return;
                {
                    QScopedValueRollback guard(synchronizing_, true);
                    const auto z = std::size_t(std::clamp(std::round((*mapped)[2]), 0.0, double(document_->image.depth() - 1)));
                    viewer->setSlice(z, document->viewer->channel(), document->viewer->time());
                    const auto position = document_->spatialPosition();
                    document_->setPosition(position[0], position[1], z, viewer->channel(), viewer->time());
                }
                updateLayers();
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
        lut_->setCurrentText(item->data(0, Qt::UserRole + 1).toString());
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
}

void OverlayWindow::updateLayers()
{
    if (synchronizing_ || !document_) return;
    QScopedValueRollback guard(synchronizing_, true);
    const auto &reference = document_->image;
    if (revision_ != document_->revision) {
        revision_ = document_->revision;
        const auto scale = viewer->zoom();
        const auto center = viewer->viewCenter();
        viewer->setImage(reference);
        viewer->setSlice(document_->viewer->slice(), document_->viewer->channel(), document_->viewer->time());
        viewer->setViewTransform(scale, center);
    }
    const auto &axes = reference.metadata().axes;
    viewer->setPixelAspectRatio(axes[0].unit == axes[1].unit ? axes[1].spacing / axes[0].spacing : 1);
    {
        QSignalBlocker block(slice_);
        slice_->setRange(0, int(std::min(reference.depth() - 1, std::size_t(std::numeric_limits<int>::max()))));
        slice_->setValue(int(viewer->slice()));
        slice_->setVisible(reference.depth() > 1);
        position_->setText(tr("%1/%2").arg(viewer->slice() + 1).arg(reference.depth()));
    }
    setWindowTitle(tr("%1 · Overlay").arg(document_->title()));
    QList<ImageViewer::OverlayLayer> layers;
    QStringList incompatible;
    for (int row = images_->topLevelItemCount() - 1; row >= 0; --row) {
        auto *item = images_->topLevelItem(row);
        if (item->data(0, Qt::UserRole).value<DocumentWidget *>() == document_) {
            QSignalBlocker block(images_);
            item->setCheckState(0, Qt::Checked);
        }
        if (item->checkState(0) != Qt::Checked) continue;
        auto *document = item->data(0, Qt::UserRole).value<DocumentWidget *>();
        const auto mapped = mapImagePosition(reference, document->image,
            {double(reference.width()) / 2, double(reference.height()) / 2, double(viewer->slice())});
        if (!mapped) { incompatible.append(document->title()); continue; }
        const auto z = std::size_t(std::clamp(std::round((*mapped)[2]), 0.0, double(document->image.depth() - 1)));
        const auto origin = mapImagePosition(document->image, reference, {0, 0, double(z)});
        const auto x = mapImagePosition(document->image, reference, {1, 0, double(z)});
        const auto y = mapImagePosition(document->image, reference, {0, 1, double(z)});
        if (!origin || !x || !y) { incompatible.append(document->title()); continue; }
        // Calibration describes pixel centers; the viewer draws pixel edges.
        const double xx = (*x)[0] - (*origin)[0], xy = (*x)[1] - (*origin)[1];
        const double yx = (*y)[0] - (*origin)[0], yy = (*y)[1] - (*origin)[1];
        const QTransform transform(xx, xy, yx, yy,
            (*origin)[0] + .5 - .5 * (xx + yx), (*origin)[1] + .5 - .5 * (xy + yy));
        const auto position = document->spatialPosition();
        document->setPosition(position[0], position[1], z, viewer->channel(), viewer->time());
        layers.append({document->viewer, item->data(0, Qt::UserRole + 1).toString(),
            item->data(0, Qt::UserRole + 2).toInt() / 100.0, transform});
    }
    viewer->setOverlayLayers(layers);
    if (!incompatible.isEmpty())
        emit viewError(tr("Cannot overlay images with incompatible spatial units or invalid directions: %1").arg(incompatible.join(", ")));
}

void OverlayWindow::showEditor()
{
    showNormal();
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

void OverlayWindow::closeEvent(QCloseEvent *event)
{
    editor_->hide();
    event->accept();
}
} // namespace imagecxx
