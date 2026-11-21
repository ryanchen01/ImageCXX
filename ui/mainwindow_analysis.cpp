#include "mainwindow.h"
#include "core/csv.h"
#include "plots/gel_window.h"
#include "plots/plot_widget.h"
#include "processing/processing.h"
#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHash>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTableView>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>

using namespace imagecxx;

namespace {
QList<QVariant> measurementValues(const Measurement &m)
{
    return {QVariant::fromValue<qulonglong>(m.count), m.area, m.mean, m.standardDeviation,
        m.minimum, m.maximum, m.sum, m.areaUnit, m.intensityUnit};
}
} // namespace

void MainWindow::registerAnalysisCommands()
{
    results_ = new QStandardItemModel(0, 11, this);
    const QStringList measurementHeaders{tr("Pixels"), tr("Area"), tr("Mean"), tr("Std. dev."),
        tr("Min"), tr("Max"), tr("Sum"), tr("Area unit"), tr("Intensity unit")};
    results_->setHorizontalHeaderLabels(QStringList{tr("Image"), tr("ROI / slice")} + measurementHeaders);

    commands_.add({"analyze.measure", tr("Measure selection"), QT_TRANSLATE_NOOP("CommandCategories", "Analyze"), QKeySequence("M"), true, {}, [this](const QVariantMap &) {
        const auto *doc = activeDocument();
        if (!doc) return;
        const auto slice = doc->image.slice(doc->viewer->slice(), doc->viewer->channel(), doc->viewer->time());
        const auto roi = selectedRoi();
        const auto sliceIndex = doc->viewer->slice() + 1;
        const QString location = roi ? roi->name : tr("Slice Z%1 C%2 T%3").arg(doc->viewer->slice() + 1).arg(doc->viewer->channel() + 1).arg(doc->viewer->time() + 1);
        auto output = std::make_shared<Measurement>();
        jobs_.submit(tr("Measuring"), [slice, roi, output](auto stop, auto) { *output = measure(slice, roi, true, stop); },
            [this, output, location, sliceIndex](auto error) {
                if (showJobError(error)) return;
                const auto &m = *output;
                QList<QStandardItem *> row;
                const auto values = QList<QVariant>{m.imageName, location} + measurementValues(m);
                for (const auto &value : values) { auto *item = new QStandardItem; item->setData(value, Qt::DisplayRole); row.append(item); }
                row[1]->setData(QVariant::fromValue<qulonglong>(sliceIndex), Qt::UserRole);
                ensurePanel("measurements");
                results_->appendRow(row);
                showPanel("measurements");
            });
    }});
    commands_.add({"analyze.multiMeasure", tr("Multi Measure..."), QT_TRANSLATE_NOOP("CommandCategories", "Analyze"), {}, true, {},
        [this, measurementHeaders](const QVariantMap &) {
        const auto *doc = activeDocument();
        if (!doc) return;
        std::vector<Roi> rois;
        if (rois_ && panel("rois")->isVisible()) for (int row = 0; row < rois_->count(); ++row) {
            if (!rois_->item(row)->isSelected()) continue;
            auto roi = doc->rois[std::size_t(row)];
            if (roi.name.isEmpty()) roi.name = tr("ROI %1").arg(row + 1);
            rois.push_back(std::move(roi));
        }
        if (rois.empty()) { showError(tr("Select one or more ROIs in the ROI manager first.")); return; }
        const auto image = doc->image;
        const auto planeCount = image.depth() * image.channels() * image.frames();
        const auto currentPlane = doc->viewer->channel() + image.channels()
            * (doc->viewer->slice() + image.depth() * doc->viewer->time());
        QDialog dialog(this);
        dialog.setObjectName("multiMeasureDialog");
        dialog.setWindowTitle(tr("Multi Measure"));
        auto *layout = new QVBoxLayout(&dialog);
        auto *allSlices = new QCheckBox(tr("Measure all %1 slices").arg(planeCount), &dialog);
        allSlices->setObjectName("measureAllSlices");
        allSlices->setChecked(true);
        allSlices->setEnabled(planeCount > 1);
        allSlices->setToolTip(tr("Includes all channels and time frames."));
        auto *oneRow = new QCheckBox(tr("One row per slice"), &dialog);
        oneRow->setObjectName("oneRowPerSlice");
        oneRow->setToolTip(tr("Put each selected ROI's measurements in separate columns on the same row."));
        auto *append = new QCheckBox(tr("Append results"), &dialog);
        append->setObjectName("appendResults");
        layout->addWidget(allSlices);
        layout->addWidget(oneRow);
        layout->addWidget(append);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted) return;
        const bool wide = oneRow->isChecked(), appendResults = append->isChecked();
        const auto count = allSlices->isChecked() ? planeCount : std::size_t(1);
        const auto firstPlane = allSlices->isChecked() ? std::size_t(0) : currentPlane;
        QStringList headers{tr("Image"), tr("Slice"), tr("Channel"), tr("Frame")};
        QStringList keys{"image", "slice", "channel", "frame"};
        if (wide) {
            for (std::size_t i = 0; i < rois.size(); ++i) for (int field = 0; field < measurementHeaders.size(); ++field) {
                headers.append(measurementHeaders[field]);
                keys.append(rois[i].id + '/' + QString::number(field));
            }
        } else {
            headers.insert(1, tr("ROI"));
            keys.insert(1, "roi");
            headers += measurementHeaders;
            for (int field = 0; field < measurementHeaders.size(); ++field) keys.append(QString::number(field));
        }
        auto output = std::make_shared<QList<QList<QVariant>>>();
        jobs_.submit(tr("Multi Measure"), [image, rois, firstPlane, count, wide, output](auto stop, auto progress) {
            for (std::size_t plane = firstPlane; plane < firstPlane + count; ++plane) {
                const auto channel = plane % image.channels();
                const auto z = (plane / image.channels()) % image.depth();
                const auto time = plane / (image.channels() * image.depth());
                const auto slice = image.slice(z, channel, time);
                const QList<QVariant> position{image.metadata().name, QVariant::fromValue<qulonglong>(z + 1),
                    QVariant::fromValue<qulonglong>(channel + 1), QVariant::fromValue<qulonglong>(time + 1)};
                auto row = position;
                for (std::size_t i = 0; i < rois.size(); ++i) {
                    const auto m = measure(slice, rois[i], true, stop);
                    if (wide) row += measurementValues(m);
                    else {
                        auto values = position;
                        values.insert(1, rois[i].name);
                        output->append(values + measurementValues(m));
                    }
                    progress((double(plane - firstPlane) + double(i + 1) / rois.size()) / count);
                }
                if (wide) output->append(std::move(row));
            }
        }, [this, output, wide, appendResults, headers, keys, rois, measurementHeaders](auto error) {
            if (showJobError(error)) return;
            const QString id = wide ? "multiMeasurementsPerSlice" : "multiMeasurements";
            ensurePanel(id);
            auto *window = panel(id);
            auto *model = qobject_cast<QStandardItemModel *>(window->findChild<QTableView *>()->model());
            if (!appendResults) { model->clear(); window->setProperty("exportedRows", 0); }
            QHash<QString, int> columns;
            for (int column = 0; column < model->columnCount(); ++column)
                columns.insert(model->headerData(column, Qt::Horizontal, Qt::UserRole).toString(), column);
            QList<int> destinations;
            for (int column = 0; column < keys.size(); ++column) {
                if (!columns.contains(keys[column])) {
                    columns.insert(keys[column], model->columnCount());
                    model->setColumnCount(model->columnCount() + 1);
                }
                const int destination = columns.value(keys[column]);
                destinations.append(destination);
                auto heading = headers[column];
                if (wide && column >= 4) {
                    // Retained column groups give each ROI a stable number across appends.
                    const auto number = (destination - 4) / measurementHeaders.size() + 1;
                    const auto &roi = rois[std::size_t((column - 4) / measurementHeaders.size())];
                    heading = tr("%1 [%2]").arg(heading, tr("%1: %2").arg(number).arg(roi.name));
                }
                model->setHeaderData(destination, Qt::Horizontal, heading);
                model->setHeaderData(destination, Qt::Horizontal, keys[column], Qt::UserRole);
            }
            for (const auto &values : *output) {
                const int row = model->rowCount();
                model->insertRow(row);
                for (int column = 0; column < values.size(); ++column)
                    model->setData(model->index(row, destinations[column]), values[column]);
            }
            showPanel(id);
        });
    }});
    commands_.add({"analyze.histogram", tr("Histogram"), QT_TRANSLATE_NOOP("CommandCategories", "Analyze"), QKeySequence("H"), true, {}, [this](const QVariantMap &) { showPlot(false); }});
    commands_.add({"analyze.profile", tr("Line profile"), QT_TRANSLATE_NOOP("CommandCategories", "Analyze"), QKeySequence("K"), true, {}, [this](const QVariantMap &) { showPlot(true); }});
    commands_.add({"analyze.histogram.options", tr("Histogram options..."), QT_TRANSLATE_NOOP("CommandCategories", "Analyze"), QKeySequence("Alt+H"), true,
        {{"bins", tr("Bins"), ParameterKind::Integer, 256, 1, 65536, {}},
         {"calibrated", tr("Calibrated intensity"), ParameterKind::Boolean, true, 0, 1, {}},
         {"log", tr("Logarithmic counts"), ParameterKind::Boolean, false, 0, 1, {}}},
        [this](const QVariantMap &options) { showPlot(false, options); }});
    commands_.add({"analyze.histogram.log", tr("Log histogram"), QT_TRANSLATE_NOOP("CommandCategories", "Analyze"), QKeySequence("Shift+H"), true, {},
        [this](const QVariantMap &) { showPlot(false, {{"log", true}}); }});
    commands_.add({"analyze.profile.options", tr("Profile options..."), QT_TRANSLATE_NOOP("CommandCategories", "Analyze"), QKeySequence("Alt+K"), true,
        {{"samples", tr("Line samples (0 = automatic)"), ParameterKind::Integer, 0, 0, 1048576, {}},
         {"calibrated", tr("Calibrated intensity"), ParameterKind::Boolean, true, 0, 1, {}},
         {"rows", tr("Average rows of rectangular selection"), ParameterKind::Boolean, true, 0, 1, {}}},
        [this](const QVariantMap &options) { showPlot(true, options); }});
    for (bool first : {true, false}) {
        commands_.add({first ? "gel.first" : "gel.next", first ? tr("Select first lane") : tr("Select next lane"),
            QT_TRANSLATE_NOOP("CommandCategories", "Gels"), QKeySequence(first ? "1" : "2"), true, {}, [this, first](const QVariantMap &) {
            auto *doc = activeDocument();
            if (!doc) return;
            auto roi = selectedRoi();
            if (!roi || roi->type != RoiType::Rectangle || roi->points.size() != 2) {
                showError(tr("Select a rectangular lane first."));
                return;
            }
            const auto bounds = roi->bounds();
            if (!std::isfinite(bounds.left()) || !std::isfinite(bounds.top())
                || !std::isfinite(bounds.right()) || !std::isfinite(bounds.bottom())
                || bounds.isEmpty() || bounds.left() < 0 || bounds.top() < 0
                || bounds.right() > double(doc->image.width()) || bounds.bottom() > double(doc->image.height())) {
                showError(tr("Select a rectangular lane entirely inside the image."));
                return;
            }
            if (componentCount(doc->image.pixelType()) != 1) {
                showError(tr("Select a scalar channel before analyzing gels."));
                return;
            }
            if (!first) {
                if (doc->gelLanes.empty()) { showError(tr("Select the first lane before adding another.")); return; }
                const auto &previous = doc->gelLanes.front();
                const auto original = previous.bounds();
                if (roi->z != previous.z || roi->channel != previous.channel || roi->time != previous.time
                    || std::abs(bounds.width() - original.width()) > 1e-6
                    || std::abs(bounds.height() - original.height()) > 1e-6
                    || std::abs((doc->gelHorizontal ? bounds.left() : bounds.top())
                                - (doc->gelHorizontal ? original.left() : original.top())) > 1e-6) {
                    showError(tr("Use lanes of the same size and lengthwise alignment on the same Z/C/T plane."));
                    return;
                }
                for (const auto &lane : doc->gelLanes) if (lane.bounds().intersects(bounds)) {
                    showError(tr("The lane overlaps an existing lane."));
                    return;
                }
            }
            if (first) doc->gelLanes.clear();
            roi->name = tr("Lane %1").arg(doc->gelLanes.size() + 1);
            roi->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            doc->gelLanes.push_back(*roi);
            doc->refreshRois();
            statusBar()->showMessage(tr("%1 selected (%2 lanes). Move the rectangle to the next lane and press 2.")
                .arg(roi->name).arg(doc->gelLanes.size()), 5000);
        }});
    }
    commands_.add({"gel.options", tr("Gel analyzer options..."), QT_TRANSLATE_NOOP("CommandCategories", "Gels"), {}, true,
        {{"orientation", tr("Lane direction"), ParameterKind::Choice, QT_TRANSLATE_NOOP("CommandChoices", "Vertical"), 0, 0, {QT_TRANSLATE_NOOP("CommandChoices", "Vertical"), QT_TRANSLATE_NOOP("CommandChoices", "Horizontal")}},
         {"darkBands", tr("Dark bands on a light background"), ParameterKind::Boolean, true, 0, 0, {}}}, [this](const QVariantMap &p) {
        auto *doc = activeDocument();
        if (!doc) return;
        const bool horizontal = p.value("orientation").toString() == "Horizontal";
        if (horizontal != doc->gelHorizontal) { doc->gelLanes.clear(); doc->refreshRois(); }
        doc->gelHorizontal = horizontal;
        doc->gelDarkBands = p.value("darkBands").toBool();
    }});
    commands_.add({"gel.plot", tr("Plot lanes"), QT_TRANSLATE_NOOP("CommandCategories", "Gels"), QKeySequence("3"), true, {}, [this](const QVariantMap &) {
        auto *doc = activeDocument();
        if (!doc) return;
        if (doc->gelLanes.empty()) { showError(tr("Select the first lane before plotting.")); return; }
        const auto lanes = doc->gelLanes;
        const auto &first = lanes.front();
        const auto slice = doc->image.slice(first.z, first.channel, first.time);
        const bool horizontal = doc->gelHorizontal, dark = doc->gelDarkBands;
        const QString title = tr("%1: gel lanes, Z%2, C%3, T%4").arg(doc->title())
            .arg(first.z + 1).arg(first.channel + 1).arg(first.time + 1);
        auto output = std::make_shared<std::vector<LineProfile>>();
        jobs_.submit(tr("Plotting gel lanes"), [slice, lanes, horizontal, dark, output](auto stop, auto progress) {
            for (const auto &lane : lanes) {
                output->push_back(gelLaneProfile(slice, lane, horizontal, dark, stop));
                progress(double(output->size()) / lanes.size());
            }
        }, [this, output, title, dark](auto error) {
            if (showJobError(error)) return;
            GelWindow *window = nullptr;
            try { window = new GelWindow(std::move(*output), title, dark, this); }
            catch (...) { showJobError(std::current_exception()); return; }
            window->show();
            window->raise();
            window->activateWindow();
        });
    }});
    commands_.add({"gel.label", tr("Label peaks"), QT_TRANSLATE_NOOP("CommandCategories", "Gels"), {}, false, {}, [this](const QVariantMap &) {
        auto *window = qobject_cast<GelWindow *>(QApplication::activeWindow());
        if (!window) {
            const auto plots = findChildren<GelWindow *>();
            if (!plots.empty()) window = plots.back();
        }
        if (!window) { showError(tr("Plot gel lanes and measure bands first.")); return; }
        window->labelPeaks();
        window->showNormal();
        window->raise();
        window->activateWindow();
    }});
    commands_.add({"analyze.roiPlot", tr("Plot ROI measurements..."), QT_TRANSLATE_NOOP("CommandCategories", "Analyze"), {}, true,
        {{"axis", tr("Axis"), ParameterKind::Choice, QT_TRANSLATE_NOOP("CommandChoices", "Time"), 0, 0, {QT_TRANSLATE_NOOP("CommandChoices", "Time"), QT_TRANSLATE_NOOP("CommandChoices", "Z")}}}, [this](const QVariantMap &p) {
        const auto *doc = activeDocument();
        if (!doc) return;
        const auto roi = selectedRoi();
        if (!roi || roi->type == RoiType::Line) { showError(tr("Select an area ROI first.")); return; }
        const bool overTime = p.value("axis").toString() == "Time";
        const auto image = doc->image;
        const auto count = overTime ? image.frames() : image.depth();
        if (count < 2) { showError(overTime ? tr("The image needs at least two time frames.")
                                         : tr("The image needs at least two Z slices.")); return; }
        const auto z = doc->viewer->slice(), channel = doc->viewer->channel(), time = doc->viewer->time();
        const auto &axis = image.metadata().axes[overTime ? 4 : 2];
        const bool calibrated = !axis.unit.isEmpty() && axis.unit != "px";
        const QString xTitle = calibrated ? (overTime ? tr("Time [%1]") : tr("Z [%1]")).arg(axis.unit)
                                         : (overTime ? tr("Frame") : tr("Slice"));
        const QString yTitle = image.metadata().intensityUnit.isEmpty() ? tr("Mean intensity")
            : tr("Mean intensity [%1]").arg(image.metadata().intensityUnit);
        const QString roiName = roi->name.isEmpty() ? tr("Selection") : roi->name;
        const QString title = tr("%1: %2, mean vs %3, C%4, %5%6").arg(doc->title(), roiName,
            overTime ? tr("time") : tr("Z")).arg(channel + 1).arg(overTime ? "Z" : "T").arg((overTime ? z : time) + 1);
        struct RoiPlotData { std::vector<QPointF> points; QByteArray csv; };
        auto output = std::make_shared<RoiPlotData>();
        jobs_.submit(tr("Measuring ROI series"),
            [image, roi, roiName, overTime, calibrated, z, channel, time, count, xTitle, output](auto stop, auto progress) {
                const auto &axis = image.metadata().axes[overTime ? 4 : 2];
                const auto appendRow = [&output](QStringList fields) {
                    for (auto &field : fields) field = detail::spreadsheetCsvCell(std::move(field));
                    output->csv += fields.join(',').toUtf8() + '\n';
                };
                const auto number = [](double value) { return QString::number(value, 'g', std::numeric_limits<double>::max_digits10); };
                appendRow({tr("Image"), tr("ROI"), tr("Z"), tr("Channel"), tr("Frame"), xTitle, tr("Count"), tr("Area"), tr("Mean"),
                    tr("StdDev"), tr("Min"), tr("Max"), tr("Sum"), tr("Area unit"), tr("Intensity unit")});
                output->points.reserve(count);
                for (std::size_t i = 0; i < count; ++i) {
                    // Reuse the selected XY geometry on every plane; other axes stay fixed.
                    const auto sliceZ = overTime ? z : i, frame = overTime ? i : time;
                    const auto m = measure(image.slice(sliceZ, channel, frame), roi, true, stop);
                    const double position = calibrated ? axis.origin + double(i) * axis.spacing : double(i + 1);
                    if (!std::isfinite(position))
                        throw std::invalid_argument(QCoreApplication::translate("MainWindow", "The series axis coordinates are not finite.").toStdString());
                    output->points.emplace_back(position, m.mean);
                    appendRow({m.imageName, roiName, QString::number(sliceZ + 1), QString::number(channel + 1),
                        QString::number(frame + 1), number(position), QString::number(m.count), number(m.area),
                        number(m.mean), number(m.standardDeviation), number(m.minimum), number(m.maximum),
                        number(m.sum), m.areaUnit, m.intensityUnit});
                    progress(double(i + 1) / double(count));
                }
            }, [this, output, title, xTitle, yTitle](auto error) {
                if (showJobError(error)) return;
                auto *window = new QDialog(this, Qt::Window);
                window->setAttribute(Qt::WA_DeleteOnClose);
                window->setObjectName("roiMeasurementsWindow");
                window->setWindowTitle(title);
                window->resize(640, 400);
                window->setMinimumSize(420, 260);
                auto *layout = new QVBoxLayout(window);
                layout->setContentsMargins(0, 0, 0, 0);
                auto *plot = new PlotWidget(window);
                layout->addWidget(plot);
                try { plot->setData(output->points, xTitle, yTitle); }
                catch (...) { delete window; showJobError(std::current_exception()); return; }
                auto *exportButton = new QPushButton(tr("Export CSV..."), window);
                exportButton->setObjectName("exportRoiMeasurements");
                layout->addWidget(exportButton, 0, Qt::AlignRight);
                connect(exportButton, &QPushButton::clicked, window, [this, window, output] {
                    const auto path = QFileDialog::getSaveFileName(window, tr("Export ROI measurements"),
                        "roi-measurements.csv", tr("CSV (*.csv)"));
                    if (path.isEmpty()) return;
                    QSaveFile file(path);
                    if (!file.open(QIODevice::WriteOnly) || file.write(output->csv) != output->csv.size() || !file.commit())
                        showError(tr("Cannot write ROI measurements."));
                });
                window->show();
                window->raise();
                window->activateWindow();
            });
    }});
    const QStringList measurementColumns{QT_TRANSLATE_NOOP("CommandChoices", "Index"),
        QT_TRANSLATE_NOOP("CommandChoices", "Slice"), QT_TRANSLATE_NOOP("CommandChoices", "Pixels"),
        QT_TRANSLATE_NOOP("CommandChoices", "Area"), QT_TRANSLATE_NOOP("CommandChoices", "Mean"),
        QT_TRANSLATE_NOOP("CommandChoices", "StdDev"), QT_TRANSLATE_NOOP("CommandChoices", "Min"),
        QT_TRANSLATE_NOOP("CommandChoices", "Max"), QT_TRANSLATE_NOOP("CommandChoices", "Sum")};
    commands_.add({"analyze.scatter", tr("Scatter plot..."), QT_TRANSLATE_NOOP("CommandCategories", "Analyze"), {}, false,
        {{"x", tr("X axis"), ParameterKind::Choice, "Area", 0, 0, measurementColumns},
         {"y", tr("Y axis"), ParameterKind::Choice, "Mean", 0, 0, measurementColumns}},
        [this, measurementColumns](const QVariantMap &p) {
        if (results_->rowCount() == 0) { showError(tr("Measure an image or selection first.")); return; }
        const int xColumn = measurementColumns.indexOf(p.value("x").toString());
        const int yColumn = measurementColumns.indexOf(p.value("y").toString());
        const auto value = [this](int row, int column) {
            return column == 0 ? row + 1 : results_->data(results_->index(row, column),
                column == 1 ? Qt::UserRole : Qt::DisplayRole).toDouble();
        };
        std::vector<QPointF> points;
        points.reserve(results_->rowCount());
        for (int row = 0; row < results_->rowCount(); ++row) {
            const double x = value(row, xColumn);
            const double y = value(row, yColumn);
            if (std::isfinite(x) && std::isfinite(y)) points.emplace_back(x, y);
        }
        if (points.empty()) { showError(tr("The selected columns have no finite measurement pairs.")); return; }
        const auto xTitle = QCoreApplication::translate("CommandChoices", qPrintable(measurementColumns[xColumn]));
        const auto yTitle = QCoreApplication::translate("CommandChoices", qPrintable(measurementColumns[yColumn]));
        auto *window = new QDialog(this, Qt::Window);
        window->setAttribute(Qt::WA_DeleteOnClose);
        window->setObjectName("scatterPlotWindow");
        window->setWindowTitle(tr("Scatter plot: %1 vs %2").arg(yTitle, xTitle));
        window->resize(640, 400);
        window->setMinimumSize(420, 260);
        auto *layout = new QVBoxLayout(window);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *plot = new PlotWidget(window, true);
        layout->addWidget(plot);
        try { plot->setData(points, xTitle, yTitle); }
        catch (...) { delete window; showJobError(std::current_exception()); return; }
        window->show();
        window->raise();
        window->activateWindow();
    }});
    commands_.add({"analyze.mask", tr("Create mask from ROI"), QT_TRANSLATE_NOOP("CommandCategories", "Analyze"), {}, true, {}, [this](const QVariantMap &) {
        auto *doc = activeDocument();
        const auto roi = selectedRoi();
        if (!doc || !roi || roi->type == RoiType::Line) { showError(tr("Select an area ROI first.")); return; }
        const auto slice = doc->image.slice(doc->viewer->slice(), doc->viewer->channel(), doc->viewer->time());
        auto output = std::make_shared<Mask>();
        const QPointer<DocumentWidget> target = doc;
        const auto revision = doc->revision;
        const auto z = doc->viewer->slice(), c = doc->viewer->channel(), t = doc->viewer->time();
        jobs_.submit(tr("Creating mask"), [slice, roi, output](auto stop, auto) { *output = createMask(slice, *roi, stop); },
            [this, target, revision, z, c, t, output](auto error) {
                if (showJobError(error)) return;
                if (target && target->revision == revision && target->viewer->slice() == z
                    && target->viewer->channel() == c && target->viewer->time() == t) target->viewer->setMask(output->pixels);
                output->pixels.metadata().name = output->name;
                output->pixels.metadata().sourcePath.clear();
                auto *mask = addDocument(output->pixels);
                mask->modified = true;
                updatePanels();
            });
    }});
    commands_.add({"results.export", tr("Export measurements as CSV..."), QT_TRANSLATE_NOOP("CommandCategories", "Analyze"), {}, false, {}, [this](const QVariantMap &) {
        ensurePanel("measurements");
        exportMeasurements(panel("measurements"));
    }});
}

