#include "mainwindow.h"
#include "app/batch.h"
#include "app/processing_commands.h"
#include "io/image_io.h"
#include "plots/plot_widget.h"
#include "processing/processing.h"
#include "core/memory.h"
#include <dcmtk/dcmdata/dctag.h>
#include <QActionGroup>
#include <QAbstractSpinBox>
#include <QKeyEvent>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QApplication>
#include <QCheckBox>
#include <QBuffer>
#include <QClipboard>
#include <QColorDialog>
#include <QImage>
#include <QPrintDialog>
#include <QPrinter>
#include <QPainter>
#include <QCloseEvent>
#include <QComboBox>
#include <QCompleter>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QRegularExpression>
#include <QPushButton>
#include <QSaveFile>
#include <QLockFile>
#include <QScreen>
#include <QScopedValueRollback>
#include <QSettings>
#include <QSet>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QIcon>
#include <limits>
#include <stdexcept>
#include <array>
#include <QTableView>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <QFileInfo>
#include <QDir>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <tuple>

using namespace imagecxx;

namespace {
// Keep scientific pixels in process while offering PNG to other applications.
class ImageMimeData : public QMimeData {
public:
    ImageMimeData() { setData("application/x-imagecxx-image", {}); }
    std::shared_ptr<const Image> image;
};

QString propertyClipboardText(QTreeWidget *tree, int column, const QTreeWidgetItem *clicked = nullptr)
{
    const bool copySelection = !clicked || clicked->isSelected();
    QStringList lines;
    for (QTreeWidgetItemIterator it(tree, QTreeWidgetItemIterator::NotHidden); *it; ++it) {
        const auto *item = *it;
        if (copySelection ? !item->isSelected() : item != clicked) continue;
        if (column >= 0) lines.append(item->text(column));
        else {
            QStringList fields{item->text(0), item->text(1)};
            for (auto &field : fields) {
                field.replace('"', "\"\"");
                field = QLatin1Char('"') + field + QLatin1Char('"');
            }
            lines.append(fields.join(','));
        }
    }
    return lines.join('\n');
}

Image imageFromRaster(QImage raster)
{
    if (raster.isNull()) return {};
    PixelType type;
    if (raster.format() == QImage::Format_Grayscale16) type = PixelType::UInt16;
    else if (raster.isGrayscale() && !raster.hasAlphaChannel()) {
        type = PixelType::UInt8; raster = raster.convertToFormat(QImage::Format_Grayscale8);
    } else if (raster.hasAlphaChannel()) {
        type = PixelType::RGBA8; raster = raster.convertToFormat(QImage::Format_RGBA8888);
    } else {
        type = PixelType::RGB8; raster = raster.convertToFormat(QImage::Format_RGB888);
    }
    if (raster.isNull()) throw std::runtime_error("Cannot convert clipboard image");
    auto image = Image::allocate(type, {std::size_t(raster.width()), std::size_t(raster.height())});
    auto *data = image.mutableData(); const auto rowBytes = image.width() * bytesPerPixel(type);
    for (int y = 0; y < raster.height(); ++y) std::memcpy(data + y * rowBytes, raster.constScanLine(y), rowBytes);
    return image;
}

Image cropToBounds(const Image &image, const QRectF &bounds)
{
    const auto clipped = bounds.normalized().toAlignedRect().intersected(QRect(0, 0, int(image.width()), int(image.height())));
    if (clipped.isEmpty()) return {};
    std::vector<std::size_t> start(image.shape().size(), 0), extent = image.shape();
    start[0] = std::size_t(clipped.x()); start[1] = std::size_t(clipped.y());
    extent[0] = std::size_t(clipped.width()); extent[1] = std::size_t(clipped.height());
    return image.region(start, extent);
}

QImage displayRaster(const Image &image, double minimum, double maximum, std::stop_token stop = {})
{
    QImage raster(int(image.width()), int(image.height()), QImage::Format_RGBA8888);
    if (raster.isNull()) throw std::runtime_error("Cannot allocate display image");
    const auto components = componentCount(image.pixelType());
    for (int y = 0; y < raster.height(); ++y) {
        if (stop.stop_requested()) return {};
        for (int x = 0; x < raster.width(); ++x) {
            auto *pixel = raster.scanLine(y) + 4 * x;
            for (int c = 0; c < 4; ++c) {
                double value = c == 3 && components != 4 ? 255 : image.sample(x, y, 0, 0, 0, components >= 3 ? c : 0);
                if (c < 3 && components < 3) value = maximum > minimum ? 255 * (value - minimum) / (maximum - minimum) : 0;
                pixel[c] = uchar(std::clamp(std::isfinite(value) ? value : 0., 0., 255.));
            }
        }
    }
    return raster;
}
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), jobs_(nullptr, 2)
{
    setObjectName("ImageCXXMainWindow");
    setWindowTitle(tr("ImageCXX"));
    resize(600, 90);
    setMinimumWidth(560);
    setMaximumHeight(120);
    setAcceptDrops(true);
    setStyleSheet(R"(
        QMainWindow, QDialog { background: #FAF7F2; color: #1A1C1F; }
        QWidget { font-size: 12px; }
        QMenuBar, QMenu, QToolBar, QStatusBar { background: #FAF7F2; color: #1A1C1F; }
        QMenu::item:selected { background: #DED8CF; color: #1A1C1F; }
        QToolBar { spacing: 2px; padding: 4px; border-bottom: 1px solid #D8D2C9; }
        QToolButton { color: #1A1C1F; padding: 5px; border-radius: 4px; }
        QToolButton:checked, QPushButton:hover { background: #DED8CF; color: #1A1C1F; }
        QToolBar#imageTools QToolButton:enabled:hover { background: #F0ECE6; }
        QToolBar#imageTools QToolButton:enabled:pressed,
        QToolBar#imageTools QToolButton:enabled:checked:hover { background: #D4CDC3; }
        QTreeView, QListView, QTableView { background: #FAF7F2; alternate-background-color: #F0ECE6;
            color: #1A1C1F; border: 0; selection-background-color: #DED8CF; selection-color: #1A1C1F; }
        QHeaderView::section { background: #F0ECE6; color: #1A1C1F; padding: 7px; border: 0; }
        QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox { background: #FAF7F2; color: #1A1C1F;
            border: 1px solid #D8D2C9; border-radius: 4px; padding: 5px; min-height: 19px; }
        QPushButton { background: #F0ECE6; color: #1A1C1F; border: 1px solid #D8D2C9;
            border-radius: 4px; padding: 7px 12px; }
        QLabel, QCheckBox { color: #1A1C1F; }
        QToolButton:disabled, QPushButton:disabled { color: #8B8883; }
        QStatusBar QProgressBar { border: 0; background: #E8E2D9; border-radius: 3px; height: 5px; }
        QStatusBar QProgressBar::chunk { background: #1A1C1F; border-radius: 3px; }
    )");
    auto *displayPanel = new QDialog(this, Qt::Window);
    displayPanel->setWindowTitle(tr("Display"));
    displayPanel->setObjectName("displayWindow");
    displayPanel->resize(380, 360);
    panels_.insert("display", displayPanel);
    auto *form = new QFormLayout(displayPanel);
    form->setContentsMargins(12, 12, 12, 12);
    form->setSpacing(10);
    channelIndex_ = new QSpinBox(displayPanel);
    timeIndex_ = new QSpinBox(displayPanel);
    channelIndex_->setObjectName("displayChannel");
    timeIndex_->setObjectName("displayTime");
    for (auto *spin : {channelIndex_, timeIndex_}) spin->setRange(1, 1);
    channelIndex_->setAccessibleName(tr("Channel"));
    timeIndex_->setAccessibleName(tr("Time"));
    form->addRow(tr("Channel"), channelIndex_);
    form->addRow(tr("Time"), timeIndex_);
    minimum_ = new QDoubleSpinBox(displayPanel);
    maximum_ = new QDoubleSpinBox(displayPanel);
    for (auto *spin : {minimum_, maximum_}) { spin->setRange(-1e15, 1e15); spin->setDecimals(5); }
    minimum_->setObjectName("displayMinimum"); maximum_->setObjectName("displayMaximum");
    gamma_ = new QDoubleSpinBox(displayPanel);
    gamma_->setObjectName("displayGamma");
    gamma_->setRange(.05, 20); gamma_->setSingleStep(.1); gamma_->setValue(1);
    minimumSlider_ = new QSlider(Qt::Horizontal, displayPanel);
    maximumSlider_ = new QSlider(Qt::Horizontal, displayPanel);
    gammaSlider_ = new QSlider(Qt::Horizontal, displayPanel);
    minimumSlider_->setObjectName("displayMinimumSlider");
    maximumSlider_->setObjectName("displayMaximumSlider");
    gammaSlider_->setObjectName("displayGammaSlider");
    for (auto *slider : {minimumSlider_, maximumSlider_, gammaSlider_}) {
        slider->setRange(0, 1000);
        slider->setMinimumWidth(120);
    }
    maximumSlider_->setValue(1000);
    gammaSlider_->setValue(500);
    inverted_ = new QCheckBox(tr("Invert"), displayPanel);
    linear_ = new QCheckBox(tr("Linear interpolation"), displayPanel);
    composite_ = new QCheckBox(tr("Composite channels"), displayPanel);
    auto *autoRange = new QPushButton(tr("Auto range"), displayPanel);
    auto *fitButton = new QPushButton(tr("Fit to window"), displayPanel);
    const int displayValueWidth = gamma_->sizeHint().width();
    for (const auto &[label, slider, spin] : {
             std::tuple{tr("Minimum"), minimumSlider_, minimum_},
             std::tuple{tr("Maximum"), maximumSlider_, maximum_},
             std::tuple{tr("Gamma"), gammaSlider_, gamma_}}) {
        auto *row = new QHBoxLayout;
        slider->setAccessibleName(label);
        spin->setFixedWidth(displayValueWidth);
        row->addWidget(slider, 1);
        row->addWidget(spin);
        form->addRow(label, row);
    }
    form->addRow(inverted_); form->addRow(linear_); form->addRow(composite_);
    form->addRow(autoRange); form->addRow(fitButton);

    auto *propertiesPanel = new QDialog(this, Qt::Window);
    propertiesPanel->setWindowTitle(tr("Image properties"));
    propertiesPanel->setObjectName("propertiesWindow");
    propertiesPanel->resize(420, 420);
    panels_.insert("properties", propertiesPanel);
    auto *propertiesLayout = new QVBoxLayout(propertiesPanel);
    propertiesLayout->setContentsMargins(0, 0, 0, 0);
    metadataSearch_ = new QLineEdit(propertiesPanel);
    metadataSearch_->setObjectName("propertiesSearch");
    metadataSearch_->setPlaceholderText(tr("Search properties..."));
    metadataSearch_->setAccessibleName(tr("Search properties"));
    metadataSearch_->setClearButtonEnabled(true);
    auto *searchLayout = new QHBoxLayout;
    searchLayout->setContentsMargins(4, 0, 4, 0);
    searchLayout->addWidget(metadataSearch_);
    propertiesLayout->addLayout(searchLayout);
    metadata_ = new QTreeWidget(propertiesPanel);
    metadata_->setHeaderLabels({tr("Property"), tr("Value")});
    metadata_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    metadata_->setSelectionBehavior(QAbstractItemView::SelectRows);
    metadata_->setContextMenuPolicy(Qt::CustomContextMenu);
    metadata_->setAlternatingRowColors(true);
    metadata_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    propertiesLayout->addWidget(metadata_);
    connect(metadataSearch_, &QLineEdit::textChanged, this, &MainWindow::filterProperties);
    connect(metadata_, &QWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        const auto *item = metadata_->itemAt(position);
        if (!item) return;
        const bool multiple = item->isSelected() && metadata_->selectedItems().size() > 1;
        const auto rows = propertyClipboardText(metadata_, -1, item);
        const auto properties = propertyClipboardText(metadata_, 0, item);
        const auto values = propertyClipboardText(metadata_, 1, item);
        QMenu menu(metadata_);
        auto *copyRows = menu.addAction(multiple ? tr("Copy Rows") : tr("Copy Row"));
        auto *copyProperties = menu.addAction(multiple ? tr("Copy Properties") : tr("Copy Property"));
        auto *copyValues = menu.addAction(multiple ? tr("Copy Values") : tr("Copy Value"));
        const auto *action = menu.exec(metadata_->viewport()->mapToGlobal(position));
        if (action == copyRows) QApplication::clipboard()->setText(rows);
        else if (action == copyProperties) QApplication::clipboard()->setText(properties);
        else if (action == copyValues) QApplication::clipboard()->setText(values);
    });

    auto *roiPanel = new QDialog(this, Qt::Window);
    roiPanel->setWindowTitle(tr("ROI manager"));
    roiPanel->setObjectName("roiManagerWindow");
    roiPanel->resize(250, 320);
    panels_.insert("rois", roiPanel);
    auto *roiLayout = new QVBoxLayout(roiPanel);
    roiLayout->setContentsMargins(8, 8, 8, 8);
    rois_ = new QListWidget(roiPanel);
    rois_->setObjectName("roiList");
    rois_->setSelectionMode(QAbstractItemView::SingleSelection);
    rois_->setToolTip(tr("Double-click a name to rename the ROI."));
    roiLayout->addWidget(rois_);
    auto *roiButtons = new QHBoxLayout;
    auto *measureButton = new QPushButton(tr("Measure"), roiPanel);
    auto *removeRoi = new QPushButton(tr("Remove"), roiPanel);
    roiButtons->addWidget(measureButton); roiButtons->addWidget(removeRoi);
    roiLayout->addLayout(roiButtons);

    auto *historyPanel = new QDialog(this, Qt::Window);
    historyPanel->setWindowTitle(tr("History"));
    historyPanel->setObjectName("historyWindow");
    historyPanel->resize(320, 280);
    panels_.insert("history", historyPanel);
    auto *historyLayout = new QVBoxLayout(historyPanel);
    historyLayout->setContentsMargins(0, 0, 0, 0);
    history_ = new QListWidget(historyPanel);
    historyLayout->addWidget(history_);

    auto *measurementsPanel = new QDialog(this, Qt::Window);
    measurementsPanel->setWindowTitle(tr("Measurements"));
    measurementsPanel->setObjectName("measurementsWindow");
    measurementsPanel->resize(940, 300);
    panels_.insert("measurements", measurementsPanel);
    auto *measurementsLayout = new QVBoxLayout(measurementsPanel);
    measurementsLayout->setContentsMargins(0, 0, 0, 0);
    results_ = new QStandardItemModel(0, 11, this);
    results_->setHorizontalHeaderLabels({tr("Image"), tr("ROI / slice"), tr("Pixels"), tr("Area"),
        tr("Mean"), tr("Std. dev."), tr("Min"), tr("Max"), tr("Sum"), tr("Area unit"), tr("Intensity unit")});
    resultsView_ = new QTableView(measurementsPanel);
    resultsView_->setObjectName("measurementTable");
    resultsView_->setModel(results_);
    resultsView_->setAlternatingRowColors(true);
    resultsView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    resultsView_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    measurementsLayout->addWidget(resultsView_);

    pixelStatus_ = new QLabel(tr("Ready"), this);
    jobStatus_ = new QLabel(this);
    jobStatus_->setMaximumWidth(155);
    pixelStatus_->setMaximumWidth(240);
    progress_ = new QProgressBar(this);
    progress_->setFixedWidth(70); progress_->setRange(0, 1000); progress_->setTextVisible(false); progress_->hide();
    auto *cancelButton = new QPushButton(tr("Cancel"), this);
    cancelButton->setVisible(false);
    statusBar()->setSizeGripEnabled(false);
    statusBar()->addWidget(pixelStatus_, 1);
    statusBar()->addPermanentWidget(jobStatus_);
    statusBar()->addPermanentWidget(progress_);
    statusBar()->addPermanentWidget(cancelButton);
    connect(cancelButton, &QPushButton::clicked, &jobs_, &JobScheduler::cancelAll);
    pixelStatus_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    jobStatus_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    connect(&jobs_, &JobScheduler::started, this, [this](quint64, const QString &title) {
        jobStatus_->setText(title); jobStatus_->setToolTip(title); progress_->setValue(0);
    });
    connect(&jobs_, &JobScheduler::progress, this, [this](quint64, double value) { progress_->setValue(qRound(value * 1000)); });
    connect(&jobs_, &JobScheduler::busyChanged, this, [this, cancelButton](bool busy) {
        progress_->setVisible(busy); cancelButton->setVisible(busy);
        if (!busy) jobStatus_->clear();
    });

    commands_.add({"file.open", tr("Open images..."), "File", QKeySequence::Open, false, {}, [this](const QVariantMap &) {
        openPaths(QFileDialog::getOpenFileNames(this, tr("Open images"), {}, imageFileFilter()));
    }});
    commands_.add({"file.folder", tr("Open folder..."), "File", {}, false, {}, [this](const QVariantMap &) {
        const auto path = QFileDialog::getExistingDirectory(this, tr("Open image folder"));
        if (!path.isEmpty()) openPaths({path});
    }});
    commands_.add({"file.raw", tr("Import raw binary..."), "File", {}, false, {}, [this](const QVariantMap &) {
        const auto path = QFileDialog::getOpenFileName(this, tr("Import raw binary"));
        if (path.isEmpty()) return;
        importRaw(path);
    }});
    commands_.add({"file.dicomSeries", tr("Open DICOM series..."), "File", {}, false, {}, [this](const QVariantMap &) {
        openPaths(QFileDialog::getOpenFileNames(this, tr("Select DICOM files"), {}, "DICOM (*.dcm *.dicom);;All files (*)"));
    }});
    commands_.add({"file.save", tr("Save image as..."), "File", QKeySequence::Save, true, {}, [this](const QVariantMap &) {
        const QPointer<DocumentWidget> document = activeDocument(); if (!document) return;
        const auto path = QFileDialog::getSaveFileName(this, tr("Save image"), (document->image.metadata().name.isEmpty() ? tr("Untitled") : QFileInfo(document->image.metadata().name).completeBaseName()) + ".tif", imageSaveFilter());
        if (path.isEmpty() || !document) return;
        const QFileInfo destination(path);
        QString key = destination.canonicalFilePath();
        if (key.isEmpty()) key = QDir(destination.dir().canonicalPath()).filePath(destination.fileName());
        const QFileInfo reservedFile(key);
        const auto name = reservedFile.fileName().normalized(QString::NormalizationForm_C);
        const auto lockName = name.left(32) + "." + QString::number(qHash(name.toCaseFolded()), 16) + ".imagecxx-ui-save.lock";
        const auto reservation = std::make_shared<QLockFile>(reservedFile.dir().filePath(lockName));
        reservation->setStaleLockTime(0); // A large save can take longer than Qt's default 30 seconds.
        if (!reservation->tryLock(0)) {
            showError(reservation->error() == QLockFile::LockFailedError
                ? tr("This file is still being saved. Wait for that save to finish before saving it again.")
                : tr("Cannot save in this folder. Check its permissions and free space."));
            return;
        }
        const Image image = document->image; const QPointer<DocumentWidget> target = document;
        const auto revision = document->revision;
        jobs_.submit(tr("Saving image"), [image, path](auto stop, auto progress) { writeImage(image, path, stop, progress); },
            [this, target, revision, path, reservation](auto error) {
                reservation->unlock();
                if (showJobError(error)) return;
                if (target && target->revision == revision) {
                    target->modified = false; target->image.metadata().sourcePath = QFileInfo(path).absoluteFilePath();
                    target->image.metadata().name = QFileInfo(path).fileName(); emit target->changed();
                }
                statusBar()->showMessage(tr("Saved %1").arg(path), 5000);
            });
    }});
    commands_.add({"file.sample", tr("Open sample stack"), "File", {}, false, {}, [this](const QVariantMap &) {
        ImageMetadata metadata; metadata.name = "Cells · sample stack";
        metadata.axes = {{"X", .25, 0, "µm"}, {"Y", .25, 0, "µm"}, {"Z", 1, 0, "µm"}};
        metadata.intensityUnit = "a.u.";
        auto image = Image::allocate(PixelType::UInt16, {384, 320, 12}, metadata);
        auto *pixels = reinterpret_cast<std::uint16_t *>(image.mutableData());
        for (std::size_t z = 0; z < image.depth(); ++z) for (std::size_t y = 0; y < image.height(); ++y) for (std::size_t x = 0; x < image.width(); ++x) {
            double value = 120 + 50 * std::sin(x * .035) * std::cos(y * .028);
            for (int i = 0; i < 16; ++i) {
                const double dx = double(x) - (35 + (i * 83) % 320), dy = double(y) - (30 + (i * 71) % 260);
                const double radius = 9 + (i % 4) * 3;
                value += (1400 + i * 70) * std::exp(-(dx * dx + dy * dy) / (radius * radius)) * (.6 + .4 * std::cos((double(z) - i % 6) * .18));
            }
            pixels[(z * image.height() + y) * image.width() + x] = static_cast<std::uint16_t>(value);
        }
        addDocument(std::move(image));
    }});
    commands_.add({"edit.undo", tr("Undo processing"), "Edit", QKeySequence::Undo, true, {}, [this](const QVariantMap &) {
        if (auto *doc = activeDocument()) if (auto image = doc->history.undo(doc->image)) {
            doc->image = std::move(*image); doc->modified = true; ++doc->revision; doc->refresh(); updatePanels();
        }
    }});
    commands_.add({"edit.redo", tr("Redo processing"), "Edit", QKeySequence::Redo, true, {}, [this](const QVariantMap &) {
        if (auto *doc = activeDocument()) if (auto image = doc->history.redo(doc->image)) {
            doc->image = std::move(*image); doc->modified = true; ++doc->revision; doc->refresh(); updatePanels();
        }
    }});
    registerProcessingCommands(commands_, [this](const QString &id, const QVariantMap &parameters) {
        auto *document = activeDocument(); if (!document) return;
        const auto scope = parameters.value("scope").toString();
        Image input = document->image;
        const auto z = document->viewer->slice(), c = document->viewer->channel(), t = document->viewer->time();
        if (scope != "Full image") input = input.slice(z, c, t);
        if (scope == "ROI bounds") {
            const auto roi = selectedRoi();
            if (!roi || roi->type == RoiType::Line) { showError(tr("Select an area ROI first.")); return; }
            input = cropToBounds(input, roi->bounds());
            if (input.empty()) { showError(tr("The ROI is outside this image.")); return; }
        }
        auto options = processingOptions(id, parameters);
        if (options.outputSpacing.size() > 2 && input.depth() == 1) options.outputSpacing.resize(2);
        const auto revision = document->revision;
        const QPointer<DocumentWidget> target = document;
        const auto label = operationName(options.operation);
        const bool newDocument = options.operation == Operation::Resample
            || input.byteCount() > document->history.byteBudget();
        auto output = std::make_shared<Image>();
        jobs_.submit(label, [input, options, output](auto stop, auto progress) { *output = process(input, options, stop, progress); },
            [this, target, revision, scope, newDocument, label, output](auto error) {
                if (showJobError(error)) return;
                if (!target || target->revision != revision || scope != "Full image" || newDocument) {
                    output->metadata().name += " · " + label; output->metadata().sourcePath.clear();
                    auto *doc = addDocument(*output); doc->modified = true;
                } else {
                    target->replaceImage(*output, label);
                    target->image.metadata().sourcePath.clear();
                }
                updatePanels();
            });
    });
    commands_.add({"stacks.reslice", tr("Reslice..."), "Stacks", {}, true, {}, [this](const QVariantMap &) {
        const auto *document = activeDocument(); if (!document) return;
        const Image input = document->image;
        QSettings settings; settings.beginGroup("reslice/v1");
        ResliceOptions options;
        options.outputSpacing = settings.value("outputSpacing", input.metadata().axes.size() > 2 ? input.metadata().axes[2].spacing : 1).toDouble();
        if (input.depth() < 2) throw std::invalid_argument("Reslice requires a Z stack with at least two slices");
        QDialog dialog(this);
        dialog.setObjectName("resliceDialog"); dialog.setWindowTitle(tr("Reslice")); dialog.setMinimumWidth(360);
        auto *layout = new QFormLayout(&dialog);
        layout->setContentsMargins(20, 20, 20, 20); layout->setVerticalSpacing(14);
        auto *spacingLabel = new QLabel(&dialog);
        auto *spacing = new QDoubleSpinBox(&dialog);
        spacing->setObjectName("resliceSpacing"); spacing->setDecimals(6); spacing->setRange(.000001, 1e9);
        spacing->setValue(options.outputSpacing); spacing->setMinimumWidth(130);
        spacing->setToolTip(tr("Distance between output planes in the swept source axis's units."));
        layout->addRow(spacingLabel, spacing);
        auto *start = new QComboBox(&dialog); start->setObjectName("resliceStart");
        start->addItems({tr("Top"), tr("Left"), tr("Bottom"), tr("Right")});
        start->setCurrentIndex(std::clamp(settings.value("start", 0).toInt(), 0, 3));
        layout->addRow(tr("Start at:"), start);
        auto *flip = new QCheckBox(tr("Flip vertically"), &dialog); flip->setObjectName("resliceFlip");
        auto *rotate = new QCheckBox(tr("Rotate 90 degrees"), &dialog); rotate->setObjectName("resliceRotate");
        auto *avoid = new QCheckBox(tr("Avoid interpolation"), &dialog); avoid->setObjectName("resliceAvoidInterpolation");
        flip->setChecked(settings.value("flipVertically", false).toBool());
        rotate->setChecked(settings.value("rotate90", false).toBool());
        avoid->setChecked(settings.value("avoidInterpolation", false).toBool());
        layout->addRow(flip); layout->addRow(rotate); layout->addRow(avoid);
        auto *hint = new QLabel(tr("(use 1 pixel spacing)"), &dialog); layout->addRow(hint);
        const auto &axes = input.metadata().axes;
        layout->addRow(new QLabel(tr("Voxel size: %1 × %2 × %3 %4").arg(axes[0].spacing, 0, 'g', 6)
            .arg(axes[1].spacing, 0, 'g', 6).arg(axes[2].spacing, 0, 'g', 6).arg(axes[0].unit), &dialog));
        auto *size = new QLabel(&dialog); size->setObjectName("resliceOutputSize"); size->setWordWrap(true);
        layout->addRow(size);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Help, &dialog);
        for (auto *button : buttons->buttons()) button->setIcon(QIcon{});
        layout->addRow(buttons);
        const auto update = [&] {
            options.start = static_cast<ResliceStart>(start->currentIndex());
            options.outputSpacing = spacing->value(); options.flipVertically = flip->isChecked();
            options.rotate90 = rotate->isChecked(); options.avoidInterpolation = avoid->isChecked();
            spacing->setEnabled(!options.avoidInterpolation);
            const auto sweep = start->currentIndex() % 2 == 0 ? 1 : 0;
            spacingLabel->setText(tr("Output spacing (%1):").arg(options.avoidInterpolation ? tr("pixels") : axes[sweep].unit));
            try {
                const auto geometry = resliceGeometry(input, options);
                const auto available = detail::availableMemoryBytes();
                QStringList dimensions; for (auto length : geometry.shape) dimensions << QString::number(length);
                size->setText(tr("Output size: %1 MiB\n%2\nAvailable memory: %3 MiB").arg(double(geometry.byteCount) / (1024 * 1024), 0, 'f', 2)
                    .arg(dimensions.join(QStringLiteral(" × "))).arg(double(available.value_or(0)) / (1024 * 1024), 0, 'f', 0));
                size->setToolTip(tr("Estimated output and working memory: %1 MiB. 10% of available memory is kept free. Memory is checked again when processing starts.")
                    .arg(double(geometry.requiredMemoryBytes) / (1024 * 1024), 0, 'f', 2));
                detail::checkMemoryBudget(geometry.requiredMemoryBytes, available);
                buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
            } catch (const std::exception &error) {
                size->setText(QString::fromUtf8(error.what())); buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
            }
        };
        connect(spacing, &QDoubleSpinBox::valueChanged, &dialog, update);
        connect(start, &QComboBox::currentIndexChanged, &dialog, update);
        connect(flip, &QCheckBox::toggled, &dialog, update);
        connect(rotate, &QCheckBox::toggled, &dialog, update);
        double interpolatedSpacing = spacing->value();
        if (avoid->isChecked()) spacing->setValue(1);
        connect(avoid, &QCheckBox::toggled, &dialog, [&](bool checked) {
            const QSignalBlocker blocker(spacing);
            if (checked) { interpolatedSpacing = spacing->value(); spacing->setValue(1); }
            else spacing->setValue(interpolatedSpacing);
            update();
        });
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::helpRequested, &dialog, [&dialog] {
            QMessageBox::information(&dialog, tr("Reslice help"),
                tr("Top and Bottom create XZ planes; Left and Right create YZ planes through the full image. "
                   "Output spacing is the distance between those planes. Z is interpolated at the profile axis's pixel spacing.\n\n"
                   "Flip vertically reverses the source Z order. Rotate exchanges the two plane axes, as in ImageJ. "
                   "Avoid interpolation copies source pixels exactly and keeps their original spacing.\n\n"
                   "Channels and time points are kept separately. The result opens in a new image window."));
        });
        update();
        spacing->selectAll(); spacing->setFocus();
        if (dialog.exec() != QDialog::Accepted) return;
        settings.setValue("outputSpacing", options.avoidInterpolation ? interpolatedSpacing : options.outputSpacing);
        settings.setValue("start", start->currentIndex());
        settings.setValue("flipVertically", options.flipVertically); settings.setValue("rotate90", options.rotate90);
        settings.setValue("avoidInterpolation", options.avoidInterpolation); settings.sync();
        auto output = std::make_shared<Image>();
        jobs_.submit(tr("Reslice"), [input, options, output](auto stop, auto progress) {
            *output = reslice(input, options, stop, progress);
        }, [this, output](auto error) {
            if (showJobError(error)) return;
            addDocument(*output)->modified = true; updatePanels();
        });
    }});
    commands_.add({"stacks.project3d", tr("3D Project..."), "Stacks", {}, true, {}, [this](const QVariantMap &) {
        const auto *document = activeDocument(); if (!document) return;
        const Image input = document->image;
        QSettings settings; settings.beginGroup("projection3d/v1");
        settings.remove("lower"); settings.remove("upper");
        ProjectionOptions options;
        QDialog dialog(this); dialog.setObjectName("projection3dDialog"); dialog.setWindowTitle(tr("3D Projection"));
        auto *layout = new QFormLayout(&dialog);
        layout->setContentsMargins(20, 20, 20, 20); layout->setVerticalSpacing(10);
        auto *method = new QComboBox(&dialog); method->setObjectName("projectionMethod");
        method->setFixedWidth(165);
        method->addItems({tr("Nearest Point"), tr("Brightest Point"), tr("Mean Value")});
        method->setCurrentIndex(std::clamp(settings.value("method", 1).toInt(), 0, 2));
        layout->addRow(tr("Projection method:"), method);
        auto *axis = new QComboBox(&dialog); axis->setObjectName("projectionAxis");
        axis->setFixedWidth(165);
        axis->addItems({tr("X-Axis"), tr("Y-Axis"), tr("Z-Axis")});
        axis->setCurrentIndex(std::clamp(settings.value("axis", 1).toInt(), 0, 2));
        layout->addRow(tr("Axis of rotation:"), axis);
        QMap<QString, QDoubleSpinBox *> numbers;
        const auto number = [&](const QString &key, const QString &label, double value, double low, double high, int decimals = 0) {
            auto *spin = new QDoubleSpinBox(&dialog); spin->setObjectName(key);
            spin->setDecimals(decimals); spin->setRange(low, high); spin->setFixedWidth(165);
            spin->findChild<QLineEdit *>()->setTextMargins(6, 0, 0, 0);
            spin->setValue(settings.value(key, value).toDouble()); layout->addRow(label, spin); numbers.insert(key, spin);
            return spin;
        };
        const auto &axes = input.metadata().axes;
        auto *spacing = number("sliceSpacing", tr("Slice spacing (pixels):"), axes.size() > 2 ? axes[2].spacing / axes[0].spacing : 1, .000001, 1e9, 6);
        auto *initial = number("initialAngle", tr("Initial angle (0-359 degrees):"), 0, 0, 359);
        auto *total = number("totalRotation", tr("Total rotation (0-360 degrees):"), 360, 0, 360);
        auto *increment = number("angleIncrement", tr("Rotation angle increment:"), 10, 1, 360);
        const auto range = std::make_shared<std::array<double, 2>>(std::array{
            std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()});
        auto *lower = new QLineEdit(&dialog); lower->setObjectName("lower");
        auto *upper = new QLineEdit(&dialog); upper->setObjectName("upper");
        for (auto *bound : {lower, upper}) { bound->setFixedWidth(165); bound->setEnabled(false); }
        auto *lowerSlider = new QSlider(Qt::Horizontal, &dialog); lowerSlider->setObjectName("projectionLowerSlider");
        auto *upperSlider = new QSlider(Qt::Horizontal, &dialog); upperSlider->setObjectName("projectionUpperSlider");
        for (const auto &[label, slider, bound] : {
                 std::tuple{tr("Lower transparency bound:"), lowerSlider, lower},
                 std::tuple{tr("Upper transparency bound:"), upperSlider, upper}}) {
            slider->setAccessibleName(label); slider->setRange(0, 1000); slider->setFixedWidth(165); slider->setEnabled(false);
            layout->addRow(label, bound); layout->addRow(QString{}, slider);
            connect(slider, &QSlider::valueChanged, &dialog, [range, bound](int value) {
                bound->setText(QString::number(std::lerp((*range)[0], (*range)[1], value / 1000.),
                    'g', std::numeric_limits<double>::max_digits10));
            });
        }
        lower->setToolTip(tr("Inclusive bounds in raw pixel values. Values outside this range are transparent."));
        upper->setToolTip(lower->toolTip());
        auto *opacity = number("opacity", tr("Opacity (0-100%):"), 0, 0, 100);
        auto *surface = number("surfaceDepthCue", tr("Surface depth-cueing (0-100%):"), 100, 0, 100);
        auto *interior = number("interiorDepthCue", tr("Interior depth-cueing (0-100%):"), 50, 0, 100);
        auto *interpolate = new QCheckBox(tr("Interpolate"), &dialog); interpolate->setObjectName("projectionInterpolate");
        interpolate->setChecked(settings.value("interpolate", false).toBool()); layout->addRow(interpolate);
        auto *size = new QLabel(&dialog); size->setObjectName("projectionOutputSize"); size->setWordWrap(true); layout->addRow(size);
        size->setText(tr("Reading image intensity range..."));
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Help, &dialog);
        for (auto *button : buttons->buttons()) button->setIcon(QIcon{});
        layout->addRow(buttons);
        const auto update = [&] {
            if (!lower->isEnabled()) {
                buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
                return;
            }
            options.method = static_cast<ProjectionMethod>(method->currentIndex()); options.rotationAxis = axis->currentIndex();
            options.sliceSpacing = spacing->value(); options.initialAngle = int(initial->value());
            options.totalRotation = int(total->value()); options.angleIncrement = int(increment->value());
            bool lowerValid = false, upperValid = false;
            options.lower = lower->text().toDouble(&lowerValid); options.upper = upper->text().toDouble(&upperValid);
            for (const auto &[slider, value, valid] : {
                     std::tuple{lowerSlider, options.lower, lowerValid}, std::tuple{upperSlider, options.upper, upperValid}}) {
                const QSignalBlocker blocker(slider);
                const bool hasRange = (*range)[1] > (*range)[0];
                slider->setEnabled(hasRange);
                if (!valid || !std::isfinite(value)) continue;
                const double clamped = std::clamp(value, (*range)[0], (*range)[1]);
                const double span = (*range)[1] - (*range)[0];
                const double fraction = !hasRange ? 0 : std::isfinite(span) ? (clamped - (*range)[0]) / span
                    : (clamped / 2 - (*range)[0] / 2) / ((*range)[1] / 2 - (*range)[0] / 2);
                slider->setValue(int(std::lround(fraction * 1000)));
            }
            options.opacity = int(opacity->value());
            options.surfaceDepthCue = int(surface->value()); options.interiorDepthCue = int(interior->value());
            options.interpolate = interpolate->isChecked();
            try {
                if (!lowerValid || !upperValid) throw std::invalid_argument("Enter numeric transparency bounds");
                const auto geometry = projectionGeometry(input, options);
                detail::checkMemoryBudget(geometry.requiredMemoryBytes, detail::availableMemoryBytes());
                size->setText(tr("Output: %1 × %2 × %3 angles, %4 MiB").arg(geometry.shape[0]).arg(geometry.shape[1])
                    .arg(geometry.shape[2]).arg(double(geometry.byteCount) / (1024 * 1024), 0, 'f', 2));
                buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
            } catch (const std::exception &error) {
                size->setText(QString::fromUtf8(error.what())); buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
            }
        };
        connect(lower, &QLineEdit::textChanged, &dialog, update);
        connect(upper, &QLineEdit::textChanged, &dialog, update);
        for (auto *spin : numbers) connect(spin, &QDoubleSpinBox::valueChanged, &dialog, update);
        connect(method, &QComboBox::currentIndexChanged, &dialog, update);
        connect(axis, &QComboBox::currentIndexChanged, &dialog, update);
        connect(interpolate, &QCheckBox::toggled, &dialog, update);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::helpRequested, &dialog, [&dialog] {
            QMessageBox::information(&dialog, tr("3D Project help"),
                tr("Projects the full Z stack at each rotation angle. A full turn omits the duplicate 360-degree view.\n\n"
                   "Nearest Point shows the front surface; Brightest Point selects the highest raw value; Mean Value averages visible samples. "
                   "Opacity blends the front surface with the selected projection. Depth cueing darkens more distant points. "
                   "Interpolate uses linear sampling between voxels; otherwise sampling uses the nearest voxel.\n\n"
                   "Bounds start at the image's minimum and maximum finite raw values and are reset each time this dialog opens. "
                   "Output keeps the pixel type, channels and time points, with pixel coordinates and an angle axis. "
                   "The result opens in a new image window."));
        });
        update();
        std::stop_source rangeCancellation;
        connect(&dialog, &QDialog::finished, &dialog, [rangeCancellation]() mutable { rangeCancellation.request_stop(); });
        jobs_.submit(tr("Reading projection intensity range"), [input, range, rangeCancellation](auto stop, auto progress) {
            std::size_t rows = 0;
            const auto totalRows = input.height() * input.depth() * input.channels() * input.frames();
            for (std::size_t t = 0; t < input.frames(); ++t) for (std::size_t c = 0; c < input.channels(); ++c)
                for (std::size_t z = 0; z < input.depth(); ++z) for (std::size_t y = 0; y < input.height(); ++y) {
                    if (stop.stop_requested() || rangeCancellation.stop_requested())
                        throw std::runtime_error("Intensity range calculation cancelled");
                    for (std::size_t x = 0; x < input.width(); ++x)
                        for (std::size_t k = 0; k < componentCount(input.pixelType()); ++k) {
                            const double value = input.sample(x, y, z, c, t, k);
                            if (!std::isfinite(value)) continue;
                            (*range)[0] = std::min((*range)[0], value); (*range)[1] = std::max((*range)[1], value);
                        }
                    progress(double(++rows) / totalRows);
                }
            if (!std::isfinite((*range)[0])) throw std::invalid_argument("The image has no finite intensity values");
        }, [guard = QPointer<QDialog>(&dialog), range, lower, upper, size, update](auto error) {
            if (!guard) return;
            if (error) {
                try { std::rethrow_exception(error); }
                catch (const std::exception &exception) { size->setText(QString::fromUtf8(exception.what())); }
                return;
            }
            const QSignalBlocker blockLower(lower), blockUpper(upper);
            lower->setText(QString::number((*range)[0], 'g', std::numeric_limits<double>::max_digits10));
            upper->setText(QString::number((*range)[1], 'g', std::numeric_limits<double>::max_digits10));
            lower->setEnabled(true); upper->setEnabled(true); update();
        });
        if (dialog.exec() != QDialog::Accepted) return;
        for (auto it = numbers.cbegin(); it != numbers.cend(); ++it) settings.setValue(it.key(), it.value()->value());
        settings.setValue("method", method->currentIndex()); settings.setValue("axis", axis->currentIndex());
        settings.setValue("interpolate", options.interpolate); settings.sync();
        auto output = std::make_shared<Image>();
        jobs_.submit(tr("3D Project"), [input, options, output](auto stop, auto progress) {
            *output = project3D(input, options, stop, progress);
        }, [this, output](auto error) {
            if (showJobError(error)) return;
            addDocument(*output)->modified = true; updatePanels();
        });
    }});
    commands_.add({"analyze.measure", tr("Measure selection"), "Analyze", QKeySequence("M"), true, {}, [this](const QVariantMap &) {
        const auto *doc = activeDocument(); if (!doc) return;
        const auto slice = doc->image.slice(doc->viewer->slice(), doc->viewer->channel(), doc->viewer->time());
        const auto roi = selectedRoi();
        const QString location = roi ? roi->name : tr("Slice Z%1 C%2 T%3").arg(doc->viewer->slice() + 1).arg(doc->viewer->channel() + 1).arg(doc->viewer->time() + 1);
        auto output = std::make_shared<Measurement>();
        jobs_.submit(tr("Measuring"), [slice, roi, output](auto stop, auto) { *output = measure(slice, roi, true, stop); },
            [this, output, location](auto error) {
                if (showJobError(error)) return;
                const auto &m = *output;
                QList<QStandardItem *> row;
                const QList<QVariant> values{m.imageName, location, QVariant::fromValue<qulonglong>(m.count), m.area, m.mean,
                    m.standardDeviation, m.minimum, m.maximum, m.sum, m.areaUnit, m.intensityUnit};
                for (const auto &value : values) { auto *item = new QStandardItem; item->setData(value, Qt::DisplayRole); row.append(item); }
                results_->appendRow(row); showPanel("measurements");
            });
    }});
    commands_.add({"analyze.histogram", tr("Histogram"), "Analyze", QKeySequence("H"), true, {}, [this](const QVariantMap &) { showPlot(false); }});
    commands_.add({"analyze.profile", tr("Line profile"), "Analyze", QKeySequence("K"), true, {}, [this](const QVariantMap &) { showPlot(true); }});
    commands_.add({"analyze.mask", tr("Create mask from ROI"), "Analyze", {}, true, {}, [this](const QVariantMap &) {
        auto *doc = activeDocument(); const auto roi = selectedRoi();
        if (!doc || !roi || roi->type == RoiType::Line) { showError(tr("Select an area ROI first.")); return; }
        const auto slice = doc->image.slice(doc->viewer->slice(), doc->viewer->channel(), doc->viewer->time());
        auto output = std::make_shared<Mask>(); const QPointer<DocumentWidget> target = doc;
        const auto revision = doc->revision;
        const auto z = doc->viewer->slice(), c = doc->viewer->channel(), t = doc->viewer->time();
        jobs_.submit(tr("Creating mask"), [slice, roi, output](auto stop, auto) { *output = createMask(slice, *roi, stop); },
            [this, target, revision, z, c, t, output](auto error) {
                if (showJobError(error)) return;
                if (target && target->revision == revision && target->viewer->slice() == z
                    && target->viewer->channel() == c && target->viewer->time() == t) target->viewer->setMask(output->pixels);
                output->pixels.metadata().name = output->name; output->pixels.metadata().sourcePath.clear();
                auto *mask = addDocument(output->pixels); mask->modified = true; updatePanels();
            });
    }});
    commands_.add({"results.export", tr("Export measurements as CSV..."), "Analyze", {}, false, {}, [this](const QVariantMap &) {
        const auto path = QFileDialog::getSaveFileName(this, tr("Export measurements"), "measurements.csv", "CSV (*.csv)");
        if (path.isEmpty()) return;
        const int rowCount = results_->rowCount();
        QByteArray bytes;
        for (int row = -1; row < rowCount; ++row) {
            for (int column = 0; column < results_->columnCount(); ++column) {
                QString text = row < 0 ? results_->headerData(column, Qt::Horizontal).toString() : results_->data(results_->index(row, column)).toString();
                text.replace('"', "\"\""); if (column) bytes += ','; bytes += '"' + text.toUtf8() + '"';
            }
            bytes += '\n';
        }
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            showError(tr("Cannot write measurements.")); return;
        }
        exportedMeasurementRows_ = rowCount;
    }});
    commands_.add({"process.batch", tr("Batch process files..."), "Process", {}, false, {}, [this](const QVariantMap &) {
        QStringList labels, ids;
        CommandRegistry processing;
        registerProcessingCommands(processing, [](const QString &, const QVariantMap &) {});
        for (const auto &command : processing.commands()) { labels << command.label; ids << command.id; }
        bool ok; const auto label = QInputDialog::getItem(this, tr("Batch processing"), tr("Operation"), labels, 0, false, &ok);
        if (!ok) return;
        const auto id = ids[labels.indexOf(label)]; const auto &command = commands_.command(id);
        const auto parameters = parameterForm(command, true); if (!parameters) return;
        const auto inputs = QFileDialog::getOpenFileNames(this, tr("Select batch input files"), {}, imageFileFilter()); if (inputs.isEmpty()) return;
        const auto directory = QFileDialog::getExistingDirectory(this, tr("Select output directory")); if (directory.isEmpty()) return;
        auto output = std::make_shared<BatchResult>();
        jobs_.submit(tr("Batch processing"), [inputs, directory, id, parameters, output](auto stop, auto progress) { *output = runBatch(inputs, directory, id, *parameters, stop, progress); },
            [this, output](auto error) {
                if (showJobError(error)) return;
                if (!output->errors.isEmpty()) showError(tr("Saved %1 outputs.\n\n%2").arg(output->outputs.size()).arg(output->errors.join('\n')));
                else statusBar()->showMessage(tr("Batch complete: %1 files saved.").arg(output->outputs.size()), 10000);
            });
    }});

    for (const bool cut : {false, true}) {
        commands_.add({cut ? "edit.cut" : "edit.copy", cut ? tr("Cut") : tr("Copy"), "Edit",
            QKeySequence(cut ? "X" : "C"), true, {}, [this, cut](const QVariantMap &) {
            const auto *doc = activeDocument(); if (!doc) return;
            auto image = doc->image.slice(doc->viewer->slice(), doc->viewer->channel(), doc->viewer->time());
            if (const auto roi = selectedRoi()) {
                image = cropToBounds(image, roi->bounds());
                if (image.empty()) return;
            }
            auto mime = std::make_unique<ImageMimeData>();
            mime->image = std::make_shared<Image>(image.clone());
            const std::weak_ptr<const Image> snapshot = mime->image;
            const auto minimum = doc->viewer->windowMinimum(), maximum = doc->viewer->windowMaximum();
            const QPointer<ImageMimeData> pending = mime.get();
            std::stop_source cancellation;
            connect(mime.get(), &QObject::destroyed, this, [cancellation]() mutable { cancellation.request_stop(); });
            QApplication::clipboard()->setMimeData(mime.release());
            auto output = std::make_shared<std::pair<QImage, QByteArray>>();
            jobs_.submit(tr("Preparing clipboard PNG"), [snapshot, minimum, maximum, output, cancellation](auto stop, auto progress) mutable {
                std::stop_callback cancelled(stop, [cancellation]() mutable { cancellation.request_stop(); });
                const auto token = cancellation.get_token();
                const auto held = snapshot.lock();
                if (!held || token.stop_requested()) return;
                const auto &image = *held;
                // Include the source raster, scaled raster, encoded PNG capacity and codec working buffers.
                const long double scale = std::max(1.L, 320.L / std::max(image.width(), image.height()));
                const auto width = std::ceil(image.width() * scale), height = std::ceil(image.height() * scale);
                const auto required = 4.L * image.width() * image.height() + 12.L * width * height + 8 * 1024 * 1024;
                constexpr std::size_t exportBudget = 512 * 1024 * 1024;
                if (width > std::numeric_limits<int>::max() || height > std::numeric_limits<int>::max() || required > exportBudget)
                    throw std::length_error("PNG export exceeds the clipboard memory limit.");
                detail::checkMemoryBudget(std::size_t(required), detail::availableMemoryBytes());
                auto &[raster, bytes] = *output;
                raster = displayRaster(image, minimum, maximum, token);
                if (token.stop_requested()) return;
                progress(.5);
                if (raster.width() < 320 && raster.height() < 320)
                    raster = raster.scaled(QSize(320, 320), Qt::KeepAspectRatio, Qt::FastTransformation);
                if (token.stop_requested()) return;
                QBuffer png(&bytes);
                if (!png.open(QIODevice::WriteOnly) || !raster.save(&png, "PNG"))
                    throw std::runtime_error("Cannot encode clipboard PNG");
                progress(1);
            }, [this, pending, output, cancellation](auto error) {
                if (!pending || cancellation.stop_requested() || QApplication::clipboard()->mimeData() != pending.data()) return;
                if (error) {
                    QString reason;
                    try { std::rethrow_exception(error); }
                    catch (const std::exception &exception) { reason = QString::fromUtf8(exception.what()); }
                    catch (...) { reason = tr("Unknown error."); }
                    statusBar()->showMessage(tr("Image copied internally. PNG export unavailable: %1").arg(reason), 15000);
                    return;
                }
                auto ready = std::make_unique<ImageMimeData>();
                ready->image = pending->image;
                ready->setData("image/png", output->second);
                ready->setImageData(output->first);
                QApplication::clipboard()->setMimeData(ready.release());
            });
            if (cut) executeCommand("edit.clear");
        }});
    }
    commands_.add({"edit.paste", tr("Paste"), "Edit", QKeySequence("V"), false, {}, [this](const QVariantMap &) {
        pasteImage(false, false);
    }});
    commands_.add({"edit.pasteSpecial", tr("Paste special..."), "Edit", {}, false,
        {{"source", tr("Source"), ParameterKind::Choice, "Automatic", 0, 0, {"Automatic", "System image"}},
         {"destination", tr("Destination"), ParameterKind::Choice, "Current image", 0, 0, {"Current image", "New image"}}},
        [this](const QVariantMap &parameters) {
            pasteImage(parameters["source"].toString() == "System image", parameters["destination"].toString() == "New image");
    }});
    for (const bool capture : {false, true}) {
        commands_.add({capture ? "file.capture" : "file.clipboard", capture ? tr("Capture screen") : tr("New image from clipboard"),
            "File", QKeySequence(capture ? "Shift+G" : "Shift+V"), false, {}, [this, capture](const QVariantMap &) {
            if (!capture) { pasteImage(true, true); return; }
            auto image = imageFromRaster(screen()->grabWindow(0).toImage());
            if (image.empty()) return;
            image.metadata().name = tr("Screen");
            addDocument(std::move(image))->modified = true; updatePanels();
        }});
    }
    commands_.add({"file.print", tr("Print..."), "File", QKeySequence("P"), true, {}, [this](const QVariantMap &) {
        const auto *doc = activeDocument(); if (!doc) return;
        const auto raster = displayRaster(doc->image.slice(doc->viewer->slice(), doc->viewer->channel(), doc->viewer->time()),
                                          doc->viewer->windowMinimum(), doc->viewer->windowMaximum());
        QPrinter printer(QPrinter::HighResolution); QPrintDialog dialog(&printer, this);
        if (dialog.exec() != QDialog::Accepted) return;
        QPainter painter(&printer);
        const auto size = raster.size().scaled(painter.viewport().size(), Qt::KeepAspectRatio);
        painter.drawImage(QRect(QPoint(0, 0), size), raster);
    }});
    commands_.add({"file.text", tr("New text window"), "File", QKeySequence("Shift+N"), false, {}, [this](const QVariantMap &) {
        auto *dialog = new QDialog(this, Qt::Window); dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(tr("Text")); dialog->resize(600, 420);
        auto *layout = new QVBoxLayout(dialog); layout->addWidget(new QPlainTextEdit(dialog)); dialog->show();
    }});
    commands_.add({"file.next", tr("Open next image"), "File", QKeySequence("Shift+O"), true, {}, [this](const QVariantMap &) {
        const auto *doc = activeDocument(); if (!doc || doc->image.metadata().sourcePath.isEmpty()) return;
        const QFileInfo source(doc->image.metadata().sourcePath);
        const auto paths = source.dir().entryInfoList({"*.tif", "*.tiff", "*.png", "*.jpg", "*.jpeg", "*.bmp", "*.gif",
            "*.dcm", "*.dicom", "*.mha", "*.mhd", "*.nii", "*.nii.gz", "*.nrrd"}, QDir::Files, QDir::Name | QDir::IgnoreCase);
        for (qsizetype i = 0; i < paths.size(); ++i) if (paths[i].absoluteFilePath() == source.absoluteFilePath()) {
            const bool previous = QApplication::keyboardModifiers().testFlag(Qt::AltModifier);
            const auto next = i + (previous ? -1 : 1);
            if (next >= 0 && next < paths.size()) openPaths({paths[next].absoluteFilePath()});
            break;
        }
    }});
    commands_.add({"file.revert", tr("Revert"), "File", QKeySequence("R"), true, {}, [this](const QVariantMap &) {
        auto *doc = activeDocument(); if (!doc || doc->image.metadata().sourcePath.isEmpty()) return;
        if (doc->modified && !QApplication::keyboardModifiers().testFlag(Qt::AltModifier) && QMessageBox::question(doc, tr("Revert"), tr("Discard pixel changes and reload the saved image?")) != QMessageBox::Yes) return;
        const auto path = doc->image.metadata().sourcePath; const auto revision = doc->revision;
        const QPointer<DocumentWidget> target = doc; auto output = std::make_shared<Image>();
        jobs_.submit(tr("Reverting"), [path, output](auto stop, auto progress) { *output = readImage(path, {}, stop, progress); },
            [this, target, revision, output](auto error) {
                if (showJobError(error)) return;
                if (target && target->revision == revision) {
                    target->replaceImage(*output, tr("Revert")); target->modified = false; updatePanels();
                }
            });
    }});
    auto *animation = new QTimer(this); animation->setInterval(100);
    connect(animation, &QTimer::timeout, this, [this, animation] {
        auto *doc = activeDocument(); if (!doc) { animation->stop(); return; }
        auto *viewer = doc->viewer;
        if (doc->image.frames() > 1) viewer->setSlice(viewer->slice(), viewer->channel(), (viewer->time() + 1) % doc->image.frames());
        else viewer->setSlice((viewer->slice() + 1) % doc->image.depth(), viewer->channel(), viewer->time());
    });
    commands_.add({"image.animate", tr("Start/stop animation"), "View", QKeySequence(Qt::Key_Backslash), true, {}, [animation](const QVariantMap &) {
        if (animation->isActive()) animation->stop(); else animation->start();
    }});
    commands_.add({"edit.color", tr("Drawing color..."), "Edit", {}, false, {}, [this](const QVariantMap &) {
        const auto color = QColorDialog::getColor(drawingColor_, this, tr("Drawing color"));
        if (!color.isValid()) return;
        drawingColor_ = color;
        QPixmap swatch(24, 24); swatch.fill(color);
        findChild<QAction *>("edit.color")->setIcon(QIcon(swatch));
    }});
    for (const auto &operation : {QString("clear"), QString("fill"), QString("draw"), QString("invert"), QString("smooth")}) {
        const QMap<QString, QString> labels{{"clear", tr("Clear")}, {"fill", tr("Fill")}, {"draw", tr("Draw")},
            {"invert", tr("Invert pixels")}, {"smooth", tr("Smooth")}};
        commands_.add({"edit." + operation, labels.value(operation), "Edit", {}, true, {}, [this, operation](const QVariantMap &) {
            auto *doc = activeDocument(); if (!doc) return;
            const auto roi = selectedRoi();
            if (operation == "draw" && !roi) { showError(tr("Select an area ROI first.")); return; }
            if (doc->image.shape().size() > 5) { showError(tr("Pixel editing supports up to five dimensions.")); return; }
            if (roi && roi->type == RoiType::Line) { showError(tr("Select an area ROI first.")); return; }
            const auto input = doc->image;
            const auto z = doc->viewer->slice(), c = doc->viewer->channel(), t = doc->viewer->time();
            const auto revision = doc->revision; const QPointer<DocumentWidget> target = doc;
            const auto color = drawingColor_;
            auto output = std::make_shared<Image>();
            jobs_.submit(operation, [input, output, roi, z, c, t, operation, color](auto stop, auto progress) {
                *output = input.clone();
                const auto type = input.pixelType();
                const bool painting = operation == "fill" || operation == "draw";
                double maximum = type == PixelType::UInt16 ? 65535 : type == PixelType::Int16 ? 32767 : 255;
                double minimum = type == PixelType::Int16 ? -32768 : 0;
                if ((operation == "invert" || painting) && (type == PixelType::Float32 || type == PixelType::Float64)) {
                    const auto stats = measure(input.slice(z, c, t), {}, false, stop); minimum = stats.minimum; maximum = stats.maximum;
                    if (painting && !stats.count) throw std::runtime_error("Cannot draw without a finite intensity range.");
                }
                const double gray = std::lerp(minimum, maximum, qGray(color.rgb()) / 255.0);
                const std::array<double, 3> rgb{double(color.red()), double(color.green()), double(color.blue())};
                for (std::size_t y = 0; y < input.height(); ++y) {
                    if (stop.stop_requested()) throw std::runtime_error("Operation cancelled");
                    for (std::size_t x = 0; x < input.width(); ++x) {
                        const QPointF point(x + .5, y + .5);
                        if (roi && !roi->contains(point)) continue;
                        if (operation == "draw" && roi && roi->contains(point + QPointF(1, 0))
                            && roi->contains(point - QPointF(1, 0)) && roi->contains(point + QPointF(0, 1))
                            && roi->contains(point - QPointF(0, 1))) continue;
                        const std::array<std::size_t, 5> index{x, y, z, c, t};
                        for (std::size_t component = 0; component < componentCount(type); ++component) {
                            if (type == PixelType::RGBA8 && component == 3) continue;
                            double value = operation == "clear" ? 0 : maximum;
                            if (painting) value = componentCount(type) > 1 ? rgb[component] : gray;
                            if (operation == "invert") value = minimum + maximum - input.sample(x, y, z, c, t, component);
                            if (operation == "smooth") {
                                value = 0;
                                int count = 0;
                                for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                                    const double sample = input.sample(std::clamp<qint64>(qint64(x) + dx, 0, input.width() - 1),
                                        std::clamp<qint64>(qint64(y) + dy, 0, input.height() - 1), z, c, t, component);
                                    ++count;
                                    value = std::isfinite(value) && std::isfinite(sample)
                                        ? std::lerp(value, sample, 1.0 / count) : value + sample;
                                }
                            }
                            output->setValue(std::span(index).first(input.shape().size()), value, component);
                        }
                    }
                    progress(double(y + 1) / input.height());
                }
            }, [this, target, revision, output, operation](auto error) {
                if (showJobError(error)) return;
                if (target && target->revision == revision) target->replaceImage(*output, operation);
                else { output->metadata().sourcePath.clear(); addDocument(*output)->modified = true; }
                updatePanels();
            });
        }});
    }
    commands_.add({"file.new", tr("New image..."), "File", QKeySequence("N"), false,
        {{"width", tr("Width"), ParameterKind::Integer, 512, 1, 16384, {}},
         {"height", tr("Height"), ParameterKind::Integer, 512, 1, 16384, {}},
         {"depth", tr("Slices"), ParameterKind::Integer, 1, 1, 10000, {}}}, [this](const QVariantMap &p) {
        const std::vector<std::size_t> shape{p.value("width").toUInt(), p.value("height").toUInt(), p.value("depth").toUInt()};
        std::size_t bytes = 1;
        for (const auto length : shape) {
            if (length > std::numeric_limits<std::size_t>::max() / bytes)
                throw std::length_error("New image exceeds the addressable size");
            bytes *= length;
        }
        detail::checkMemoryBudget(bytes, detail::availableMemoryBytes());
        auto output = std::make_shared<Image>();
        jobs_.submit(tr("Creating image"), [shape, bytes, output](auto stop, auto progress) {
            if (stop.stop_requested()) throw std::runtime_error("Image creation cancelled");
            detail::checkMemoryBudget(bytes, detail::availableMemoryBytes());
            ImageMetadata metadata; metadata.name = tr("Untitled");
            *output = Image::allocate(PixelType::UInt8, shape, std::move(metadata));
            if (stop.stop_requested()) throw std::runtime_error("Image creation cancelled");
            progress(1);
        }, [this, output](auto error) {
            if (showJobError(error)) return;
            addDocument(*output)->modified = true; updatePanels();
        });
    }});
    for (const bool crop : {false, true}) {
        commands_.add({crop ? "image.crop" : "image.duplicate", crop ? tr("Crop") : tr("Duplicate"), "Edit",
            QKeySequence(crop ? "Shift+X" : "Shift+D"), true, {}, [this, crop](const QVariantMap &) {
            auto *doc = activeDocument(); if (!doc) return;
            Image output = doc->image;
            if (const auto roi = selectedRoi()) {
                if (roi->type == RoiType::Line) { showError(tr("Select an area ROI first.")); return; }
                output = cropToBounds(output, roi->bounds());
                if (output.empty()) { showError(tr("Select an area ROI first.")); return; }
            } else if (crop) { showError(tr("Select an area ROI first.")); return; }
            output = output.clone(); output.metadata().sourcePath.clear();
            if (crop) { doc->setSelection({}); doc->rois.clear(); doc->replaceImage(std::move(output), tr("Crop")); }
            else { output.metadata().name += tr(" copy"); addDocument(std::move(output))->modified = true; }
            updatePanels();
        }});
    }
    commands_.add({"selection.all", tr("Select all"), "Edit", QKeySequence("A"), true, {}, [this](const QVariantMap &) {
        auto *doc = activeDocument(); if (!doc) return;
        doc->previousSelection = selectedRoi();
        Roi roi; roi.name = tr("All"); roi.points = {{0, 0}, {double(doc->image.width()), double(doc->image.height())}};
        roi.z = doc->viewer->slice(); roi.channel = doc->viewer->channel(); roi.time = doc->viewer->time();
        doc->setSelection(roi);
    }});
    commands_.add({"selection.none", tr("Select none"), "Edit", QKeySequence("Shift+A"), true, {}, [this](const QVariantMap &) {
        auto *doc = activeDocument(); if (!doc) return;
        doc->previousSelection = selectedRoi(); doc->setSelection({});
    }});
    commands_.add({"selection.restore", tr("Restore selection"), "Edit", QKeySequence("Shift+E"), true, {}, [this](const QVariantMap &) {
        auto *doc = activeDocument(); if (doc && doc->previousSelection) doc->setSelection(doc->previousSelection);
    }});
    for (const auto &entry : {std::pair{"view.display", "display"}, {"view.info", "properties"},
                              {"view.properties", "properties"}, {"view.channels", "display"}}) {
        const QMap<QString, QString> labels{{"view.display", tr("Display")}, {"view.info", tr("Show info")},
            {"view.properties", tr("Image properties")}, {"view.channels", tr("Channels")}};
        commands_.add({entry.first, labels.value(entry.first), "View", {}, QString(entry.first) != "view.display", {}, [this, panel = QString(entry.second)](const QVariantMap &) { showPanel(panel); }});
    }
    commands_.add({"selection.add", tr("Add to ROI manager"), "Analyze", QKeySequence("T"), true, {}, [this](const QVariantMap &) {
        auto *doc = activeDocument(); const auto roi = selectedRoi(); if (!doc || !roi) return;
        if (std::none_of(doc->rois.begin(), doc->rois.end(), [&](const Roi &saved) { return saved.id == roi->id; })) {
            auto saved = *roi; saved.name = tr("ROI %1").arg(doc->rois.size() + 1);
            doc->rois.push_back(std::move(saved));
        }
        showPanel("rois");
    }});
    commands_.add({"view.fit", tr("Fit to window"), "View", QKeySequence("Ctrl+0"), true, {}, [this](const QVariantMap &) {
        if (auto *doc = activeDocument()) doc->viewer->fitToWindow();
    }});
    const QList<QPair<QString, QString>> viewModes{{"view.single", tr("Single view")},
        {"view.linked", tr("Linked view")}, {"view.orthogonal", tr("Orthogonal views")}};
    for (int index = 0; index < viewModes.size(); ++index) {
        commands_.add({viewModes[index].first, viewModes[index].second, "View", {}, true, {}, [this, index](const QVariantMap &) {
            QPointer<DocumentWidget> doc = activeDocument();
            if (!doc) return;
            if (index != int(DocumentWidget::ViewMode::Linked)) {
                doc->setViewMode(DocumentWidget::ViewMode(index));
                return;
            }
            QList<QPointer<DocumentWidget>> candidates;
            QDialog picker(this);
            picker.setWindowTitle(tr("Linked view"));
            auto *layout = new QVBoxLayout(&picker);
            auto *images = new QListWidget(&picker);
            images->setObjectName("linkedImages");
            images->setAccessibleName(tr("Linked images"));
            layout->addWidget(images);
            for (auto *other : documents()) if (other != doc) {
                candidates.append(other);
                auto *item = new QListWidgetItem(tr("%1. %2").arg(candidates.size()).arg(other->title()), images);
                item->setToolTip(other->image.metadata().sourcePath);
                item->setCheckState(doc->linkedDocuments().contains(other) ? Qt::Checked : Qt::Unchecked);
            }
            if (candidates.isEmpty()) return;
            auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &picker);
            layout->addWidget(buttons);
            connect(buttons, &QDialogButtonBox::accepted, &picker, &QDialog::accept);
            connect(buttons, &QDialogButtonBox::rejected, &picker, &QDialog::reject);
            picker.resize(400, 300);
            if (picker.exec() == QDialog::Accepted && doc) {
                QList<DocumentWidget *> selected;
                for (int i = 0; i < candidates.size(); ++i)
                    if (candidates[i] && images->item(i)->checkState() == Qt::Checked) selected.append(candidates[i]);
                doc->linkTo(selected);
            }
            updatePanels();
        }});
    }
    commands_.add({"process.repeat", tr("Repeat command"), "Process", QKeySequence("Shift+R"), true, {}, [this](const QVariantMap &) {
        if (!repeatCommand_.isEmpty()) commands_.execute(repeatCommand_, repeatParameters_);
    }});
    commands_.add({"window.close", tr("Close window"), "View", QKeySequence::Close, false, {}, [this](const QVariantMap &) {
        auto *window = QApplication::activeWindow();
        if (window && window != this) window->close(); else if (active_) active_->close();
    }});
    commands_.add({"window.showAll", tr("Show all windows"), "View", QKeySequence("Shift+F"), false, {}, [this](const QVariantMap &) {
        showNormal(); for (auto *doc : documents()) doc->showViews();
        for (auto *panel : panels_) if (panel->isVisible() || panel->isMinimized()) panel->showNormal();
    }});
    commands_.add({"window.next", tr("Next image window"), "View", QKeySequence("Tab"), true, {}, [this](const QVariantMap &) {
        const auto list = documents(); if (list.isEmpty()) return;
        auto *next = list[(list.indexOf(activeDocument()) + 1) % list.size()];
        setActiveDocument(next); next->showNormal(); next->raise(); next->activateWindow(); next->viewer->setFocus();
    }});
    commands_.add({"window.launcher", tr("ImageCXX toolbar"), "View", QKeySequence("Return"), false, {}, [this](const QVariantMap &) {
        showNormal(); raise(); activateWindow();
    }});

    QMap<QString, QMenu *> menus;
    for (const auto &category : {"File", "Edit", "Process", "Analyze", "View"}) menus.insert(category, menuBar()->addMenu(tr(category)));
    auto *stacksMenu = new QMenu(tr("Stacks"), this); stacksMenu->setObjectName("stacksMenu");
    menus.insert("Stacks", stacksMenu);
    const QMap<QString, QStringList> bindings{
            {"file.open", {"O"}}, {"file.new", {"N"}}, {"file.save", {"S"}},
            {"file.next", {"Shift+O", "Alt+Shift+O"}}, {"file.revert", {"R", "Alt+R"}},
            {"edit.copy", {"C", "Alt+C", "Ctrl+Alt+C"}}, {"image.duplicate", {"Shift+D", "Alt+Shift+D"}},
            {"file.sample", {"Shift+B"}}, {"edit.undo", {"Z"}}, {"edit.redo", {"Ctrl+Y"}},
            {"analyze.measure", {"M"}}, {"analyze.histogram", {"H"}}, {"analyze.profile", {"K"}},
            {"edit.clear", {"Backspace", "Delete"}}, {"edit.fill", {"F"}}, {"edit.draw", {"D"}},
            {"edit.invert", {"Shift+I"}}, {"edit.smooth", {"Shift+S"}}, {"process.threshold", {"Shift+T"}}, {"process.resample", {"E"}},
            {"view.display", {"Ctrl+J", "Shift+C"}}, {"view.info", {"I"}}, {"view.properties", {"Shift+P"}},
            {"view.channels", {"Shift+Z"}}, {"selection.add", {"T"}}, {"window.close", {"W"}},
            {"window.showAll", {"Shift+F", "]"}}, {"window.launcher", {"Return", "Enter"}}
        };
    for (const auto &command : commands_.commands()) {
        auto *action = menus[command.category]->addAction(command.label);
        action->setObjectName(command.id);

        QList<QKeySequence> shortcuts;
        if (command.id == "edit.redo") shortcuts.append(QKeySequence("Ctrl+Y"));
        else if (!command.shortcut.isEmpty()) shortcuts.append(command.shortcut);
        for (const auto &binding : bindings.value(command.id)) {
            const QKeySequence key(binding); if (!shortcuts.contains(key)) shortcuts.append(key);
        }
        // Qt maps ControlModifier to Command on macOS, and AltModifier to Option.
        const auto bareKeys = shortcuts;
        for (const auto &key : bareKeys) {
            if (key.count() == 1 && !(key[0].keyboardModifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
                && key[0].key() >= Qt::Key_A && key[0].key() <= Qt::Key_Z) {
                const QKeySequence modified(key[0].toCombined() | Qt::CTRL);
                if (!shortcuts.contains(modified)) shortcuts.append(modified);
            }
        }
        action->setShortcuts(shortcuts);
        for (const auto &key : shortcuts) shortcutActions_.insert(key, action);
        action->setShortcutContext(Qt::ApplicationShortcut);
        const QMap<QString, QString> icons{{"file.open", "open"}, {"analyze.measure", "measure"},
            {"analyze.histogram", "histogram"}, {"edit.undo", "undo"}};
        if (icons.contains(command.id)) action->setIcon(QIcon(":/icons/" + icons.value(command.id) + ".svg"));
        if (command.id == "edit.color") {
            QPixmap swatch(24, 24); swatch.fill(drawingColor_); action->setIcon(QIcon(swatch));
        }
        connect(action, &QAction::triggered, this, [this, id = command.id] { executeCommand(id); });
        if (command.needsImage) imageActions_.append(action);
    }
    menus["File"]->addSeparator();
    auto *quit = menus["File"]->addAction(tr("Quit")); quit->setShortcut(QKeySequence::Quit); quit->setShortcutContext(Qt::ApplicationShortcut);
    connect(quit, &QAction::triggered, this, &QWidget::close);
    auto *search = menus["Edit"]->addAction(QIcon(":/icons/search.svg"), tr("Find command...")); search->setObjectName("commands.find"); search->setShortcuts({QKeySequence("L"), QKeySequence("Ctrl+L")});
    for (const auto &key : search->shortcuts()) shortcutActions_.insert(key, search);
    search->setShortcutContext(Qt::ApplicationShortcut);
    auto *query = new QLineEdit(this);
    query->setObjectName("commandSearch");
    query->setPlaceholderText(tr("Find command..."));
    query->setAccessibleName(tr("Find command"));
    query->setToolTip(search->text() + " (" + QKeySequence("Ctrl+L").toString(QKeySequence::NativeText) + ")");
    query->setClearButtonEnabled(true);
    query->setFixedWidth(220);
    statusBar()->addPermanentWidget(query);
    auto *searchModel = new QStandardItemModel(query);
    for (const auto &command : commands_.commands()) {
        auto *item = new QStandardItem(command.category + "  /  " + command.label);
        item->setData(command.id, Qt::UserRole);
        item->setEditable(false);
        item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        auto *action = findChild<QAction *>(command.id);
        item->setEnabled(action->isEnabled());
        connect(action, &QAction::changed, query, [action, item] { item->setEnabled(action->isEnabled()); });
        searchModel->appendRow(item);
    }
    auto *completer = new QCompleter(searchModel, query);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    completer->setMaxVisibleItems(10);
    completer->popup()->setMinimumWidth(420);
    completer->popup()->setStyleSheet("QListView::item { padding: 3px 8px 0px 8px; }");
    query->setCompleter(completer);
    const auto runSearch = [this, query](const QModelIndex &index) {
        if (!(index.flags() & Qt::ItemIsEnabled)) return;
        const auto id = index.data(Qt::UserRole).toString();
        // Let QLineEdit finish inserting the completion before clearing it or opening a command dialog.
        QTimer::singleShot(0, this, [this, query, id] { query->clear(); executeCommand(id); });
    };
    connect(completer, qOverload<const QModelIndex &>(&QCompleter::activated), this, runSearch);
    connect(query, &QLineEdit::returnPressed, this, [query, completer, runSearch] {
        if (query->text().isEmpty() || completer->popup()->currentIndex().isValid()) return;
        completer->setCompletionPrefix(query->text());
        runSearch(completer->currentIndex());
    });
    connect(search, &QAction::triggered, this, [this, query, completer] {
        showNormal(); raise(); activateWindow();
        query->setFocus(Qt::ShortcutFocusReason); query->selectAll();
        completer->setCompletionPrefix(query->text()); completer->complete();
    });
    windowMenu_ = menuBar()->addMenu(tr("Window"));
    windowMenu_->setObjectName("windowMenu");
    connect(windowMenu_, &QMenu::aboutToShow, this, &MainWindow::updateWindowMenu);
    auto *helpMenu = menuBar()->addMenu(tr("Help"));
    helpMenu->setObjectName("helpMenu");
    auto *checkUpdates = helpMenu->addAction(tr("Check for Updates..."));
    checkUpdates->setObjectName("help.checkUpdates");
    checkUpdates->setMenuRole(QAction::NoRole);
    checkUpdates->setEnabled(false);
    crosshair_ = menus["View"]->addAction(tr("Crosshair")); crosshair_->setCheckable(true);
    connect(crosshair_, &QAction::toggled, this, [this](bool value) { if (auto *doc = activeDocument()) doc->viewer->setCrosshairVisible(value); });
    scaleBar_ = menus["View"]->addAction(tr("Scale bar")); scaleBar_->setCheckable(true); scaleBar_->setChecked(false);
    connect(scaleBar_, &QAction::toggled, this, [this](bool value) { if (auto *doc = activeDocument()) doc->viewer->setScaleBarVisible(value); });
    auto *toolbar = addToolBar(tr("Image tools"));
    toolbar->setObjectName("imageTools"); toolbar->setMovable(false); toolbar->setFloatable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly); toolbar->setIconSize(QSize(24, 24));
    toolbar->addAction(findChild<QAction *>("file.open")); toolbar->addSeparator();
    tools_ = new QActionGroup(this);
    const std::vector<std::pair<QString, ImageViewer::Tool>> tools{{tr("Pointer"), ImageViewer::Tool::Pointer},
        {tr("Pan"), ImageViewer::Tool::Pan},
        {tr("Rectangle"), ImageViewer::Tool::Rectangle}, {tr("Ellipse"), ImageViewer::Tool::Ellipse},
        {tr("Polygon"), ImageViewer::Tool::Polygon}, {tr("Line"), ImageViewer::Tool::Line}};
    for (const auto &[name, tool] : tools) {
        auto *action = toolbar->addAction(name);
        const QStringList iconNames{"pointer", "pan", "rectangle", "ellipse", "polygon", "line"};
        action->setIcon(QIcon(":/icons/" + iconNames[int(tool)] + ".svg")); action->setCheckable(true); tools_->addAction(action);
        if (tool == ImageViewer::Tool::Pointer) action->setToolTip(tr("Click or drag to move the crosshair in orthogonal views."));
        else if (tool == ImageViewer::Tool::Polygon) action->setToolTip(tr("Draw a polygon. Double-click to finish."));
        else if (tool == ImageViewer::Tool::Pan) action->setToolTip(tr("Drag to pan. Wheel to browse slices. Ctrl or Command + wheel to zoom. F to fit."));
        action->setData(static_cast<int>(tool)); if (tool == ImageViewer::Tool::Pointer) action->setChecked(true);
        connect(action, &QAction::triggered, this, [this, tool] { if (auto *doc = activeDocument()) doc->viewer->setTool(tool); });
    }
    toolbar->addSeparator(); toolbar->addAction(findChild<QAction *>("analyze.measure"));
    toolbar->addAction(findChild<QAction *>("analyze.histogram"));
    toolbar->addAction(findChild<QAction *>("edit.undo"));
    toolbar->addAction(findChild<QAction *>("edit.color"));
    toolbar->addSeparator();
    lutMenu_ = new QMenu(tr("LUT"), toolbar);
    lutMenu_->setObjectName("lutMenu");
    auto *luts = new QActionGroup(lutMenu_);
    for (const auto &name : QStringList{"grayscale", "hot", "viridis"}) {
        auto *action = lutMenu_->addAction(name);
        action->setCheckable(true); action->setData(name); luts->addAction(action);
    }
    toolbar->addAction(stacksMenu->menuAction());
    toolbar->addAction(lutMenu_->menuAction());
    auto *lutButton = qobject_cast<QToolButton *>(toolbar->widgetForAction(lutMenu_->menuAction()));
    lutButton->setObjectName("lutButton");
    lutButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    lutButton->setPopupMode(QToolButton::InstantPopup);
    lutButton->setToolTip(tr("Color map"));
    lutButton->setStyleSheet(R"(
        QToolButton#lutButton { min-width: 24px; min-height: 24px; }
        QToolButton#lutButton::menu-indicator { image: none; width: 0px; }
    )");
    connect(lutMenu_, &QMenu::aboutToShow, this, &MainWindow::updateDisplayControls);
    auto *stacksButton = qobject_cast<QToolButton *>(toolbar->widgetForAction(stacksMenu->menuAction()));
    stacksButton->setObjectName("stacksButton"); stacksButton->setText(tr("Stk"));
    stacksButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    stacksButton->setPopupMode(QToolButton::InstantPopup);
    stacksButton->setAccessibleName(tr("Stacks")); stacksButton->setToolTip(tr("Stacks"));
    stacksButton->setStyleSheet(R"(
        QToolButton#stacksButton { min-width: 24px; min-height: 24px; padding: 5px; }
        QToolButton#stacksButton::menu-indicator { image: none; width: 0px; }
    )");
    connect(luts, &QActionGroup::triggered, this, [this](QAction *action) {
        if (loadingPanels_) return;
        if (auto *doc = activeDocument()) doc->viewer->setLut(action->data().toString());
        updateDisplayControls();
    });
    connect(measureButton, &QPushButton::clicked, this, [this] { executeCommand("analyze.measure"); });
    connect(autoRange, &QPushButton::clicked, this, [this] { if (auto *doc = activeDocument()) doc->viewer->autoWindow(); });
    connect(fitButton, &QPushButton::clicked, this, [this] { if (auto *doc = activeDocument()) doc->viewer->fitToWindow(); });
    const auto applyChannelTime = [this] {
        if (loadingPanels_) return;
        if (auto *doc = activeDocument())
            doc->viewer->setSlice(doc->viewer->slice(), channelIndex_->value() - 1, timeIndex_->value() - 1);
    };
    connect(channelIndex_, &QSpinBox::valueChanged, this, applyChannelTime);
    connect(timeIndex_, &QSpinBox::valueChanged, this, applyChannelTime);
    const auto applyDisplay = [this] {
        if (loadingPanels_) return;
        if (auto *doc = activeDocument()) {
            QScopedValueRollback applyingDisplay(loadingPanels_, true);
            if (maximum_->value() > minimum_->value()) doc->viewer->setWindow(minimum_->value(), maximum_->value());
            doc->viewer->setGamma(gamma_->value());
            doc->viewer->setInverted(inverted_->isChecked()); doc->viewer->setLinearInterpolation(linear_->isChecked());
            doc->viewer->setComposite(composite_->isChecked());
        }
        updateDisplayControls();
    };
    for (auto *spin : {minimum_, maximum_, gamma_}) connect(spin, &QDoubleSpinBox::valueChanged, this, applyDisplay);
    for (const auto &[slider, spin] : {std::pair{minimumSlider_, minimum_}, std::pair{maximumSlider_, maximum_}}) {
        connect(slider, &QSlider::valueChanged, this, [this, spin](int value) {
            if (loadingPanels_) return;
            const double before = spin->value();
            spin->setValue(std::lerp(sliderMinimum_, sliderMaximum_, value / 1000.));
            if (spin->value() == before) updateDisplayControls();
        });
    }
    connect(gammaSlider_, &QSlider::valueChanged, this, [this](int value) {
        if (loadingPanels_) return;
        gamma_->setValue(std::pow(20., (value - 500) / 500.));
    });
    for (auto *check : {inverted_, linear_, composite_}) connect(check, &QCheckBox::toggled, this, applyDisplay);
    connect(removeRoi, &QPushButton::clicked, this, [this] {
        auto *doc = activeDocument(); const int index = rois_->currentRow(); if (!doc || index < 0) return;
        doc->rois.erase(doc->rois.begin() + index);
        doc->refreshRois(); updatePanels();
    });
    connect(rois_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        const QPointer<DocumentWidget> target = activeDocument();
        const int row = rois_->row(item);
        if (!target || row < 0 || row >= int(target->rois.size())) return;
        const QString id = target->rois[std::size_t(row)].id;
        const QString before = target->rois[std::size_t(row)].name;
        bool ok;
        const auto name = QInputDialog::getText(this, tr("Rename ROI"), tr("Name"), QLineEdit::Normal, before, &ok);
        if (!ok || name.isEmpty() || !target) return;
        const auto roi = std::find_if(target->rois.begin(), target->rois.end(), [&](const Roi &value) { return value.id == id; });
        if (roi == target->rois.end()) return;
        auto renamed = *roi;
        renamed.name = name;
        target->editRoi(std::move(renamed));
    });
    connect(rois_, &QListWidget::currentRowChanged, this, [this](int row) {
        auto *doc = activeDocument(); if (!doc || row < 0 || row >= int(doc->rois.size())) return;
        const auto roi = doc->rois[row]; doc->viewer->setSlice(roi.z, roi.channel, roi.time); doc->setSelection(roi);
    });
    QSettings settings;
    recentFiles_ = settings.value("recentFiles").toStringList();
    settings.beginGroup("windowWorkflow/v1");
    if (!settings.value("launcherGeometry").toByteArray().isEmpty())
        restoreGeometry(settings.value("launcherGeometry").toByteArray());
    resize(std::clamp(width(), 560, 900), minimumSizeHint().height());
    restorePanelLayout(settings.value("panels").toByteArray());
    recentMenu_ = menus["File"]->addMenu(tr("Recent files"));
    updateRecentFiles();
    updateWindowMenu();
    updatePanels();
    qApp->installEventFilter(this);
}

MainWindow::~MainWindow()
{
    qApp->removeEventFilter(this);
    jobs_.cancelAll();
    clearDocuments();
    for (auto *window : panels_) delete window;
    panels_.clear();
}

DocumentWidget *MainWindow::activeDocument() const
{
    return active_.data();
}

QList<DocumentWidget *> MainWindow::documents() const
{
    QList<DocumentWidget *> result;
    for (const auto &document : documents_) if (document) result.append(document.data());
    return result;
}

QWidget *MainWindow::panel(const QString &id) const
{
    return panels_.value(id, nullptr);
}

void MainWindow::showPanel(const QString &id)
{
    auto *window = panel(id);
    if (!window && (id == "histogram" || id == "profile")) window = plotWindow(id == "profile");
    if (!window) return;
    updatePanels();
    window->showNormal();
    window->raise();
    window->activateWindow();
}

void MainWindow::setActiveDocument(DocumentWidget *document)
{
    if (active_ == document) return;
    active_ = document;
    if (document) {
        document->viewer->setCrosshairVisible(crosshair_->isChecked());
        document->viewer->setScaleBarVisible(scaleBar_->isChecked());
        if (auto *action = tools_->checkedAction())
            document->viewer->setTool(static_cast<ImageViewer::Tool>(action->data().toInt()));
        pixelStatus_->setText(document->image.metadata().name);
        pixelStatus_->setToolTip(document->image.metadata().name);
    } else {
        pixelStatus_->setText(tr("Ready"));
        pixelStatus_->setToolTip({});
    }
    updatePanels();
}

void MainWindow::clearDocuments()
{
    // Include windows waiting for WA_DeleteOnClose's deferred destruction.
    const auto windows = findChildren<DocumentWidget *>(QString{}, Qt::FindDirectChildrenOnly);
    documents_.clear();
    active_.clear();
    for (auto *document : windows) {
        disconnect(document, nullptr, this, nullptr);
        disconnect(document->viewer, nullptr, this, nullptr);
        delete document;
    }
}

DocumentWidget *MainWindow::addDocument(Image image)
{
    auto *document = new DocumentWidget(std::move(image), this);
    connect(document, &DocumentWidget::pathsDropped, this, &MainWindow::openPaths);
    document->setAttribute(Qt::WA_DeleteOnClose);
    documents_.append(document);
    document->setScreen(screen());
    document->resizeToImage();
    const int offset = int((documents_.size() - 1) % 8) * 28;
    const QPoint position = frameGeometry().bottomLeft() + QPoint(offset, 12 + offset);
    document->move(position);
    connect(document, &DocumentWidget::activated, this, [this, document] { setActiveDocument(document); });
    connect(document, &DocumentWidget::closing, this, [this, document] {
        documents_.removeAll(QPointer<DocumentWidget>(document));
        if (active_ == document) {
            const auto remaining = documents();
            setActiveDocument(remaining.isEmpty() ? nullptr : remaining.last());
        }
        updatePanels();
    });
    connect(document, &DocumentWidget::changed, this, [this, document] {
        document->setWindowTitle(document->title());
        if (activeDocument() == document) updatePanels();
    });
    connect(document, &DocumentWidget::pixelHovered, this, [this, document](std::size_t x, std::size_t y, std::size_t z, double value) {
        if (activeDocument() != document) return;
        const auto text = tr("X %1   Y %2   Z %3   Value %4").arg(x).arg(y).arg(z).arg(value, 0, 'g', 12);
        pixelStatus_->setText(text);
        pixelStatus_->setToolTip(text);
    });
    connect(document, &DocumentWidget::viewError, this, [this](const QString &error) { statusBar()->showMessage(error, 15000); });
    connect(document, &DocumentWidget::displayChanged, this, [this, document] {
        if (activeDocument() == document) updateDisplayControls();
    });
    connect(document->viewer, &ImageViewer::sliceReady, this, [this, document] {
        if (activeDocument() == document) updateDisplayControls();
    });
    const auto showWhenReady = connect(document->viewer, &ImageViewer::sliceReady, document, [this, document] {
        auto *active = activeDocument();
        document->setAttribute(Qt::WA_ShowWithoutActivating);
        document->show();
        setActiveDocument(active);
        if (active && active->isVisible()) {
            active->raise();
            active->activateWindow();
        }
    }, Qt::SingleShotConnection);
    connect(document->viewer, &ImageViewer::loadFailed, this, [this, document, showWhenReady](const QString &error) {
        if (disconnect(showWhenReady)) document->close();
        statusBar()->showMessage(error, 15000);
    });
    connect(document->viewer, &ImageViewer::zoomChanged, this, [this, document](double scale) {
        if (activeDocument() == document)
            statusBar()->showMessage(tr("Zoom %1%").arg(scale * 100, 0, 'f', 1), 2000);
    });
    setActiveDocument(document);
    return document;
}

void MainWindow::pasteImage(bool systemImage, bool newImage)
{
    const auto *mime = QApplication::clipboard()->mimeData();
    if (!mime) return;
    Image clipboard;
    if (const auto *native = dynamic_cast<const ImageMimeData *>(mime); native && !systemImage) {
        clipboard = *native->image;
    } else {
        QImage raster;
        if (mime->hasFormat("image/png")) raster.loadFromData(mime->data("image/png"), "PNG");
        if (raster.isNull()) raster = QApplication::clipboard()->image();
        clipboard = imageFromRaster(std::move(raster));
    }
    if (clipboard.empty()) return;
    auto *doc = activeDocument();
    if (newImage || !doc) {
        clipboard.metadata().name = tr("Clipboard");
        clipboard.metadata().sourcePath.clear();
        addDocument(std::move(clipboard))->modified = true;
        updatePanels();
        return;
    }
    if (doc->image.shape().size() > 5) { showError(tr("Pixel editing supports up to five dimensions.")); return; }
    if (clipboard.pixelType() != doc->image.pixelType()) {
        showError(tr("Clipboard and image pixel types must match. Use Paste special to paste into a new image.")); return;
    }
    const Image input = doc->image;
    const QPointer<DocumentWidget> target = doc;
    const auto revision = doc->revision;
    const auto z = doc->viewer->slice(), c = doc->viewer->channel(), t = doc->viewer->time();
    auto output = std::make_shared<Image>();
    const auto roi = selectedRoi();
    const auto origin = roi ? roi->bounds().topLeft().toPoint() : QPoint(0, 0);
    jobs_.submit(tr("Paste"), [input, clipboard, output, origin, z, c, t](auto stop, auto progress) {
        *output = input.clone();
        const auto pixelBytes = bytesPerPixel(input.pixelType());
        auto *destination = output->mutableData() + (z + input.depth() * (c + input.channels() * t))
            * input.width() * input.height() * pixelBytes;
        const auto left = std::max<qint64>(0, origin.x()), top = std::max<qint64>(0, origin.y());
        const auto right = std::min<qint64>(input.width(), qint64(origin.x()) + qint64(clipboard.width()));
        const auto bottom = std::min<qint64>(input.height(), qint64(origin.y()) + qint64(clipboard.height()));
        for (auto y = top; y < bottom && right > left; ++y) {
            if (stop.stop_requested()) throw std::runtime_error("Paste cancelled");
            std::memcpy(destination + (y * input.width() + left) * pixelBytes,
                clipboard.data() + ((y - origin.y()) * clipboard.width() + left - origin.x()) * pixelBytes,
                (right - left) * pixelBytes);
            progress(double(y - top + 1) / (bottom - top));
        }
    }, [this, target, revision, output](auto error) {
        if (showJobError(error)) return;
        if (target && target->revision == revision) target->replaceImage(*output, tr("Paste"));
        else {
            output->metadata().sourcePath.clear();
            addDocument(*output)->modified = true;
        }
        updatePanels();
    });
}

void MainWindow::importRaw(const QString &path)
{
    QSettings settings;
    settings.beginGroup("rawImport/v1");
    QDialog dialog(this);
    dialog.setObjectName("rawImportDialog");
    dialog.setWindowTitle(tr("Import raw: %1").arg(QFileInfo(path).fileName()));
    auto *layout = new QFormLayout(&dialog);
    auto *type = new QComboBox(&dialog);
    type->setObjectName("rawPixelType");
    const std::array types{PixelType::UInt8, PixelType::Int16, PixelType::UInt16,
                           PixelType::Float32, PixelType::Float64, PixelType::RGB8, PixelType::RGBA8};
    const QStringList labels{tr("8-bit"), tr("16-bit Signed"), tr("16-bit Unsigned"),
                             tr("32-bit Real"), tr("64-bit Real"), tr("24-bit RGB"), tr("32-bit RGBA")};
    for (std::size_t i = 0; i < types.size(); ++i) type->addItem(labels[int(i)], pixelTypeName(types[i]));
    type->setCurrentIndex(2);
    layout->addRow(tr("Image type"), type);
    const QStringList dimensionNames{"rawWidth", "rawHeight", "rawImages"};
    const QStringList dimensionLabels{tr("Width"), tr("Height"), tr("Number of images")};
    std::array<QSpinBox *, 3> dimensions;
    for (std::size_t i = 0; i < dimensions.size(); ++i) {
        auto *spin = new QSpinBox(&dialog);
        spin->setObjectName(dimensionNames[int(i)]);
        spin->setRange(1, std::numeric_limits<int>::max());
        spin->setValue(i < 2 ? 512 : 1);
        dimensions[i] = spin;
        layout->addRow(dimensionLabels[int(i)], spin);
    }
    dimensions[2]->setToolTip(tr("Maximum number of images to import. Stops at the last complete image."));
    auto *offset = new QLineEdit("0", &dialog);
    offset->setObjectName("rawOffset");
    layout->addRow(tr("Offset to first image, bytes"), offset);
    auto *endian = new QComboBox(&dialog);
    endian->setObjectName("rawByteOrder");
    endian->addItem(tr("Native"), "native");
    endian->addItem(tr("Little endian"), "little");
    endian->addItem(tr("Big endian"), "big");
    layout->addRow(tr("Byte order"), endian);

    auto *advanced = new QToolButton(&dialog);
    advanced->setObjectName("rawAdvanced");
    advanced->setText(tr("Advanced"));
    advanced->setCheckable(true);
    advanced->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    advanced->setArrowType(Qt::RightArrow);
    layout->addRow(advanced);
    auto *advancedPanel = new QWidget(&dialog);
    auto *advancedLayout = new QFormLayout(advancedPanel);
    advancedLayout->setContentsMargins(0, 0, 0, 0);
    auto *channels = new QSpinBox(advancedPanel);
    channels->setObjectName("rawChannels");
    channels->setRange(1, std::numeric_limits<int>::max());
    auto *frames = new QSpinBox(advancedPanel);
    frames->setObjectName("rawFrames");
    frames->setRange(1, std::numeric_limits<int>::max());
    auto *spacing = new QLineEdit("1,1,1", advancedPanel);
    spacing->setObjectName("rawSpacing");
    auto *unit = new QLineEdit("px", advancedPanel);
    unit->setObjectName("rawUnit");
    auto *mapped = new QCheckBox(tr("Use memory mapping when possible"), advancedPanel);
    mapped->setObjectName("rawMemoryMap");
    mapped->setChecked(true);
    advancedLayout->addRow(tr("Channels"), channels);
    advancedLayout->addRow(tr("Time frames"), frames);
    advancedLayout->addRow(tr("Spacing X,Y,Z"), spacing);
    advancedLayout->addRow(tr("Spatial unit"), unit);
    advancedLayout->addRow(mapped);
    layout->addRow(advancedPanel);
    advancedPanel->hide();
    connect(advanced, &QToolButton::toggled, &dialog, [advanced, advancedPanel, &dialog](bool expanded) {
        advanced->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        advancedPanel->setVisible(expanded);
        dialog.adjustSize();
    });

    const QMap<QString, QSpinBox *> integerOptions{{"width", dimensions[0]}, {"height", dimensions[1]},
        {"maximumImages", dimensions[2]}, {"channels", channels}, {"frames", frames}};
    const QMap<QString, QLineEdit *> textOptions{{"headerBytes", offset}, {"spacing", spacing}, {"unit", unit}};
    for (auto it = integerOptions.cbegin(); it != integerOptions.cend(); ++it)
        it.value()->setValue(settings.value(it.key(), it.value()->value()).toInt());
    for (auto it = textOptions.cbegin(); it != textOptions.cend(); ++it)
        it.value()->setText(settings.value(it.key(), it.value()->text()).toString());
    const auto savedType = type->findData(settings.value("pixelType", type->currentData()));
    if (savedType >= 0) type->setCurrentIndex(savedType);
    const auto savedOrder = endian->findData(settings.value("byteOrder", endian->currentData()));
    if (savedOrder >= 0) endian->setCurrentIndex(savedOrder);
    mapped->setChecked(settings.value("memoryMap", true).toBool());
    advanced->setChecked(settings.value("advanced", false).toBool());

    auto *error = new QLabel(&dialog);
    error->setObjectName("rawImportError");
    error->setWordWrap(true);
    error->setStyleSheet("color: #B3261E;");
    layout->addRow(error);
    error->hide();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    RawImportOptions options;
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        try {
            bool ok;
            const auto header = offset->text().trimmed().toULongLong(&ok);
            if (!ok || header > static_cast<quint64>(std::numeric_limits<qint64>::max()))
                throw std::invalid_argument("Offset must be a non-negative whole number of bytes.");
            QJsonArray shape{dimensions[0]->value(), dimensions[1]->value()};
            if (dimensions[2]->value() > 1 || channels->value() > 1 || frames->value() > 1)
                shape.append(1);
            if (channels->value() > 1 || frames->value() > 1) shape.append(channels->value());
            if (frames->value() > 1) shape.append(frames->value());
            options = rawImportOptionsFromJson({{"pixelType", type->currentData().toString()},
                {"shape", shape}, {"headerBytes", static_cast<qint64>(header)},
                {"byteOrder", endian->currentData().toString()}, {"memoryMap", mapped->isChecked()}});
            std::size_t bytes = bytesPerPixel(options.pixelType);
            for (const auto size : options.shape) bytes *= size;
            const auto fileBytes = static_cast<quint64>(QFileInfo(path).size());
            if (header > fileBytes || bytes > fileBytes - header)
                throw std::invalid_argument("The file is too small for these dimensions, image type and offset.");
            const auto imageCount = std::min<quint64>(dimensions[2]->value(), (fileBytes - header) / bytes);
            if (options.shape.size() > 2) options.shape[2] = static_cast<std::size_t>(imageCount);
            const QStringList axisNames{"X", "Y", "Z", "C", "T"};
            const auto spacings = spacing->text().split(',');
            for (std::size_t i = 0; i < options.shape.size(); ++i) {
                ok = true;
                const double value = i < 3 && i < std::size_t(spacings.size()) ? spacings[int(i)].toDouble(&ok) : 1;
                if (!ok || !std::isfinite(value) || value <= 0)
                    throw std::invalid_argument("Spacing must be positive.");
                options.metadata.axes.push_back({axisNames[int(i)], value, 0, i < 3 ? unit->text() : "index"});
            }
            dialog.accept();
        } catch (const std::exception &e) {
            error->setText(QString::fromUtf8(e.what()));
            error->show();
            dialog.adjustSize();
        }
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    for (auto it = integerOptions.cbegin(); it != integerOptions.cend(); ++it)
        settings.setValue(it.key(), it.value()->value());
    for (auto it = textOptions.cbegin(); it != textOptions.cend(); ++it)
        settings.setValue(it.key(), it.value()->text());
    settings.setValue("pixelType", type->currentData());
    settings.setValue("byteOrder", endian->currentData());
    settings.setValue("memoryMap", mapped->isChecked());
    settings.setValue("advanced", advanced->isChecked());
    settings.sync();
    auto output = std::make_shared<Image>();
    jobs_.submit(tr("Importing raw data"),
        [path, options, output](auto stop, auto progress) { *output = readImage(path, options, stop, progress); },
        [this, path, output](auto error) {
            if (showJobError(error)) return;
            addDocument(*output); recentFiles_.removeAll(path); recentFiles_.prepend(path);
            while (recentFiles_.size() > 12) recentFiles_.removeLast();
            updateRecentFiles();
        });
}

void MainWindow::openPaths(const QStringList &paths)
{
    QSet<QString> metaImagePayloads;
    for (const auto &path : paths) {
        const QFileInfo info(path);
        if (info.suffix().compare("mhd", Qt::CaseInsensitive) != 0) continue;
        QFile header(path);
        if (!header.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        while (!header.atEnd()) {
            const auto line = QString::fromUtf8(header.readLine());
            const auto separator = line.indexOf('=');
            if (separator < 0 || line.left(separator).trimmed() != "ElementDataFile") continue;
            const auto payload = QFileInfo(info.dir().filePath(line.mid(separator + 1).trimmed())).canonicalFilePath();
            if (!payload.isEmpty()) metaImagePayloads.insert(payload);
            break;
        }
    }
    QStringList pending;
    QSet<QString> explicitRaw;
    for (const auto &path : paths) {
        if (path.isEmpty()) continue;
        const QFileInfo info(path);
        if (metaImagePayloads.contains(info.canonicalFilePath())) continue;
        const auto suffix = info.suffix().toLower();
        // Keep explicit raw layout dialogs immediate, including cancellation
        // followed by other files in the same selection.
        if (!info.isDir() && (suffix == "raw" || suffix == "bin")) {
            const auto key = info.exists() ? info.canonicalFilePath() : info.absoluteFilePath();
            if (!explicitRaw.contains(key)) { explicitRaw.insert(key); openFiles({path}); }
        } else pending.append(path);
    }
    if (pending.isEmpty()) return;
    auto plan = std::make_shared<ImageImportPlan>();
    jobs_.submit(tr("Scanning images"), [pending, plan](auto stop, auto progress) {
        *plan = discoverImageImports(pending, stop, progress);
        try { checkDicomImportMemory(plan->images, detail::availableMemoryBytes()); }
        catch (const std::exception &error) {
            plan->errors.append(QString::fromUtf8(error.what()));
            plan->images.removeIf([](const auto &image) { return image.dicomBytes || image.paths.size() > 1; });
        }
    }, [this, plan, explicitRaw](auto error) {
        if (showJobError(error)) return;
        for (const auto &image : plan->images) {
            if (image.paths.size() == 1) {
                if (explicitRaw.isEmpty() || !explicitRaw.contains(QFileInfo(image.paths.front()).canonicalFilePath())) openFiles(image.paths);
                continue;
            }
            auto output = std::make_shared<Image>();
            const auto paths = image.paths;
            jobs_.submit(tr("Reading DICOM %1").arg(QFileInfo(paths.front()).fileName()),
                [paths, output](auto stop, auto progress) {
                    *output = readDicomSeries(paths, stop, progress);
                }, [this, paths, output](auto error) {
                    if (error) {
                        try { std::rethrow_exception(error); }
                        catch (const std::exception &exception) {
                            showError(tr("Could not import DICOM %1:\n%2").arg(paths.front(), QString::fromUtf8(exception.what())));
                        }
                        catch (...) { showJobError(error); }
                        return;
                    }
                    addDocument(*output);
                });
        }
        if (!plan->errors.isEmpty()) showError(plan->errors.join('\n'));
    });
}

void MainWindow::openFiles(const QStringList &paths)
{
    for (const auto &path : paths) {
        if (path.isEmpty()) continue;
        if ((QFileInfo(path).suffix().compare("raw", Qt::CaseInsensitive) == 0 ||
             QFileInfo(path).suffix().compare("bin", Qt::CaseInsensitive) == 0) &&
            !QFileInfo::exists(path + ".json")) {
            importRaw(path);
        } else {
            auto output = std::make_shared<Image>();
            jobs_.submit(tr("Opening %1").arg(QFileInfo(path).fileName()),
                [path, output](auto stop, auto progress) { *output = readImage(path, {}, stop, progress); },
                [this, path, output](auto error) {
                    if (showJobError(error)) return;
                    addDocument(*output); recentFiles_.removeAll(path); recentFiles_.prepend(path);
                    while (recentFiles_.size() > 12) recentFiles_.removeLast();
                    updateRecentFiles();
                });
        }
    }
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Show) {
        auto *window = qobject_cast<QWidget *>(watched);
        if (window && (window == this || panels_.values().contains(window))) {
            keepWindowOnScreen(window);
            QTimer::singleShot(0, window, [window] { keepWindowOnScreen(window); });
        }
    }
    if (event->type() == QEvent::Close && watched == panel("measurements") && !confirmDiscardMeasurements()) {
        event->ignore(); return true;
    }
    if (event->type() != QEvent::ShortcutOverride && event->type() != QEvent::KeyPress)
        return QMainWindow::eventFilter(watched, event);
    auto *widget = qobject_cast<QWidget *>(watched);
    if (!widget) return false;
    auto *key = static_cast<QKeyEvent *>(event);
    if ((widget == metadata_ || metadata_->isAncestorOf(widget)) && key->matches(QKeySequence::Copy)) {
        if (event->type() == QEvent::KeyPress && !metadata_->selectedItems().isEmpty())
            QApplication::clipboard()->setText(propertyClipboardText(metadata_, -1));
        key->accept(); return true;
    }
    auto *top = widget->window();
    const auto docs = documents();
    DocumentWidget *imageDocument = nullptr;
    for (auto *document : docs)
        for (auto *view : document->views())
            if (view->window() == top) imageDocument = document;
    if (top != this && !isAncestorOf(top) && !imageDocument) return false;
    if (imageDocument && event->type() == QEvent::KeyPress) setActiveDocument(imageDocument);
    if (top == panel("measurements") && key->key() == Qt::Key_Escape && !QApplication::activeModalWidget()) {
        if (event->type() == QEvent::KeyPress) top->close();
        key->accept(); return true;
    }
    const QKeySequence sequence(key->keyCombination());
    auto *action = shortcutActions_.value(sequence);
    bool editing = QApplication::activeModalWidget() != nullptr;
    for (auto *parent = widget; parent && !editing; parent = parent->parentWidget()) {
        editing = qobject_cast<QLineEdit *>(parent) || qobject_cast<QTextEdit *>(parent)
            || qobject_cast<QPlainTextEdit *>(parent) || qobject_cast<QAbstractSpinBox *>(parent)
            || qobject_cast<QComboBox *>(parent);
    }
    const bool canvas = qobject_cast<ImageViewer *>(widget) != nullptr;
    const bool selectionCanvas = canvas && activeDocument()
        && (widget == activeDocument()->viewer || activeDocument()->viewMode() != DocumentWidget::ViewMode::Orthogonal);
    // Keep Tab/Enter and arrow navigation local to controls and ROI drafts.
    if ((key->key() == Qt::Key_Tab || key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
        && !canvas && top != this) return false;
    if (editing) {
        if (action && event->type() == QEvent::ShortcutOverride) { key->accept(); return true; }
        return false;
    }
    if (canvas && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
        && activeDocument() && activeDocument()->viewer->tool() == ImageViewer::Tool::Polygon) return false;
    if (selectionCanvas && key->key() >= Qt::Key_Left && key->key() <= Qt::Key_Down
        && !(key->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))) {
        if (auto roi = selectedRoi()) {
            if (event->type() == QEvent::ShortcutOverride) { key->accept(); return true; }
            const QPointF delta(key->key() == Qt::Key_Left ? -1 : key->key() == Qt::Key_Right ? 1 : 0,
                                key->key() == Qt::Key_Up ? -1 : key->key() == Qt::Key_Down ? 1 : 0);
            if (key->modifiers().testFlag(Qt::AltModifier) && (roi->type == RoiType::Rectangle || roi->type == RoiType::Ellipse)) {
                const auto end = roi->points[1] + delta;
                if (end.x() > roi->points[0].x() && end.y() > roi->points[0].y()) roi->points[1] = end;
            } else for (auto &point : roi->points) point += delta;
            activeDocument()->editRoi(*roi); return true;
        }
    }
    if (action) {
        if (event->type() == QEvent::ShortcutOverride) { key->accept(); return true; }
        if (action->isEnabled() && !key->isAutoRepeat()) action->trigger();
        return true;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::executeCommand(const QString &id)
{
    try {
        const auto &command = commands_.command(id);
        if (command.needsImage && !activeDocument()) return;
        const auto parameters = command.parameters.empty() ? std::optional<QVariantMap>(QVariantMap{}) : parameterForm(command);
        if (parameters) {
            commands_.execute(id, *parameters);
            if ((id.startsWith("process.") && id != "process.repeat" && id != "process.batch") || id == "edit.smooth" || id == "edit.invert") {
                repeatCommand_ = id; repeatParameters_ = *parameters;
            }
        }
    } catch (const std::exception &error) { showError(QString::fromUtf8(error.what())); }
}

std::optional<QVariantMap> MainWindow::parameterForm(const Command &command, bool batch)
{
    QDialog dialog(this); dialog.setWindowTitle(command.label); dialog.setMinimumWidth(380);
    auto *layout = new QFormLayout(&dialog);
    QMap<QString, QWidget *> editors;
    for (const auto &parameter : command.parameters) {
        if (batch && parameter.id == "scope") continue;
        QWidget *editor = nullptr;
        if (parameter.kind == ParameterKind::Real) {
            auto *spin = new QDoubleSpinBox(&dialog); spin->setDecimals(6);
            spin->setRange(parameter.minimum, parameter.maximum); spin->setValue(parameter.defaultValue.toDouble());
            if (const auto *doc = activeDocument()) {
                if (parameter.id == "lower") spin->setValue(doc->viewer->windowMinimum());
                if (parameter.id == "upper") spin->setValue(doc->viewer->windowMaximum());
                if (parameter.id.startsWith("spacing")) {
                    const int axis = parameter.id.endsWith('X') ? 0 : parameter.id.endsWith('Y') ? 1 : 2;
                    if (axis < int(doc->image.metadata().axes.size())) spin->setValue(doc->image.metadata().axes[axis].spacing);
                }
            }
            editor = spin;
        } else if (parameter.kind == ParameterKind::Integer) {
            auto *spin = new QSpinBox(&dialog); spin->setRange(int(parameter.minimum), int(parameter.maximum)); spin->setValue(parameter.defaultValue.toInt()); editor = spin;
        } else if (parameter.kind == ParameterKind::Boolean) {
            auto *check = new QCheckBox(&dialog); check->setChecked(parameter.defaultValue.toBool()); editor = check;
        } else {
            auto *combo = new QComboBox(&dialog); combo->addItems(parameter.choices); combo->setCurrentText(parameter.defaultValue.toString()); editor = combo;
        }
        editor->setObjectName(parameter.id); editors.insert(parameter.id, editor);
        layout->addRow(parameter.label, editor);
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog); layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return {};
    QVariantMap result;
    for (const auto &parameter : command.parameters) {
        auto *editor = editors.value(parameter.id);
        if (!editor) result.insert(parameter.id, parameter.defaultValue);
        else if (auto *spin = qobject_cast<QDoubleSpinBox *>(editor)) result.insert(parameter.id, spin->value());
        else if (auto *spin = qobject_cast<QSpinBox *>(editor)) result.insert(parameter.id, spin->value());
        else if (auto *check = qobject_cast<QCheckBox *>(editor)) result.insert(parameter.id, check->isChecked());
        else if (auto *combo = qobject_cast<QComboBox *>(editor)) result.insert(parameter.id, combo->currentText());
    }
    return result;
}

std::optional<Roi> MainWindow::selectedRoi() const
{
    const auto *document = activeDocument(); const auto index = rois_->currentRow();
    if (!document || document->selectionCleared) return {};
    auto roi = document->selection;
    if (!roi) {
        if (index < 0 || index >= int(document->rois.size())) return {};
        roi = document->rois[std::size_t(index)];
    }
    if (roi->channel != document->viewer->channel() || roi->time != document->viewer->time()) return {};
    // Use the same geometry on the current slice while retaining its original saved position.
    roi->z = document->viewer->slice();
    return roi;
}

void MainWindow::showPlot(bool profile)
{
    const auto *document = activeDocument(); if (!document) return;
    const auto roi = selectedRoi();
    if (profile && (!roi || roi->type != RoiType::Line)) { showError(tr("Draw and select a line ROI first.")); return; }
    const auto slice = document->image.slice(document->viewer->slice(), document->viewer->channel(), document->viewer->time());
    struct PlotData { std::vector<QPointF> points; QString xTitle, yTitle; };
    auto output = std::make_shared<PlotData>();
    jobs_.submit(profile ? tr("Line profile") : tr("Histogram"), [slice, roi, profile, output](auto stop, auto) {
        if (profile) {
            auto start = roi->points.front() - QPointF(.5, .5), end = roi->points.back() - QPointF(.5, .5);
            for (auto *point : {&start, &end}) { point->setX(std::clamp(point->x(), 0.0, double(slice.width() - 1))); point->setY(std::clamp(point->y(), 0.0, double(slice.height() - 1))); }
            const auto data = lineProfile(slice, start, end, 0, true, stop);
            for (std::size_t i = 0; i < data.distance.size(); ++i) output->points.emplace_back(data.distance[i], data.intensity[i]);
            output->xTitle = "Distance [" + data.distanceUnit + "]"; output->yTitle = data.intensityUnit.isEmpty() ? "Intensity" : data.intensityUnit;
        } else {
            const auto data = histogram(slice, 256, roi && roi->type != RoiType::Line ? roi : std::optional<Roi>{}, true, stop);
            for (std::size_t i = 0; i < data.centers.size(); ++i) output->points.emplace_back(data.centers[i], data.counts[i]);
            output->xTitle = "Intensity"; output->yTitle = "Pixel count";
        }
    }, [this, output, profile](auto error) {
        if (showJobError(error)) return;
        auto *window = plotWindow(profile);
        auto *plot = profile ? profilePlot_ : histogramPlot_;
        try { plot->setData(output->points, output->xTitle, output->yTitle); }
        catch (...) { showJobError(std::current_exception()); return; }
        window->showNormal(); window->raise(); window->activateWindow();
    });
}

void MainWindow::updateDisplayControls()
{
    if (loadingPanels_) return;
    QScopedValueRollback loading(loadingPanels_, true);
    const auto *doc = activeDocument();
    for (auto *widget : std::initializer_list<QWidget *>{minimum_, maximum_, gamma_, minimumSlider_, maximumSlider_, gammaSlider_, inverted_, linear_, composite_, channelIndex_, timeIndex_}) widget->setEnabled(doc != nullptr);
    lutMenu_->menuAction()->setEnabled(doc != nullptr);
    for (auto *action : lutMenu_->actions()) action->setChecked(doc && action->data().toString() == doc->viewer->lut());
    if (doc) {
        for (const auto &[spin, count, value] : {
                 std::tuple{channelIndex_, doc->image.channels(), doc->viewer->channel()},
                 std::tuple{timeIndex_, doc->image.frames(), doc->viewer->time()}}) {
            const QSignalBlocker blocker(spin);
            spin->setRange(1, int(std::min<std::size_t>(count, std::numeric_limits<int>::max())));
            spin->setValue(int(std::min<std::size_t>(value + 1, std::numeric_limits<int>::max())));
            spin->setSuffix(tr(" / %1").arg(count));
            spin->setEnabled(count > 1);
        }
        minimum_->setValue(doc->viewer->windowMinimum()); maximum_->setValue(doc->viewer->windowMaximum());
        gamma_->setValue(doc->viewer->gamma());
        const auto [sliceMinimum, sliceMaximum] = doc->viewer->sliceRange();
        sliderMinimum_ = std::clamp(std::min(sliceMinimum, minimum_->value()), minimum_->minimum(), minimum_->maximum());
        sliderMaximum_ = std::clamp(std::max(sliceMaximum, maximum_->value()), maximum_->minimum(), maximum_->maximum());
        for (const auto &[slider, spin] : {std::pair{minimumSlider_, minimum_}, std::pair{maximumSlider_, maximum_}}) {
            const QSignalBlocker blocker(slider);
            slider->setEnabled(sliderMaximum_ > sliderMinimum_);
            slider->setValue(sliderMaximum_ > sliderMinimum_
                ? int(std::lround(1000 * (spin->value() - sliderMinimum_) / (sliderMaximum_ - sliderMinimum_))) : 0);
        }
        const QSignalBlocker gammaBlocker(gammaSlider_);
        gammaSlider_->setValue(int(std::lround(500 + 500 * std::log(gamma_->value()) / std::log(20.))));
        inverted_->setChecked(doc->viewer->isInverted()); linear_->setChecked(doc->viewer->linearInterpolation()); composite_->setChecked(doc->viewer->composite());
    }
}

void MainWindow::filterProperties()
{
    const auto normalized = [](QString text) {
        static const QRegularExpression symbols(QStringLiteral("[^\\p{L}\\p{N}]"));
        return text.toCaseFolded().remove(symbols);
    };
    const auto query = normalized(metadataSearch_->text());
    const auto filter = [&](auto &&self, QTreeWidgetItem *item, bool ancestorMatches) -> bool {
        const bool matches = query.isEmpty() || normalized(item->text(0)).contains(query)
            || normalized(item->text(1)).contains(query);
        bool visible = ancestorMatches || matches;
        for (int index = 0; index < item->childCount(); ++index) {
            const bool childVisible = self(self, item->child(index), ancestorMatches || matches);
            visible = visible || childVisible;
        }
        item->setHidden(!visible);
        if (!query.isEmpty() && visible && item->childCount()) item->setExpanded(true);
        return visible;
    };
    for (int index = 0; index < metadata_->topLevelItemCount(); ++index)
        filter(filter, metadata_->topLevelItem(index), false);
    if (query.isEmpty() && metadata_->currentItem())
        metadata_->scrollToItem(metadata_->currentItem());
}

void MainWindow::updatePanels()
{
    if (loadingPanels_) return;
    updateDisplayControls();
    QScopedValueRollback loading(loadingPanels_, true);
    auto *doc = activeDocument();
    for (auto *action : imageActions_) action->setEnabled(doc != nullptr);
    findChild<QAction *>("stacks.reslice")->setEnabled(doc && doc->image.depth() > 1);
    findChild<QAction *>("stacks.project3d")->setEnabled(doc && doc->image.depth() > 1);
    const QStringList viewCommands{"view.single", "view.linked", "view.orthogonal"};
    for (int index = 0; index < viewCommands.size(); ++index) {
        auto *action = findChild<QAction *>(viewCommands[index]);
        action->setCheckable(true);
        action->setChecked(doc && int(doc->viewMode()) == index);
    }
    findChild<QAction *>("view.orthogonal")->setEnabled(doc && doc->image.depth() > 1);
    findChild<QAction *>("view.linked")->setEnabled(doc && documents().size() > 1);
    metadata_->clear(); history_->clear();
    const QString selectedId = rois_->currentItem() ? rois_->currentItem()->data(Qt::UserRole).toString() : QString{};
    QSignalBlocker roiSignals(rois_); rois_->clear();
    if (doc) {
        doc->setWindowTitle(doc->title());
        const auto &metadata = doc->image.metadata();
        const bool dicom = !metadata.dicomSequences.isEmpty() || std::any_of(metadata.attributes.keyBegin(), metadata.attributes.keyEnd(),
            [](const QString &key) { return key.startsWith("DICOM."); });
        const bool nifti = metadata.attributes.contains("nifti_type");
        if (!dicom && !nifti) {
            QStringList shape; for (const auto size : doc->image.shape()) shape << QString::number(size);
            const QList<QPair<QString, QString>> properties{{tr("Name"), metadata.name}, {tr("Pixel type"), pixelTypeName(doc->image.pixelType())},
                {tr("Shape X,Y,Z,C,T"), shape.join(" × ")}, {tr("Storage"), doc->image.storageKind()},
                {tr("Pixel data"), tr("%1 MiB").arg(double(doc->image.byteCount()) / (1024 * 1024), 0, 'f', 2)},
                {tr("Source"), metadata.sourcePath}, {tr("Intensity scale"), QString::number(metadata.intensityScale, 'g', 12)},
                {tr("Intensity offset"), QString::number(metadata.intensityOffset, 'g', 12)}, {tr("Intensity unit"), metadata.intensityUnit}};
            for (const auto &property : properties) new QTreeWidgetItem(metadata_, {property.first, property.second});
            for (const auto &axis : metadata.axes) {
                auto *item = new QTreeWidgetItem(metadata_, {tr("Axis %1").arg(axis.name), tr("%1 %2").arg(axis.spacing, 0, 'g', 8).arg(axis.unit)});
                new QTreeWidgetItem(item, {tr("Origin"), QString::number(axis.origin, 'g', 12)});
            }
            QStringList direction; for (const auto value : metadata.direction) direction << QString::number(value, 'g', 6);
            if (!direction.empty()) new QTreeWidgetItem(metadata_, {tr("Direction"), direction.join(", ")});
            for (const auto &channel : metadata.channels) new QTreeWidgetItem(metadata_, {tr("Channel"), channel.name});
        }
        QJsonObject attributes = metadata.dicomSequences;
        for (auto it = metadata.attributes.begin(); it != metadata.attributes.end(); ++it) attributes.insert(it.key(), it.value());
        const auto addAttributes = [dicom, nifti](auto &&self, QTreeWidgetItem *parent, const QJsonObject &attributes) -> void {
            for (auto it = attributes.begin(); it != attributes.end(); ++it) {
                QString label = it.key();
                if (nifti) {
                    static const QMap<QString, QString> fieldNames{
                        {"dim_info", tr("MRI Dimension Encoding")}, {"dim", tr("Dimensions")},
                        {"intent_p1", tr("Intent Parameter 1")}, {"intent_p2", tr("Intent Parameter 2")},
                        {"intent_p3", tr("Intent Parameter 3")}, {"intent_code", tr("Intent Code")},
                        {"datatype", tr("Data Type")}, {"bitpix", tr("Bits per Voxel")},
                        {"slice_start", tr("First Slice Index")}, {"pixdim", tr("Grid Spacing")},
                        {"vox_offset", tr("Pixel Data Offset")}, {"scl_slope", tr("Intensity Scale")},
                        {"scl_inter", tr("Intensity Offset")}, {"slice_end", tr("Last Slice Index")},
                        {"slice_code", tr("Slice Timing Order")}, {"xyzt_units", tr("Spatial and Time Units")},
                        {"cal_max", tr("Display Maximum")}, {"cal_min", tr("Display Minimum")},
                        {"slice_duration", tr("Slice Duration")}, {"toffset", tr("Time Offset")},
                        {"descrip", tr("Description")}, {"aux_file", tr("Auxiliary File")},
                        {"qform_code", tr("Q-form Code")}, {"sform_code", tr("S-form Code")},
                        {"quatern_b", tr("Quaternion B")}, {"quatern_c", tr("Quaternion C")},
                        {"quatern_d", tr("Quaternion D")}, {"qoffset_x", tr("Q-form X Offset")},
                        {"qoffset_y", tr("Q-form Y Offset")}, {"qoffset_z", tr("Q-form Z Offset")},
                        {"srow_x", tr("Affine Transform X Row")}, {"srow_y", tr("Affine Transform Y Row")},
                        {"srow_z", tr("Affine Transform Z Row")}, {"intent_name", tr("Intent Name")}};
                    const auto name = fieldNames.value(it.key().section('[', 0, 0));
                    if (name.isEmpty()) continue;
                    label = QStringLiteral("%1 (%2)").arg(name, it.key());
                    new QTreeWidgetItem(parent, {label, it.value().toString()});
                    continue;
                }
                static const QRegularExpression dicomTag(QStringLiteral("^DICOM\\.([0-9a-fA-F]{4}),([0-9a-fA-F]{4})$"));
                const auto match = dicomTag.match(label);
                if (dicom && !match.hasMatch()) continue;
                if (match.hasMatch()) {
                    DcmTag tag(match.captured(1).toUShort(nullptr, 16), match.captured(2).toUShort(nullptr, 16));
                    label = QStringLiteral("(%1,%2)").arg(match.captured(1), match.captured(2));
                    QString name = QString::fromLatin1(tag.getTagName());
                    if (name != QLatin1String(DcmTag_ERROR_TagName)) {
                        static const QRegularExpression wordBoundary(QStringLiteral("(?<=[a-z0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])"));
                        label.append(QLatin1Char(' ') + name.replace(wordBoundary, QStringLiteral(" ")));
                    }
                }
                auto *item = new QTreeWidgetItem(parent, {label, it.value().toString()});
                if (it.value().isArray()) {
                    item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
                    const auto items = it.value().toArray();
                    for (qsizetype index = 0; index < items.size(); ++index) {
                        auto *sequenceItem = new QTreeWidgetItem(item, {tr("Item %1").arg(index + 1)});
                        self(self, sequenceItem, items[index].toObject());
                    }
                }
            }
        };
        addAttributes(addAttributes, metadata_->invisibleRootItem(), attributes);
        metadata_->expandAll();
        filterProperties();
        for (const auto &roi : doc->rois) {
            auto *item = new QListWidgetItem(roi.name, rois_); item->setData(Qt::UserRole, roi.id);
            item->setToolTip(tr("Z %1 C %2 T %3").arg(roi.z + 1).arg(roi.channel + 1).arg(roi.time + 1));
            if (!doc->selectionCleared && roi.id == selectedId) rois_->setCurrentItem(item);
        }
        if (!doc->selectionCleared && rois_->currentRow() < 0 && rois_->count()) rois_->setCurrentRow(rois_->count() - 1);
        history_->addItems(doc->history.labels());
        history_->addItem(tr("Undo storage: %1 / %2 MiB").arg(double(doc->history.retainedBytes()) / (1024 * 1024), 0, 'f', 1).arg(doc->history.byteBudget() / (1024 * 1024)));
        if (auto *undo = findChild<QAction *>("edit.undo")) undo->setEnabled(doc->history.canUndo());
        if (auto *redo = findChild<QAction *>("edit.redo")) redo->setEnabled(doc->history.canRedo());
        if (auto *revert = findChild<QAction *>("file.revert")) revert->setEnabled(!doc->image.metadata().sourcePath.isEmpty());
    }
}

void MainWindow::updateRecentFiles()
{
    if (!recentMenu_) return;
    recentMenu_->clear(); recentMenu_->setEnabled(!recentFiles_.empty());
    for (const auto &path : recentFiles_) {
        auto *action = recentMenu_->addAction(QFileInfo(path).fileName()); action->setToolTip(path);
        connect(action, &QAction::triggered, this, [this, path] { openPaths({path}); });
    }
}

void MainWindow::updateWindowMenu()
{
    windowMenu_->clear();
    auto *launcher = windowMenu_->addAction(tr("ImageCXX toolbar"));
    connect(launcher, &QAction::triggered, this, [this] { showNormal(); raise(); activateWindow(); });
    windowMenu_->addSeparator();
    const QList<QPair<QString, QString>> entries{{"display", tr("Display")}, {"properties", tr("Image properties")},
        {"rois", tr("ROI manager")}, {"history", tr("History")}, {"measurements", tr("Measurements")},
        {"histogram", tr("Histogram")}, {"profile", tr("Line profile")}};
    for (const auto &[id, title] : entries) {
        auto *action = windowMenu_->addAction(title);
        action->setObjectName("window." + id);
        connect(action, &QAction::triggered, this, [this, id] {
            if (!panel(id) && activeDocument() && (id == "histogram" || id == "profile"))
                showPlot(id == "profile");
            else showPanel(id);
        });
    }
    if (!documents().isEmpty()) windowMenu_->addSeparator();
    for (auto *document : documents()) {
        auto *action = windowMenu_->addAction(document->title());
        action->setCheckable(true); action->setChecked(document == activeDocument());
        const QPointer<DocumentWidget> target(document);
        connect(action, &QAction::triggered, this, [this, target] {
            if (!target) return;
            setActiveDocument(target);
            target->showNormal(); target->raise(); target->activateWindow();
        });
        for (int index = 1; index < document->views().size(); ++index) {
            const QPointer<QWidget> viewWindow(document->views()[index]->window());
            auto *viewAction = windowMenu_->addAction(viewWindow->windowTitle());
            connect(viewAction, &QAction::triggered, this, [this, target, viewWindow] {
                if (!target || !viewWindow) return;
                setActiveDocument(target);
                viewWindow->showNormal(); viewWindow->raise(); viewWindow->activateWindow();
            });
        }
    }
    windowMenu_->addSeparator();
    windowMenu_->addAction(findChild<QAction *>("window.close"));
}

QWidget *MainWindow::plotWindow(bool profile)
{
    const QString id = profile ? QStringLiteral("profile") : QStringLiteral("histogram");
    if (auto *window = panel(id)) return window;
    auto *window = new QDialog(this, Qt::Window);
    window->setObjectName(profile ? "profileWindow" : "histogramWindow");
    window->setWindowTitle(profile ? tr("Line profile") : tr("Histogram"));
    window->resize(640, 360);
    window->setMinimumSize(420, 240);
    auto *layout = new QVBoxLayout(window);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *plot = new PlotWidget(window);
    layout->addWidget(plot);
    if (profile) profilePlot_ = plot;
    else histogramPlot_ = plot;
    panels_.insert(id, window);
    if (panelGeometries_.contains(id)) window->restoreGeometry(panelGeometries_.value(id));
    return window;
}

QByteArray MainWindow::panelLayout() const
{
    QJsonObject windows;
    for (auto it = panelGeometries_.cbegin(); it != panelGeometries_.cend(); ++it)
        windows.insert(it.key(), QJsonObject{{"geometry", QString::fromLatin1(it.value().toBase64())}});
    for (auto it = panels_.cbegin(); it != panels_.cend(); ++it)
        windows.insert(it.key(), QJsonObject{{"geometry", QString::fromLatin1(it.value()->saveGeometry().toBase64())}});
    return QJsonDocument(QJsonObject{{"format", "ImageCXX windows"}, {"version", 1}, {"panels", windows}}).toJson(QJsonDocument::Compact);
}

void MainWindow::restorePanelLayout(const QByteArray &layout)
{
    const auto root = QJsonDocument::fromJson(layout).object();
    if (root.value("format") != "ImageCXX windows" || root.value("version").toInt() != 1) return;
    const auto windows = root.value("panels").toObject();
    for (auto it = windows.begin(); it != windows.end(); ++it) {
        const auto state = it.value().toObject();
        const auto geometry = QByteArray::fromBase64(state.value("geometry").toString().toLatin1());
        panelGeometries_.insert(it.key(), geometry);
        auto *window = panel(it.key());
        if (!window) continue;
        if (!geometry.isEmpty()) window->restoreGeometry(geometry);
    }
}

bool MainWindow::showJobError(std::exception_ptr error)
{
    if (!error) return false;
    try { std::rethrow_exception(error); }
    catch (const std::exception &exception) { showError(QString::fromUtf8(exception.what())); }
    catch (...) { showError(tr("The background job failed with an unknown error.")); }
    return true;
}

void MainWindow::showError(const QString &message)
{
    QMessageBox::warning(this, tr("ImageCXX"), message);
}

bool MainWindow::confirmDiscardMeasurements()
{
    auto *window = panel("measurements");
    if (!window->isVisible() || results_->rowCount() <= exportedMeasurementRows_) return true;
    QStringList unsavedMeasurements;
    for (int row = exportedMeasurementRows_; row < results_->rowCount(); ++row) {
        auto name = results_->data(results_->index(row, 0)).toString();
        if (name.isEmpty()) name = tr("Untitled");
        unsavedMeasurements.append(tr("%1: %2").arg(name, results_->data(results_->index(row, 1)).toString()));
    }
    unsavedMeasurements.removeDuplicates();
    QMessageBox prompt(QMessageBox::Question, tr("Unsaved measurements"),
        tr("You have unsaved measurements that have not been exported to CSV:\n\n%1\n\nClose without exporting them?")
            .arg(unsavedMeasurements.join('\n')),
        QMessageBox::Discard | QMessageBox::Cancel, window);
    prompt.setTextFormat(Qt::PlainText);
    prompt.setDefaultButton(QMessageBox::Cancel);
    return prompt.exec() == QMessageBox::Discard;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    QStringList unsavedImages;
    for (const auto *doc : documents()) if (doc->modified)
        unsavedImages.append(doc->image.metadata().name.isEmpty() ? tr("Untitled") : doc->image.metadata().name);
    if (!unsavedImages.isEmpty()) {
        QMessageBox prompt(QMessageBox::Question, tr("Close ImageCXX"),
            tr("These images have unsaved pixel changes:\n\n%1\n\nClose without saving them?").arg(unsavedImages.join('\n')),
            QMessageBox::Discard | QMessageBox::Cancel, this);
        prompt.setTextFormat(Qt::PlainText);
        prompt.setDefaultButton(QMessageBox::Cancel);
        if (prompt.exec() != QMessageBox::Discard) { event->ignore(); return; }
    }
    if (!confirmDiscardMeasurements()) { event->ignore(); return; }
    if (jobs_.busy() && QMessageBox::question(this, tr("Background jobs running"), tr("Cancel running jobs and close ImageCXX?"),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) { event->ignore(); return; }
    jobs_.cancelAll();
    QSettings settings;
    settings.setValue("recentFiles", recentFiles_);
    settings.beginGroup("windowWorkflow/v1");
    settings.setValue("launcherGeometry", saveGeometry()); settings.setValue("panels", panelLayout());
    clearDocuments();
    for (auto *window : panels_) window->hide();
    event->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    for (const auto &url : event->mimeData()->urls())
        if (url.isLocalFile()) { event->acceptProposedAction(); return; }
}

void MainWindow::dropEvent(QDropEvent *event)
{
    QStringList paths;
    for (const auto &url : event->mimeData()->urls()) if (url.isLocalFile()) paths.append(url.toLocalFile());
    if (!paths.isEmpty()) { openPaths(paths); event->acceptProposedAction(); }
}
