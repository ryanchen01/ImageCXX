#include "gel_window.h"
#include "plots/plot_widget.h"
#include "processing/processing.h"
#include <QComboBox>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QQuickWidget>
#include <QQuickItem>
#include <QSaveFile>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTableView>
#include <QVBoxLayout>
#include <algorithm>
#include <stdexcept>
#include <cmath>
#include <limits>

namespace imagecxx {
GelWindow::GelWindow(std::vector<LineProfile> profiles, QString title, bool darkBands, QWidget *parent)
    : QDialog(parent, Qt::Window), profiles_(std::move(profiles))
{
    setAttribute(Qt::WA_DeleteOnClose);
    setObjectName("gelWindow"); setWindowTitle(title); resize(820, 640);
    auto *layout = new QVBoxLayout(this);
    auto *controls = new QHBoxLayout;
    lane_ = new QComboBox(this); lane_->setObjectName("gelLane");
    for (std::size_t i = 0; i < profiles_.size(); ++i) lane_->addItem(tr("Lane %1").arg(i + 1));
    first_ = new QSpinBox(this); first_->setObjectName("gelBandStart");
    last_ = new QSpinBox(this); last_->setObjectName("gelBandEnd");
    controls->addWidget(lane_);
    controls->addWidget(new QLabel(tr("Start sample"), this)); controls->addWidget(first_);
    controls->addWidget(new QLabel(tr("End sample"), this)); controls->addWidget(last_);
    auto *measure = new QPushButton(tr("Measure band"), this); measure->setObjectName("gelMeasure");
    first_->setToolTip(tr("One-based sample index along the lane. The blue line joins the baseline endpoints."));
    last_->setToolTip(first_->toolTip());
    controls->addWidget(measure); layout->addLayout(controls);
    plot_ = new PlotWidget(this); layout->addWidget(plot_, 1);
    auto *table = new QTableView(this); table->setObjectName("gelResults");
    results_ = new QStandardItemModel(this);
    results_->setHorizontalHeaderLabels({tr("Lane"), tr("Peak"), tr("Start sample"), tr("End sample"),
        tr("Start distance"), tr("End distance"), tr("Area"), tr("Percent of lane"), tr("Area unit")});
    table->setModel(results_); table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    layout->addWidget(table);
    auto *buttons = new QHBoxLayout;
    auto *remove = new QPushButton(tr("Remove selected bands"), this); remove->setObjectName("gelRemove");
    auto *label = new QPushButton(tr("Label peaks"), this); label->setObjectName("gelLabel");
    auto *exportButton = new QPushButton(tr("Export CSV..."), this); exportButton->setObjectName("gelExport");
    buttons->addWidget(remove); buttons->addWidget(label); buttons->addStretch(); buttons->addWidget(exportButton);
    layout->addLayout(buttons);
    const auto changeLane = [this, darkBands] {
        const auto &profile = profiles_.at(std::size_t(lane_->currentIndex()));
        std::vector<QPointF> points;
        for (std::size_t i = 0; i < profile.distance.size(); ++i) points.emplace_back(profile.distance[i], profile.intensity[i]);
        plot_->setData(points, tr("Distance [%1]").arg(profile.distanceUnit),
            (darkBands ? tr("Negated mean intensity") : tr("Mean intensity"))
                + (profile.intensityUnit.isEmpty() ? QString() : " [" + profile.intensityUnit + "]"));
        first_->setRange(1, int(points.size())); last_->setRange(1, int(points.size()));
        first_->setValue(1); last_->setValue(int(points.size())); updateAnnotations();
    };
    connect(lane_, &QComboBox::currentIndexChanged, this, changeLane);
    connect(first_, &QSpinBox::valueChanged, this, &GelWindow::updateAnnotations);
    connect(last_, &QSpinBox::valueChanged, this, &GelWindow::updateAnnotations);
    connect(measure, &QPushButton::clicked, this, [this] {
        const int lane = lane_->currentIndex();
        const auto &profile = profiles_.at(std::size_t(lane));
        const auto first = std::size_t(first_->value() - 1), last = std::size_t(last_->value() - 1);
        try {
            const auto area = gelPeakArea(profile, first, last);
            if (!(area > 0)) throw std::invalid_argument("The selected band has no signal above its baseline.");
            for (int row = 0; row < results_->rowCount(); ++row) {
                if (results_->index(row, 0).data().toInt() == lane + 1
                    && first_->value() < results_->index(row, 3).data().toInt()
                    && last_->value() > results_->index(row, 2).data().toInt())
                    throw std::invalid_argument("This band overlaps a measured band in the same lane.");
            }
            QList<QStandardItem *> row;
            for (const auto &value : QVariantList{lane + 1, 0, first_->value(), last_->value(),
                     profile.distance[first], profile.distance[last], area, 0.,
                     (profile.intensityUnit.isEmpty() ? QStringLiteral("a.u.") : profile.intensityUnit) + "·" + profile.distanceUnit}) {
                auto *item = new QStandardItem; item->setData(value, Qt::DisplayRole); row.append(item);
            }
            results_->appendRow(row); labelPeaks();
        } catch (const std::exception &error) { QMessageBox::warning(this, tr("Gel analysis"), QString::fromUtf8(error.what())); }
    });
    connect(label, &QPushButton::clicked, this, &GelWindow::labelPeaks);
    connect(remove, &QPushButton::clicked, this, [this, table] {
        auto rows = table->selectionModel()->selectedRows();
        std::sort(rows.begin(), rows.end(), [](auto a, auto b) { return a.row() > b.row(); });
        for (const auto &row : rows) results_->removeRow(row.row());
        labelPeaks();
    });
    connect(exportButton, &QPushButton::clicked, this, [this, darkBands] {
        const auto path = QFileDialog::getSaveFileName(this, tr("Export gel measurements"), "gel-measurements.csv", "CSV (*.csv)");
        if (path.isEmpty()) return;
        QByteArray csv;
        for (int row = -1; row < results_->rowCount(); ++row) {
            QStringList fields{row < 0 ? QStringLiteral("Source") : windowTitle(),
                row < 0 ? QStringLiteral("Polarity") : (darkBands ? QStringLiteral("Dark bands") : QStringLiteral("Bright bands"))};
            for (auto &field : fields) { field.replace('"', "\"\""); field = '"' + field + '"'; }
            for (int col = 0; col < results_->columnCount(); ++col) {
                const auto value = row < 0 ? results_->headerData(col, Qt::Horizontal) : results_->index(row, col).data();
                QString text = value.metaType().id() == QMetaType::Double
                    ? QString::number(value.toDouble(), 'g', std::numeric_limits<double>::max_digits10) : value.toString();
                text.replace('"', "\"\""); fields.append('"' + text + '"');
            }
            csv += fields.join(',').toUtf8() + '\n';
        }
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(csv) != csv.size() || !file.commit())
            QMessageBox::warning(this, tr("Gel analysis"), tr("Cannot write gel measurements."));
    });
    changeLane();
}

