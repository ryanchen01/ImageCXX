#include "document_widget.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QVBoxLayout>
#include <QSignalBlocker>
#include <QCloseEvent>
#include <QEvent>
#include <QMessageBox>
#include <algorithm>
#include <limits>

namespace imagecxx {
DocumentWidget::DocumentWidget(Image source, QWidget *parent) : QWidget(parent),
    image(std::move(source)), viewer(new ImageViewer(this)), z_(new QSlider(Qt::Horizontal, this)),
    channel_(new QSlider(Qt::Horizontal, this)), time_(new QSlider(Qt::Horizontal, this)),
    position_(new QLabel(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(viewer, 1);
    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(14, 10, 14, 10);
    std::size_t axisIndex = 0;
    for (auto pair : {std::pair{tr("Z"), z_}, {tr("C"), channel_}, {tr("T"), time_}}) {
        auto *label = new QLabel(pair.first, this);
        axisLabels_[axisIndex++] = label;
        bar->addWidget(label);
        bar->addWidget(pair.second, 1);
        connect(pair.second, &QSlider::valueChanged, this, [this] {
            viewer->setSlice(z_->value(), channel_->value(), time_->value());

        });
    }
    connect(viewer, &ImageViewer::sliceChanged, this, [this](std::size_t z, std::size_t c, std::size_t t) {
        const QSignalBlocker bz(z_), bc(channel_), bt(time_);
        z_->setValue(int(z)); channel_->setValue(int(c)); time_->setValue(int(t));
        position_->setText(tr("Z %1/%2  C %3/%4  T %5/%6").arg(z + 1).arg(image.depth())
            .arg(c + 1).arg(image.channels()).arg(t + 1).arg(image.frames()));
        emit changed();
    });
    bar->addWidget(position_);
    layout->addLayout(bar);
    connect(viewer, &ImageViewer::roiCreated, this, [this](Roi roi) {
        setSelection(std::move(roi));
    });
    connect(viewer, &ImageViewer::roiSelected, this, [this](const Roi &roi) { setSelection(roi); });
    connect(viewer, &ImageViewer::selectionCleared, this, [this] { setSelection({}); });
    connect(viewer, &ImageViewer::roiEdited, this, &DocumentWidget::editRoi);
    connect(this, &DocumentWidget::changed, this, [this] { setWindowTitle(title()); });
    refresh();
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
    viewer->setRois(rois);
    viewer->setSelection(selection);
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
    const auto oldZ = viewer->slice(), oldC = viewer->channel(), oldT = viewer->time();
    viewer->setImage(image, !viewer->image().empty());
    const auto clampSlider = [](QSlider *slider, std::size_t size, std::size_t value) {
        QSignalBlocker blocker(slider);
        slider->setRange(0, static_cast<int>(std::min<std::size_t>(size - 1, std::numeric_limits<int>::max())));
        slider->setValue(static_cast<int>(std::min(value, size - 1)));
        slider->setEnabled(size > 1);
    };
    clampSlider(z_, image.depth(), oldZ);
    clampSlider(channel_, image.channels(), oldC);
    clampSlider(time_, image.frames(), oldT);
    const std::array<QSlider *, 3> controls{z_, channel_, time_};
    const std::array<std::size_t, 3> lengths{image.depth(), image.channels(), image.frames()};
    for (std::size_t axis = 0; axis < controls.size(); ++axis) {
        controls[axis]->setVisible(lengths[axis] > 1); axisLabels_[axis]->setVisible(lengths[axis] > 1);
    }
    viewer->setSlice(z_->value(), channel_->value(), time_->value());
    refreshRois();
    position_->setText(tr("Z %1/%2  C %3/%4  T %5/%6").arg(z_->value() + 1)
        .arg(image.depth()).arg(channel_->value() + 1).arg(image.channels())
        .arg(time_->value() + 1).arg(image.frames()));
}

bool DocumentWidget::event(QEvent *event)
{
    const bool handled = QWidget::event(event);
    if (event->type() == QEvent::WindowActivate) emit activated();
    return handled;
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
    emit closing();
    event->accept();
}

} // namespace imagecxx