void MainWindow::showPlot(bool profile, const QVariantMap &options)
{
    const auto *document = activeDocument();
    if (!document) return;
    const auto roi = selectedRoi();
    if (profile && (!roi || (roi->type != RoiType::Line && roi->type != RoiType::Rectangle))) {
        showError(tr("Select a line or rectangular ROI first."));
        return;
    }
    const auto slice = document->image.slice(document->viewer->slice(), document->viewer->channel(), document->viewer->time());
    const bool calibrated = options.value("calibrated", true).toBool();
    const bool logarithmic = options.value("log", false).toBool();
    const bool rows = options.value("rows", false).toBool();
    const auto bins = std::size_t(options.value("bins", 256).toUInt());
    const auto samples = std::size_t(options.value("samples", 0).toUInt());
    struct PlotData { std::vector<QPointF> points; QString xTitle, yTitle; };
    auto output = std::make_shared<PlotData>();
    jobs_.submit(profile ? tr("Line profile") : tr("Histogram"), [slice, roi, profile, output, calibrated, logarithmic, rows, bins, samples](auto stop, auto) {
        if (profile) {
            auto start = roi->points.front() - QPointF(.5, .5), end = roi->points.back() - QPointF(.5, .5);
            for (auto *point : {&start, &end}) {
                point->setX(std::clamp(point->x(), 0.0, double(slice.width() - 1)));
                point->setY(std::clamp(point->y(), 0.0, double(slice.height() - 1)));
            }
            auto profileSlice = slice;
            if (!calibrated) {
                profileSlice.metadata().intensityScale = 1;
                profileSlice.metadata().intensityOffset = 0;
                profileSlice.metadata().intensityUnit.clear();
            }
            const auto data = roi->type == RoiType::Rectangle
                ? gelLaneProfile(profileSlice, *roi, !rows, false, stop)
                : lineProfile(slice, start, end, samples, calibrated, stop);
            for (std::size_t i = 0; i < data.distance.size(); ++i) output->points.emplace_back(data.distance[i], data.intensity[i]);
            output->xTitle = tr("Distance [%1]").arg(data.distanceUnit);
            output->yTitle = data.intensityUnit.isEmpty() ? tr("Intensity") : data.intensityUnit;
        } else {
            const auto data = histogram(slice, bins, roi && roi->type != RoiType::Line ? roi : std::optional<Roi>{}, calibrated, stop);
            for (std::size_t i = 0; i < data.centers.size(); ++i) output->points.emplace_back(data.centers[i], logarithmic ? std::log10(1.0 + data.counts[i]) : double(data.counts[i]));
            output->xTitle = tr("Intensity");
            output->yTitle = logarithmic ? tr("log10(1 + pixel count)") : tr("Pixel count");
        }
    }, [this, output, profile](auto error) {
        if (showJobError(error)) return;
        auto *window = plotWindow(profile);
        auto *plot = profile ? profilePlot_ : histogramPlot_;
        try { plot->setData(output->points, output->xTitle, output->yTitle); }
        catch (...) { showJobError(std::current_exception()); return; }
        window->showNormal();
        window->raise();
        window->activateWindow();
    });
}

