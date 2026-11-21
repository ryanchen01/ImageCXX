#include "mainwindow.h"
#include "io/image_io.h"
#include "plots/gel_window.h"
#include "plots/plot_widget.h"
#include "ui/clipboard.h"
#include "ui/overlay_window.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QSet>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTableView>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

using namespace imagecxx;

void MainWindow::ensurePanel(const QString &id)
{
    if (panel(id)) return;
    if (id == "display") {
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
        minimum_->setObjectName("displayMinimum");
        maximum_->setObjectName("displayMaximum");
        gamma_ = new QDoubleSpinBox(displayPanel);
        gamma_->setObjectName("displayGamma");
        gamma_->setRange(.05, 20);
        gamma_->setSingleStep(.1);
        gamma_->setValue(1);
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
        form->addRow(linear_);
        form->addRow(composite_);
        form->addRow(autoRange);
        form->addRow(fitButton);
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
                doc->viewer->setLinearInterpolation(linear_->isChecked());
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
        for (auto *check : {linear_, composite_}) connect(check, &QCheckBox::toggled, this, applyDisplay);
    } else if (id == "properties") {
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
    } else if (id == "rois") {
        auto *roiPanel = new QDialog(this, Qt::Window);
        roiPanel->setWindowTitle(tr("ROI manager"));
        roiPanel->setObjectName("roiManagerWindow");
        roiPanel->resize(250, 320);
        panels_.insert("rois", roiPanel);
        auto *roiLayout = new QVBoxLayout(roiPanel);
        roiLayout->setContentsMargins(8, 8, 8, 8);
        rois_ = new QListWidget(roiPanel);
        rois_->setObjectName("roiList");
        rois_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        rois_->setToolTip(tr("Double-click a name to rename the ROI."));
        roiLayout->addWidget(rois_);
        auto *roiButtons = new QHBoxLayout;
        auto *measureButton = new QPushButton(tr("Measure..."), roiPanel);
        measureButton->setObjectName("roiMeasure");
        auto *measureMenu = new QMenu(measureButton);
        measureMenu->addAction(tr("Measure"), this, [this] { executeCommand("analyze.measure"); });
        measureMenu->addAction(tr("Multi Measure..."), this, [this] { executeCommand("analyze.multiMeasure"); });
        measureButton->setMenu(measureMenu);
        auto *removeRoi = new QPushButton(tr("Remove"), roiPanel);
        roiButtons->addWidget(measureButton);
        roiButtons->addWidget(removeRoi);
        roiLayout->addLayout(roiButtons);
        connect(removeRoi, &QPushButton::clicked, this, [this] {
            auto *doc = activeDocument();
            const int index = rois_->currentRow();
            if (!doc || index < 0) return;
            doc->rois.erase(doc->rois.begin() + index);
            doc->refreshRois();
            updatePanels();
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
            auto *doc = activeDocument();
            if (!doc || row < 0 || row >= int(doc->rois.size())) return;
            const auto roi = doc->rois[row];
            doc->viewer->setSlice(roi.z, roi.channel, roi.time);
            doc->setSelection(roi);
        });
    } else if (id == "history") {
        auto *historyPanel = new QDialog(this, Qt::Window);
        historyPanel->setWindowTitle(tr("History"));
        historyPanel->setObjectName("historyWindow");
        historyPanel->resize(320, 280);
        panels_.insert("history", historyPanel);
        auto *historyLayout = new QVBoxLayout(historyPanel);
        historyLayout->setContentsMargins(0, 0, 0, 0);
        history_ = new QListWidget(historyPanel);
        historyLayout->addWidget(history_);
    } else if (id == "measurements" || id == "multiMeasurements" || id == "multiMeasurementsPerSlice") {
        const bool multi = id != "measurements";
        auto *measurementsPanel = new QDialog(this, Qt::Window);
        measurementsPanel->setWindowTitle(multi ? (id == "multiMeasurementsPerSlice"
            ? tr("Multi Measure - one row per slice") : tr("Multi Measure")) : tr("Measurements"));
        measurementsPanel->setObjectName(id + "Window");
        measurementsPanel->resize(940, 300);
        panels_.insert(id, measurementsPanel);
        auto *measurementsLayout = new QVBoxLayout(measurementsPanel);
        measurementsLayout->setContentsMargins(0, 0, 0, 0);
        auto *table = new QTableView(measurementsPanel);
        table->setObjectName(multi ? "multiMeasurementTable" : "measurementTable");
        table->setModel(multi ? new QStandardItemModel(table) : results_);
        table->setAlternatingRowColors(true);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        measurementsLayout->addWidget(table);
        if (!multi) resultsView_ = table;
        else {
            auto *exportButton = new QPushButton(tr("Export CSV..."), measurementsPanel);
            exportButton->setObjectName("exportMultiMeasurements");
            measurementsLayout->addWidget(exportButton, 0, Qt::AlignRight);
            connect(exportButton, &QPushButton::clicked, this, [this, measurementsPanel] { exportMeasurements(measurementsPanel); });
        }
    } else if (id == "controlPanel") {
        auto *window = new QDialog(this, Qt::Window);
        window->setObjectName("controlPanel");
        window->setWindowTitle(tr("Control panel"));
        window->resize(460, 560);
        auto *layout = new QVBoxLayout(window);
        auto *filter = new QLineEdit(window);
        filter->setPlaceholderText(tr("Find command"));
        layout->addWidget(filter);
        auto *tree = new QTreeWidget(window);
        tree->setObjectName("controlPanelCommands");
        tree->setHeaderLabels({tr("Command"), tr("Shortcut")});
        layout->addWidget(tree);
        QMap<QString, QTreeWidgetItem *> categories;
        for (const auto &command : commands_.commands()) {
            auto *category = categories.value(command.category);
            if (!category) {
                category = new QTreeWidgetItem(tree, {QCoreApplication::translate("CommandCategories", command.category.toUtf8().constData())});
                categories.insert(command.category, category);
            }
            auto *item = new QTreeWidgetItem(category, {command.label, command.shortcut.toString(QKeySequence::NativeText)});
            item->setData(0, Qt::UserRole, command.id);
        }
        tree->expandAll();
        tree->resizeColumnToContents(0);
        connect(filter, &QLineEdit::textChanged, tree, [tree](const QString &text) {
            for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                auto *category = tree->topLevelItem(i);
                bool any = false;
                for (int j = 0; j < category->childCount(); ++j) {
                    auto *item = category->child(j);
                    const bool match = item->text(0).contains(text, Qt::CaseInsensitive);
                    item->setHidden(!match);
                    any |= match;
                }
                category->setHidden(!any);
            }
        });
        connect(tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
            if (auto *action = findChild<QAction *>(item->data(0, Qt::UserRole).toString()); action && action->isEnabled()) action->trigger();
        });
        panels_.insert("controlPanel", window);
    }
    if (auto *window = panel(id); window && panelGeometries_.contains(id))
        window->restoreGeometry(panelGeometries_.value(id));
}