void GelWindow::labelPeaks()
{
    for (int lane = 1; lane <= int(profiles_.size()); ++lane) {
        long double total = 0;
        for (int row = 0; row < results_->rowCount(); ++row)
            if (results_->index(row, 0).data().toInt() == lane) total += results_->index(row, 6).data().toDouble();
        int peak = 0;
        for (int row = 0; row < results_->rowCount(); ++row) if (results_->index(row, 0).data().toInt() == lane) {
            results_->setData(results_->index(row, 1), ++peak);
            results_->setData(results_->index(row, 7), double(results_->index(row, 6).data().toDouble() / total * 100));
        }
    }
    updateAnnotations();
}

void GelWindow::updateAnnotations()
{
    const auto &profile = profiles_.at(std::size_t(lane_->currentIndex()));
    QVariantList labels;
    for (int row = 0; row < results_->rowCount(); ++row) {
        if (results_->index(row, 0).data().toInt() != lane_->currentIndex() + 1) continue;
        const auto first = std::size_t(results_->index(row, 2).data().toInt() - 1);
        const auto last = std::size_t(results_->index(row, 3).data().toInt() - 1);
        auto peak = first;
        for (auto i = first; i <= last; ++i) if (profile.intensity[i] > profile.intensity[peak]) peak = i;
        labels.append(QVariantMap{{"x", profile.distance[peak]}, {"y", profile.intensity[peak]},
            {"text", tr("%1: %2%").arg(results_->index(row, 1).data().toInt()).arg(results_->index(row, 7).data().toDouble(), 0, 'f', 2)}});
    }
    auto *root = plot_->findChild<QQuickWidget *>()->rootObject();
    if (!root) return;
    root->setProperty("bandLabels", labels);
    const auto first = std::size_t(first_->value() - 1), last = std::size_t(last_->value() - 1);
    if (first < profile.distance.size() && last < profile.distance.size()) {
        root->setProperty("bandStart", QPointF(profile.distance[first], profile.intensity[first]));
        root->setProperty("bandEnd", QPointF(profile.distance[last], profile.intensity[last]));
        root->setProperty("bandVisible", first < last);
    }
}
}