void MainWindow::exportMeasurements(QWidget *window)
{
    const auto path = QFileDialog::getSaveFileName(window, tr("Export measurements"), "measurements.csv", tr("CSV (*.csv)"));
    if (path.isEmpty()) return;
    const auto *model = window->findChild<QTableView *>()->model();
    QByteArray bytes;
    for (int row = -1; row < model->rowCount(); ++row) {
        for (int column = 0; column < model->columnCount(); ++column) {
            QString text = row < 0 ? model->headerData(column, Qt::Horizontal).toString() : model->data(model->index(row, column)).toString();
            if (column) bytes += ',';
            bytes += detail::spreadsheetCsvCell(std::move(text)).toUtf8();
        }
        bytes += '\n';
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        showError(tr("Cannot write measurements."));
        return;
    }
    window->setProperty("exportedRows", model->rowCount());
}

bool MainWindow::confirmDiscardMeasurements(QWidget *window)
{
    if (!window) {
        for (const auto &id : {"measurements", "multiMeasurements", "multiMeasurementsPerSlice"})
            if (auto *measurements = panel(id); measurements && !confirmDiscardMeasurements(measurements)) return false;
        return true;
    }
    const auto *model = window->findChild<QTableView *>()->model();
    const auto exportedRows = window->property("exportedRows").toInt();
    if (!window->isVisible() || model->rowCount() <= exportedRows) return true;
    QStringList unsavedMeasurements;
    for (int row = exportedRows; row < model->rowCount(); ++row) {
        auto name = model->data(model->index(row, 0)).toString();
        if (name.isEmpty()) name = tr("Untitled");
        unsavedMeasurements.append(tr("%1: %2").arg(name, model->data(model->index(row, 1)).toString()));
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