void MainWindow::showPanel(const QString &id)
{
    ensurePanel(id);
    auto *window = panel(id);
    if (!window && (id == "histogram" || id == "profile")) window = plotWindow(id == "profile");
    if (!window) return;
    window->showNormal();
    updatePanels();
    window->raise();
    window->activateWindow();
}

void MainWindow::updateDisplayControls()
{
    if (loadingPanels_) return;
    QScopedValueRollback loading(loadingPanels_, true);
    const auto *doc = activeDocument();
    lutMenu_->menuAction()->setEnabled(doc != nullptr);
    for (auto *action : lutMenu_->actions())
        action->setChecked(doc && (action == invertLut_ ? doc->viewer->isInverted() : action->data().toString() == doc->viewer->lut()));
    if (!minimum_ || !panel("display")->isVisible()) return;
    for (auto *widget : std::initializer_list<QWidget *>{minimum_, maximum_, gamma_, minimumSlider_, maximumSlider_, gammaSlider_, linear_, composite_, channelIndex_, timeIndex_})
        widget->setEnabled(doc != nullptr);
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
        minimum_->setValue(doc->viewer->windowMinimum());
        maximum_->setValue(doc->viewer->windowMaximum());
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
        linear_->setChecked(doc->viewer->linearInterpolation());
        composite_->setChecked(doc->viewer->composite());
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
    if (auto *control = panel("controlPanel")) {
        QTreeWidgetItemIterator it(control->findChild<QTreeWidget *>("controlPanelCommands"));
        while (*it) {
            const auto id = (*it)->data(0, Qt::UserRole).toString();
            if (!id.isEmpty()) (*it)->setDisabled(commands_.command(id).needsImage && !doc);
            ++it;
        }
    }
    auto *overlay = activeOverlay();
    auto *overlayAction = findChild<QAction *>("view.overlay");
    auto *singleAction = findChild<QAction *>("view.single");
    auto *linkedAction = findChild<QAction *>("view.linked");
    auto *orthogonalAction = findChild<QAction *>("view.orthogonal");
    for (auto *action : {overlayAction, singleAction, linkedAction, orthogonalAction}) action->setCheckable(true);
    bool groupHas3d = doc && doc->image.depth() > 1;
    bool groupOrthogonal = doc && doc->orthogonalViews();
    if (doc) for (const auto &other : doc->linkedDocuments()) if (other) {
        groupHas3d |= other->image.depth() > 1;
        groupOrthogonal |= other->orthogonalViews();
    }
    overlayAction->setChecked(overlay != nullptr);
    singleAction->setChecked(doc && !overlay && !doc->isLinked() && !doc->orthogonalViews());
    linkedAction->setChecked(doc && !overlay && doc->isLinked());
    orthogonalAction->setChecked(overlay ? overlay->orthogonalViews() : groupOrthogonal);
    orthogonalAction->setEnabled(overlay ? overlay->document()->image.depth() > 1 : groupHas3d);
    linkedAction->setEnabled(doc && documents().size() > 1);
    for (auto *document : documents())
        for (auto *view : document->views()) view->setCacheActive(document == doc);
    const bool showProperties = metadata_ && panel("properties")->isVisible();
    const bool showRois = rois_ && panel("rois")->isVisible();
    const bool showHistory = history_ && panel("history")->isVisible();
    const QString selectedId = rois_ && rois_->currentItem() ? rois_->currentItem()->data(Qt::UserRole).toString() : QString{};
    QSet<QString> selectedIds;
    if (rois_) for (auto *item : rois_->selectedItems()) selectedIds.insert(item->data(Qt::UserRole).toString());
    QSignalBlocker roiSignals(rois_);
    bool sameRois = doc && rois_ && rois_->count() == int(doc->rois.size());
    if (sameRois) for (int row = 0; row < rois_->count(); ++row)
        if (rois_->item(row)->data(Qt::UserRole).toString() != doc->rois[std::size_t(row)].id) { sameRois = false; break; }
    if (showRois && !sameRois) rois_->clear();
    if (showHistory) history_->clear();
    if (!doc && showProperties) {
        metadata_->clear();
        propertiesMetadata_.reset();
        propertiesDocument_.clear();
    }
    if (doc) {
        doc->setWindowTitle(doc->title());
        if (showProperties && (propertiesDocument_ != doc || !propertiesMetadata_
            || *propertiesMetadata_ != doc->image.metadata() || propertiesShape_ != doc->image.shape()
            || propertiesType_ != doc->image.pixelType() || propertiesStorage_ != doc->image.storageKind())) {
            metadata_->clear();
            propertiesDocument_ = doc;
            propertiesMetadata_ = doc->image.metadata();
            propertiesShape_ = doc->image.shape();
            propertiesType_ = doc->image.pixelType();
            propertiesStorage_ = doc->image.storageKind();
            const auto &metadata = doc->image.metadata();
            const bool dicom = !metadata.dicomSequences.isEmpty() || std::any_of(metadata.attributes.keyBegin(), metadata.attributes.keyEnd(),
                [](const QString &key) { return key.startsWith("DICOM."); });
            const bool nifti = metadata.attributes.contains("nifti_type");
            if (!dicom && !nifti) {
                QStringList shape;
                for (const auto size : doc->image.shape()) shape << QString::number(size);
                const QList<QPair<QString, QString>> properties{{tr("Name"), metadata.name}, {tr("Pixel type"), pixelTypeName(doc->image.pixelType())},
                    {tr("Shape X,Y,Z,C,T"), shape.join(" × ")}, {tr("Storage"), doc->image.storageKind() == "Memory mapped" ? tr("Memory mapped") : doc->image.storageKind()},
                    {tr("Pixel data"), tr("%1 MiB").arg(double(doc->image.byteCount()) / (1024 * 1024), 0, 'f', 2)},
                    {tr("Source"), metadata.sourcePath}, {tr("Intensity scale"), QString::number(metadata.intensityScale, 'g', 12)},
                    {tr("Intensity offset"), QString::number(metadata.intensityOffset, 'g', 12)}, {tr("Intensity unit"), metadata.intensityUnit}};
                for (const auto &property : properties) new QTreeWidgetItem(metadata_, {property.first, property.second});
                for (const auto &axis : metadata.axes) {
                    auto *item = new QTreeWidgetItem(metadata_, {tr("Axis %1").arg(axis.name), tr("%1 %2").arg(axis.spacing, 0, 'g', 8).arg(axis.unit)});
                    new QTreeWidgetItem(item, {tr("Origin"), QString::number(axis.origin, 'g', 12)});
                }
                QStringList direction;
                for (const auto value : metadata.direction) direction << QString::number(value, 'g', 6);
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
                    const auto formattedDicom = formatDicomTag(label);
                    if (dicom && formattedDicom.isEmpty()) continue;
                    if (!formattedDicom.isEmpty()) label = formattedDicom;
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
        }
        if (showRois) {
            for (std::size_t row = 0; row < doc->rois.size(); ++row) {
                const auto &roi = doc->rois[row];
                auto *item = sameRois ? rois_->item(int(row)) : new QListWidgetItem(rois_);
                item->setText(roi.name);
                item->setData(Qt::UserRole, roi.id);
                item->setToolTip(tr("Z %1 C %2 T %3").arg(roi.z + 1).arg(roi.channel + 1).arg(roi.time + 1));
                if (!sameRois && !doc->selectionCleared) {
                    if (roi.id == selectedId) rois_->setCurrentItem(item, QItemSelectionModel::NoUpdate);
                    item->setSelected(selectedIds.contains(roi.id));
                }
            }
            if (!doc->selectionCleared && rois_->currentRow() < 0 && rois_->count()) rois_->setCurrentRow(rois_->count() - 1);
            if (doc->selectionCleared) { rois_->clearSelection(); rois_->setCurrentRow(-1); }
        }
        if (showHistory) {
            history_->addItems(doc->history.labels());
            history_->addItem(tr("Undo storage: %1 / %2 MiB").arg(double(doc->history.retainedBytes()) / (1024 * 1024), 0, 'f', 1).arg(doc->history.byteBudget() / (1024 * 1024)));
        }
        if (auto *undo = findChild<QAction *>("edit.undo")) undo->setEnabled(doc->history.canUndo());
        if (auto *redo = findChild<QAction *>("edit.redo")) redo->setEnabled(doc->history.canRedo());
        if (auto *revert = findChild<QAction *>("file.revert")) revert->setEnabled(!doc->image.metadata().sourcePath.isEmpty());
    }
}

void MainWindow::updateRecentFiles()
{
    if (!recentMenu_) return;
    recentMenu_->clear();
    recentMenu_->setEnabled(!recentFiles_.empty());
    for (const auto &path : recentFiles_) {
        auto *action = recentMenu_->addAction(QFileInfo(path).fileName());
        action->setToolTip(path);
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
    for (const auto &id : {"multiMeasurements", "multiMeasurementsPerSlice"}) if (auto *window = panel(id)) {
        auto *action = windowMenu_->addAction(window->windowTitle());
        connect(action, &QAction::triggered, this, [this, id] { showPanel(id); });
    }
    if (!documents().isEmpty()) windowMenu_->addSeparator();
    for (auto *document : documents()) {
        auto *action = windowMenu_->addAction(document->title());
        action->setCheckable(true);
        action->setChecked(document == activeDocument());
        const QPointer<DocumentWidget> target(document);
        connect(action, &QAction::triggered, this, [this, target] {
            if (!target) return;
            setActiveDocument(target);
            target->showNormal();
            target->raise();
            target->activateWindow();
        });
        for (int index = 1; index < document->views().size(); ++index) {
            const QPointer<QWidget> viewWindow(document->views()[index]->window());
            auto *viewAction = windowMenu_->addAction(viewWindow->windowTitle());
            connect(viewAction, &QAction::triggered, this, [this, target, viewWindow] {
                if (!target || !viewWindow) return;
                setActiveDocument(target);
                viewWindow->showNormal();
                viewWindow->raise();
                viewWindow->activateWindow();
            });
        }
    }
    for (auto *overlay : findChildren<OverlayWindow *>(QString{}, Qt::FindDirectChildrenOnly)) {
        auto *action = windowMenu_->addAction(overlay->windowTitle());
        connect(action, &QAction::triggered, overlay, &OverlayWindow::showEditor);
        for (int index = 1; index < overlay->views().size(); ++index) {
            const QPointer<QWidget> viewWindow(overlay->views()[index]->window());
            const QPointer<OverlayWindow> target(overlay);
            auto *viewAction = windowMenu_->addAction(viewWindow->windowTitle());
            connect(viewAction, &QAction::triggered, this, [this, target, viewWindow] {
                if (!target || !viewWindow) return;
                target->showViews();
                setActiveOverlay(target);
                viewWindow->showNormal();
                viewWindow->raise();
                viewWindow->activateWindow();
            });
        }
    }
    for (auto *plot : findChildren<QDialog *>("roiMeasurementsWindow") + findChildren<QDialog *>("scatterPlotWindow")) {
        auto *action = windowMenu_->addAction(plot->windowTitle());
        const QPointer<QDialog> target(plot);
        connect(action, &QAction::triggered, this, [target] {
            if (target) { target->showNormal(); target->raise(); target->activateWindow(); }
        });
    }
    for (auto *plot : findChildren<GelWindow *>()) {
        auto *action = windowMenu_->addAction(plot->windowTitle());
        connect(action, &QAction::triggered, plot, [plot] {
            plot->showNormal();
            plot->raise();
            plot->activateWindow();
        });
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
