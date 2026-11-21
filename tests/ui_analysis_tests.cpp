#include "tests/desktop_test_helpers.h"
#include "core/memory.h"
#include "io/image_io.h"
#include "mainwindow.h"
#include "ui/theme.h"
#include <gtest/gtest.h>
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QImage>
#include <QInputDialog>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLineSeries>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPrintPreviewDialog>
#include <QPrintPreviewWidget>
#include <QPrinter>
#include <QPushButton>
#include <QQuickItem>
#include <QQuickWidget>
#include <QScatterSeries>
#include <QScreen>
#include <QSet>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTranslator>
#include <QTreeWidget>
#include <QUrl>
#include <QValueAxis>
#include <QWindow>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>

using namespace imagecxx;
using namespace imagecxx::test;

namespace {
bool runMultiMeasure(MainWindow &window, bool allSlices, bool oneRowPerSlice, bool append = false, bool accept = true)
{
    QTimer::singleShot(0, &window, [=] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); ASSERT_NE(dialog, nullptr);
        EXPECT_EQ(dialog->objectName(), "multiMeasureDialog");
        auto *all = dialog->findChild<QCheckBox *>("measureAllSlices"); ASSERT_NE(all, nullptr);
        auto *oneRow = dialog->findChild<QCheckBox *>("oneRowPerSlice"); ASSERT_NE(oneRow, nullptr);
        auto *appendResults = dialog->findChild<QCheckBox *>("appendResults"); ASSERT_NE(appendResults, nullptr);
        EXPECT_TRUE(all->isChecked()); EXPECT_FALSE(oneRow->isChecked()); EXPECT_FALSE(appendResults->isChecked());
        all->setChecked(allSlices); oneRow->setChecked(oneRowPerSlice); appendResults->setChecked(append);
        if (accept) dialog->accept(); else dialog->reject();
    });
    window.commands().execute("analyze.multiMeasure");
    return finishJobs(window);
}
} // namespace

TEST(Desktop, LineProfilesSamplePixelCentersAndKeepCalibratedDistances)
{
    MainWindow window;
    auto image = Image::allocate(PixelType::UInt16, {4, 2});
    image.metadata().axes[0].spacing = image.metadata().axes[1].spacing = .25;
    image.metadata().intensityScale = 2;
    image.metadata().intensityOffset = 100;
    for (std::size_t y = 0; y < 2; ++y) for (std::size_t x = 0; x < 4; ++x)
        image.setValue(std::array<std::size_t, 2>{x, y}, 10 * x);
    auto *doc = window.addDocument(image);
    for (bool reverse : {false, true}) {
        Roi line; line.type = RoiType::Line;
        line.points = reverse ? std::vector<QPointF>{{3.5, .5}, {.5, .5}} : std::vector<QPointF>{{.5, .5}, {3.5, .5}};
        doc->setSelection(line);
        window.commands().execute("analyze.profile");
        ASSERT_TRUE(finishJobs(window));
        auto *panel = window.panel("profile");
        ASSERT_NE(panel, nullptr);
        auto *series = panel->findChild<QLineSeries *>();
        ASSERT_NE(series, nullptr);
        const auto points = series->points();
        ASSERT_EQ(points.size(), 4);
        for (qsizetype i = 0; i < points.size(); ++i) {
            EXPECT_DOUBLE_EQ(points[i].x(), double(i) * .25);
            EXPECT_DOUBLE_EQ(points[i].y(), 100 + 20 * (reverse ? 3 - i : i));
        }
    }
}

TEST(Desktop, UnrepresentableLineProfileShowsAnErrorAndFinishesItsJob)
{
    MainWindow window;
    auto image = Image::allocate(PixelType::Float64, {2, 2});
    for (std::size_t y = 0; y < 2; ++y) {
        image.setValue(std::array<std::size_t, 2>{0, y}, -1e308);
        image.setValue(std::array<std::size_t, 2>{1, y}, 1e308);
    }
    auto *doc = window.addDocument(image);
    Roi line; line.type = RoiType::Line; line.points = {{.5, .5}, {1.5, .5}};
    doc->setSelection(line);
    QString error;
    QTimer dismiss;
    QObject::connect(&dismiss, &QTimer::timeout, [&] {
        if (auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            error = dialog->text(); dialog->accept();
        }
    });
    dismiss.start(1);
    window.commands().execute("analyze.profile");
    ASSERT_TRUE(finishJobs(window));
    EXPECT_TRUE(error.contains("too wide")) << error.toStdString();
    ASSERT_NE(window.panel("profile"), nullptr);
    EXPECT_FALSE(window.panel("profile")->isVisible());
}

TEST(Desktop, RoiManagerMeasureMenuKeepsSingleMeasureAndOpensMultiMeasureOptions)
{
    MainWindow window;
    auto *doc = window.addDocument(Image::allocate(PixelType::UInt8, {4, 2, 3}));
    Roi first; first.name = "First"; first.points = {{0, 0}, {1, 1}};
    Roi second; second.name = "Second"; second.points = {{1, 0}, {3, 1}};
    doc->rois = {first, second}; doc->setSelection(first); window.showPanel("rois");
    auto *list = window.panel("rois")->findChild<QListWidget *>("roiList"); ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->selectionMode(), QAbstractItemView::ExtendedSelection);
    list->setCurrentRow(0);
    auto *button = window.panel("rois")->findChild<QPushButton *>("roiMeasure"); ASSERT_NE(button, nullptr);
    EXPECT_EQ(button->text(), "Measure...");
    auto *menu = button->menu(); ASSERT_NE(menu, nullptr); ASSERT_EQ(menu->actions().size(), 2);
    EXPECT_EQ(menu->actions()[0]->text(), "Measure"); EXPECT_EQ(menu->actions()[1]->text(), "Multi Measure...");
    menu->actions()[0]->trigger(); ASSERT_TRUE(finishJobs(window));
    auto *model = window.panel("measurements")->findChild<QTableView *>()->model();
    EXPECT_EQ(model->rowCount(), 1);
    EXPECT_EQ(model->data(model->index(0, 1)).toString(), "First");
    EXPECT_EQ(model->data(model->index(0, 2)).toInt(), 1);
    bool prompted = false;
    QTimer::singleShot(0, &window, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); ASSERT_NE(dialog, nullptr);
        EXPECT_EQ(dialog->windowTitle(), "Multi Measure");
        auto *all = dialog->findChild<QCheckBox *>("measureAllSlices"); ASSERT_NE(all, nullptr);
        EXPECT_EQ(all->text(), "Measure all 3 slices"); EXPECT_TRUE(all->isChecked());
        EXPECT_EQ(dialog->findChildren<QCheckBox *>().size(), 3);
        prompted = true; dialog->reject();
    });
    menu->actions()[1]->trigger(); EXPECT_TRUE(prompted); EXPECT_FALSE(window.jobs().busy());
    EXPECT_EQ(window.panel("multiMeasurements"), nullptr); EXPECT_EQ(model->rowCount(), 1);
}

TEST(Desktop, MultiMeasureUsesOnlySelectedRoisAndSupportsBothLayoutsAndSliceScopes)
{
    for (bool all : {false, true}) for (bool wide : {false, true}) {
        SCOPED_TRACE(::testing::Message() << "all=" << all << " wide=" << wide);
        MainWindow window;
        ImageMetadata metadata;
        metadata.name = "Stack"; metadata.intensityScale = 2; metadata.intensityOffset = 3; metadata.intensityUnit = "HU";
        metadata.axes = {{"X", .5, 0, "um"}, {"Y", 2, 0, "um"}};
        auto image = Image::allocate(PixelType::UInt16, {4, 2, 3, 2, 2}, metadata);
        for (std::size_t t = 0; t < 2; ++t) for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t z = 0; z < 3; ++z) for (std::size_t x = 0; x < 4; ++x)
                image.setValue(std::array<std::size_t, 5>{x, 0, z, c, t}, 100 * t + 20 * z + 10 * c + x);
        auto *doc = window.addDocument(image);
        for (int x = 0; x < 3; ++x) {
            Roi roi; roi.name = QString("ROI %1").arg(x + 1); roi.points = {{double(x), 0}, {double(x + 1), 1}};
            doc->rois.push_back(roi);
        }
        doc->refreshRois(); window.showPanel("rois");
        auto *list = window.panel("rois")->findChild<QListWidget *>("roiList"); ASSERT_NE(list, nullptr);
        list->setCurrentRow(0); list->item(2)->setSelected(true);
        doc->viewer->setSlice(1, 1, 1); doc->changed();
        ASSERT_EQ(list->selectedItems().size(), 2);
        ASSERT_TRUE(runMultiMeasure(window, all, wide));
        auto *panel = window.panel(wide ? "multiMeasurementsPerSlice" : "multiMeasurements"); ASSERT_NE(panel, nullptr);
        auto *model = panel->findChild<QTableView *>()->model();
        const int planes = all ? 12 : 1;
        EXPECT_EQ(model->rowCount(), wide ? planes : planes * 2);
        EXPECT_EQ(model->columnCount(), wide ? 22 : 14);
        for (int p = 0; p < planes; ++p) for (int roi = 0; roi < 2; ++roi) {
            const int plane = all ? p : 9;
            const int c = plane % 2, z = plane / 2 % 3, t = plane / 6;
            const int row = wide ? p : p * 2 + roi, stats = wide ? 4 + roi * 9 : 5;
            const int position = wide ? 1 : 2;
            EXPECT_EQ(model->data(model->index(row, 0)).toString(), "Stack");
            EXPECT_EQ(model->data(model->index(row, position)).toInt(), z + 1);
            EXPECT_EQ(model->data(model->index(row, position + 1)).toInt(), c + 1);
            EXPECT_EQ(model->data(model->index(row, position + 2)).toInt(), t + 1);
            EXPECT_EQ(model->data(model->index(row, stats)).toInt(), 1);
            EXPECT_DOUBLE_EQ(model->data(model->index(row, stats + 1)).toDouble(), 1);
            EXPECT_DOUBLE_EQ(model->data(model->index(row, stats + 2)).toDouble(), 2 * (100 * t + 20 * z + 10 * c + 2 * roi) + 3);
            EXPECT_DOUBLE_EQ(model->data(model->index(row, stats + 3)).toDouble(), 0);
            EXPECT_EQ(model->data(model->index(row, stats + 7)).toString(), QString::fromUtf8("um²"));
            EXPECT_EQ(model->data(model->index(row, stats + 8)).toString(), "HU");
            if (!wide) EXPECT_EQ(model->data(model->index(row, 1)).toString(), QString("ROI %1").arg(roi * 2 + 1));
        }
        if (wide) {
            EXPECT_EQ(model->headerData(6, Qt::Horizontal).toString(), "Mean [1: ROI 1]");
            EXPECT_EQ(model->headerData(15, Qt::Horizontal).toString(), "Mean [2: ROI 3]");
        }
        EXPECT_EQ(doc->viewer->slice(), 1u); EXPECT_EQ(doc->viewer->channel(), 1u); EXPECT_EQ(doc->viewer->time(), 1u);
        EXPECT_EQ(list->selectedItems().size(), 2); EXPECT_FALSE(doc->modified);
    }
}

TEST(Desktop, MultiMeasureAppendsWithoutMixingRoisOrLayoutsAndCanReplaceOrCancel)
{
    MainWindow window;
    auto image = Image::allocate(PixelType::UInt8, {4, 2, 2});
    std::fill_n(image.mutableData(), image.byteCount(), std::byte{7});
    auto *doc = window.addDocument(image);
    for (int x = 0; x < 2; ++x) {
        Roi roi; roi.name = "Same name"; roi.points = {{double(x), 0}, {double(x + 1), 1}}; doc->rois.push_back(roi);
    }
    doc->setSelection(doc->rois[0]); window.showPanel("rois");
    auto *list = window.panel("rois")->findChild<QListWidget *>("roiList"); list->setCurrentRow(0);
    window.commands().execute("analyze.measure"); ASSERT_TRUE(finishJobs(window));
    ASSERT_TRUE(runMultiMeasure(window, true, true));
    auto *model = window.panel("multiMeasurementsPerSlice")->findChild<QTableView *>()->model();
    ASSERT_EQ(model->rowCount(), 2); ASSERT_EQ(model->columnCount(), 13);
    list->setCurrentRow(1);
    ASSERT_TRUE(runMultiMeasure(window, false, true, true));
    ASSERT_EQ(model->rowCount(), 3); ASSERT_EQ(model->columnCount(), 22);
    EXPECT_EQ(model->headerData(6, Qt::Horizontal).toString(), "Mean [1: Same name]");
    EXPECT_EQ(model->headerData(15, Qt::Horizontal).toString(), "Mean [2: Same name]");
    EXPECT_DOUBLE_EQ(model->data(model->index(0, 6)).toDouble(), 7);
    EXPECT_FALSE(model->data(model->index(0, 15)).isValid());
    EXPECT_FALSE(model->data(model->index(2, 6)).isValid());
    EXPECT_DOUBLE_EQ(model->data(model->index(2, 15)).toDouble(), 7);
    ASSERT_TRUE(runMultiMeasure(window, true, false, true));
    auto *longModel = window.panel("multiMeasurements")->findChild<QTableView *>()->model();
    EXPECT_EQ(longModel->rowCount(), 2); EXPECT_EQ(model->rowCount(), 3);
    ASSERT_TRUE(runMultiMeasure(window, false, false, true)); EXPECT_EQ(longModel->rowCount(), 3);
    ASSERT_TRUE(runMultiMeasure(window, false, true, false, false)); EXPECT_EQ(model->rowCount(), 3);
    ASSERT_TRUE(runMultiMeasure(window, false, true));
    EXPECT_EQ(model->rowCount(), 1); EXPECT_EQ(model->columnCount(), 13);
    EXPECT_EQ(model->headerData(6, Qt::Horizontal).toString(), "Mean [1: Same name]");
    EXPECT_EQ(window.panel("measurements")->findChild<QTableView *>()->model()->rowCount(), 1);
}

TEST(Desktop, MultiMeasureKeepsColumnLabelsUniqueWhenRoisAreRemovedAndReplaced)
{
    MainWindow window;
    auto image = Image::allocate(PixelType::UInt8, {3, 1});
    for (std::size_t x = 0; x < 3; ++x) image.setValue(std::array<std::size_t, 2>{x, 0}, 2 << x);
    auto *doc = window.addDocument(image);
    for (int x = 0; x < 2; ++x) {
        Roi roi; roi.name = "Same name"; roi.points = {{double(x), 0}, {double(x + 1), 1}};
        doc->rois.push_back(roi);
    }
    doc->setSelection(doc->rois[0]); window.showPanel("rois");
    auto *list = window.panel("rois")->findChild<QListWidget *>("roiList"); ASSERT_NE(list, nullptr);
    list->setCurrentRow(0);
    ASSERT_TRUE(runMultiMeasure(window, false, true));
    auto *model = window.panel("multiMeasurementsPerSlice")->findChild<QTableView *>()->model();
    list->setCurrentRow(1);
    ASSERT_TRUE(runMultiMeasure(window, false, true, true));
    ASSERT_EQ(model->columnCount(), 22);
    EXPECT_EQ(model->headerData(6, Qt::Horizontal).toString(), "Mean [1: Same name]");
    EXPECT_EQ(model->headerData(15, Qt::Horizontal).toString(), "Mean [2: Same name]");
    list->setCurrentRow(0);
    for (auto *button : window.panel("rois")->findChildren<QPushButton *>())
        if (button->text() == "Remove") button->click();
    ASSERT_EQ(doc->rois.size(), 1u);
    list->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
    ASSERT_TRUE(runMultiMeasure(window, false, true, true));
    EXPECT_EQ(model->headerData(6, Qt::Horizontal).toString(), "Mean [1: Same name]");
    EXPECT_EQ(model->headerData(15, Qt::Horizontal).toString(), "Mean [2: Same name]");
    EXPECT_FALSE(model->data(model->index(2, 6)).isValid());
    EXPECT_DOUBLE_EQ(model->data(model->index(2, 15)).toDouble(), 4);

    Roi replacement; replacement.name = "Same name"; replacement.points = {{2, 0}, {3, 1}};
    doc->rois = {replacement}; doc->setSelection(replacement);
    list->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
    ASSERT_TRUE(runMultiMeasure(window, false, true, true));
    ASSERT_EQ(model->columnCount(), 31); ASSERT_EQ(model->rowCount(), 4);
    EXPECT_EQ(model->headerData(24, Qt::Horizontal).toString(), "Mean [3: Same name]");
    EXPECT_DOUBLE_EQ(model->data(model->index(0, 6)).toDouble(), 2);
    EXPECT_DOUBLE_EQ(model->data(model->index(1, 15)).toDouble(), 4);
    EXPECT_DOUBLE_EQ(model->data(model->index(3, 24)).toDouble(), 8);
    QSet<QString> headings;
    for (int column = 0; column < model->columnCount(); ++column) {
        const auto heading = model->headerData(column, Qt::Horizontal).toString();
        EXPECT_FALSE(headings.contains(heading)); headings.insert(heading);
    }
}

TEST(Desktop, MultiMeasureRequiresSelectedEntriesAndKeepsResultsWhenMeasurementFails)
{
    MainWindow window;
    auto *doc = window.addDocument(Image::allocate(PixelType::UInt8, {4, 2}));
    Roi roi; roi.points = {{0, 0}, {2, 1}}; doc->rois = {roi}; doc->setSelection(roi); window.showPanel("rois");
    auto *list = window.panel("rois")->findChild<QListWidget *>("roiList"); list->setCurrentRow(0);
    ASSERT_TRUE(runMultiMeasure(window, true, false));
    auto *model = window.panel("multiMeasurements")->findChild<QTableView *>()->model();
    EXPECT_EQ(model->rowCount(), 1);
    QString error;
    QTimer dismiss;
    QObject::connect(&dismiss, &QTimer::timeout, [&] {
        if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            error = message->text(); message->accept();
        }
    });
    dismiss.start(1);
    list->clearSelection(); window.commands().execute("analyze.multiMeasure");
    EXPECT_EQ(error, "Select one or more ROIs in the ROI manager first.");
    list->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
    doc->rois[0].type = RoiType::Line; doc->refreshRois();
    ASSERT_TRUE(runMultiMeasure(window, true, false));
    EXPECT_EQ(error, "Use a line profile for line ROIs"); EXPECT_EQ(model->rowCount(), 1);
}

TEST(Desktop, RoiManagerKeepsMultipleSelectionsThroughClicksAndPanelRefreshes)
{
    MainWindow window;
    auto *doc = window.addDocument(Image::allocate(PixelType::UInt8, {4, 2, 3}));
    for (int z = 0; z < 3; ++z) {
        Roi roi; roi.name = QString::number(z); roi.z = z; roi.points = {{0, 0}, {1, 1}};
        doc->rois.push_back(roi);
    }
    doc->setSelection(doc->rois[0]); window.showPanel("rois");
    auto *list = window.panel("rois")->findChild<QListWidget *>("roiList"); ASSERT_NE(list, nullptr);
    QCoreApplication::processEvents();
    for (int row : {0, 2}) {
        const auto position = list->visualItemRect(list->item(row)).center();
        for (auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            QMouseEvent event(type, position, list->viewport()->mapToGlobal(position), Qt::LeftButton,
                type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, row ? Qt::ControlModifier : Qt::NoModifier);
            QApplication::sendEvent(list->viewport(), &event);
        }
    }
    ASSERT_EQ(list->selectedItems().size(), 2);
    EXPECT_TRUE(list->item(0)->isSelected()); EXPECT_TRUE(list->item(2)->isSelected());
    EXPECT_EQ(doc->viewer->slice(), 2u);
    doc->changed(); window.showPanel("rois");
    EXPECT_EQ(list->selectedItems().size(), 2); EXPECT_EQ(list->currentRow(), 2);
    ASSERT_TRUE(runMultiMeasure(window, false, false));
    EXPECT_EQ(window.panel("multiMeasurements")->findChild<QTableView *>()->model()->rowCount(), 2);
}

TEST(Desktop, MultiMeasureExportsBothLayoutsAndWarnsAboutUnsavedAndReplacedResults)
{
    for (bool wide : {false, true}) {
        MainWindow window;
        auto image = Image::allocate(PixelType::UInt8, {4, 2, 2}); image.metadata().name = "=Stack";
        std::fill_n(image.mutableData(), image.byteCount(), std::byte{7});
        auto *doc = window.addDocument(image);
        Roi roi; roi.name = "A,\"B\""; roi.points = {{0, 0}, {2, 1}};
        doc->rois = {roi}; doc->setSelection(roi); window.showPanel("rois");
        auto *list = window.panel("rois")->findChild<QListWidget *>("roiList"); list->setCurrentRow(0);
        ASSERT_TRUE(runMultiMeasure(window, true, wide));
        auto *panel = window.panel(wide ? "multiMeasurementsPerSlice" : "multiMeasurements"); ASSERT_NE(panel, nullptr);
        auto *model = panel->findChild<QTableView *>()->model();
        const auto closePanel = [&](bool expectPrompt) {
            bool prompted = false;
            QTimer::singleShot(0, &window, [&] {
                if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                    prompted = true; EXPECT_TRUE(message->text().contains("unsaved measurements"));
                    message->done(QMessageBox::Cancel);
                }
            });
            QCloseEvent close; QApplication::sendEvent(panel, &close); QCoreApplication::processEvents();
            EXPECT_EQ(prompted, expectPrompt); EXPECT_EQ(close.isAccepted(), !expectPrompt);
        };
        closePanel(true);
        QTemporaryDir directory;
        const auto path = directory.filePath("multi.csv");
        QTimer::singleShot(0, &window, [path] {
            auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget()); ASSERT_NE(dialog, nullptr);
            dialog->setDirectory(QFileInfo(path).absolutePath()); dialog->selectFile(QFileInfo(path).fileName());
            if (auto *name = dialog->findChild<QLineEdit *>("fileNameEdit")) name->setText(QFileInfo(path).fileName());
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        });
        panel->findChild<QPushButton *>("exportMultiMeasurements")->click();
        QFile file(path); ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        const auto csv = file.readAll(); EXPECT_TRUE(csv.contains("\"Text: =Stack\""));
        EXPECT_TRUE(csv.contains(wide ? "Mean [1: A,\"\"B\"\"]" : "\"A,\"\"B\"\"\""));
        EXPECT_EQ(csv.trimmed().split('\n').size(), 3);
        EXPECT_EQ(panel->property("exportedRows").toInt(), 2); closePanel(false);
        ASSERT_TRUE(runMultiMeasure(window, true, wide)); EXPECT_EQ(model->rowCount(), 2);
        EXPECT_EQ(panel->property("exportedRows").toInt(), 0); closePanel(true);
        ASSERT_TRUE(runMultiMeasure(window, false, wide, true)); EXPECT_EQ(model->rowCount(), 3);
    }
}

TEST(Desktop, RoiContextMenuActionsUseTheClickedRoiAndDocument)
{
    MainWindow window;
    auto image = Image::allocate(PixelType::UInt8, {10, 10});
    std::memset(image.mutableData(), 10, image.byteCount());
    auto *document = window.addDocument(image);
    Roi first; first.name = "First"; first.points = {{1, 1}, {4, 3}};
    Roi second; second.name = "Second"; second.points = {{6, 6}, {9, 9}};
    document->rois = {first, second}; document->setSelection(second);
    auto *other = window.addDocument(Image::allocate(PixelType::UInt8, {10, 10}));
    auto *viewer = document->viewer;
    viewer->resize(400, 400); viewer->setZoom(10); viewer->setTool(ImageViewer::Tool::Pointer);
    const auto contextAction = [&](const Roi &roi, const char *command) {
        QEvent activate(QEvent::WindowActivate); QApplication::sendEvent(other, &activate);
        QTimer::singleShot(0, viewer, [&] {
            auto *menu = viewer->findChild<QMenu *>("roiContextMenu");
            EXPECT_NE(menu, nullptr);
            if (!menu) return;
            EXPECT_EQ(window.activeDocument(), document);
            EXPECT_TRUE(document->selection);
            if (document->selection) EXPECT_EQ(document->selection->id, roi.id);
            for (const auto &id : {"analyze.measure", "selection.add", "analyze.histogram",
                                   "selection.properties", "selection.none"})
                EXPECT_TRUE(menu->actions().contains(window.findChild<QAction *>(id)));
            EXPECT_EQ(menu->actions().contains(window.findChild<QAction *>("analyze.profile")),
                roi.type == RoiType::Line || roi.type == RoiType::Rectangle);
            menu->setActiveAction(window.findChild<QAction *>(command));
            sendKey(menu, Qt::Key_Return);
        });
        const auto position = viewer->imageToWidget(roi.bounds().center()).toPoint();
        QContextMenuEvent event(QContextMenuEvent::Mouse, position, viewer->mapToGlobal(position));
        QApplication::sendEvent(viewer, &event);
    };
    contextAction(first, "analyze.measure");
    ASSERT_TRUE(finishJobs(window));
    auto *table = window.panel("measurements")->findChild<QTableView *>(); ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->model()->rowCount(), 1);
    EXPECT_EQ(table->model()->data(table->model()->index(0, 1)).toString(), "First");
    EXPECT_EQ(table->model()->data(table->model()->index(0, 2)).toInt(), 6);
    EXPECT_DOUBLE_EQ(table->model()->data(table->model()->index(0, 4)).toDouble(), 10);
    Roi unsaved; unsaved.points = {{2, 1}, {5, 4}};
    document->setSelection(unsaved);
    contextAction(unsaved, "selection.add");
    ASSERT_EQ(document->rois.size(), 3u); EXPECT_EQ(document->rois.back().id, unsaved.id);
    EXPECT_TRUE(window.panel("rois")->isVisible());
    Roi ellipse; ellipse.type = RoiType::Ellipse; ellipse.points = {{1, 1}, {4, 3}};
    document->setSelection(ellipse);
    contextAction(ellipse, "selection.none");
    EXPECT_FALSE(document->selection);
    ASSERT_TRUE(document->previousSelection); EXPECT_EQ(document->previousSelection->id, ellipse.id);
    EXPECT_TRUE(other->rois.empty()); EXPECT_FALSE(other->selection);
    EXPECT_FALSE(document->modified); EXPECT_FALSE(other->modified);
}

TEST(Desktop, CompositeRoiContextMenuActionsUseTheClickedChannelAndRegion)
{
    MainWindow window;
    auto image = Image::allocate(PixelType::UInt16, {4, 2, 3, 2, 2});
    for (std::size_t t = 0; t < 2; ++t) for (std::size_t c = 0; c < 2; ++c)
        for (std::size_t z = 0; z < 3; ++z) for (std::size_t y = 0; y < 2; ++y)
            for (std::size_t x = 0; x < 4; ++x)
                image.setValue(std::array<std::size_t, 5>{x, y, z, c, t}, 1000 * t + 100 * z + 10 * c + 4 * y + x);
    auto *doc = window.addDocument(image);
    Roi roi; roi.name = "Channel 2 ROI"; roi.channel = 1; roi.time = 1; roi.points = {{1, 0}, {3, 1}};
    doc->rois = {roi}; doc->refreshRois();
    auto *viewer = doc->viewer;
    viewer->resize(400, 400); viewer->setZoom(20); viewer->setComposite(true);
    const auto contextAction = [&](const char *command) {
        doc->setSelection({}); viewer->setSlice(2, 0, 1);
        QTimer::singleShot(0, viewer, [&] {
            auto *menu = viewer->findChild<QMenu *>("roiContextMenu"); ASSERT_NE(menu, nullptr);
            ASSERT_TRUE(doc->selection); EXPECT_EQ(doc->selection->id, roi.id);
            EXPECT_EQ(viewer->channel(), 1u); EXPECT_EQ(viewer->slice(), 2u); EXPECT_EQ(viewer->time(), 1u);
            EXPECT_TRUE(viewer->composite());
            menu->setActiveAction(window.findChild<QAction *>(command)); sendKey(menu, Qt::Key_Return);
        });
        const auto position = viewer->imageToWidget(roi.bounds().center()).toPoint();
        QContextMenuEvent event(QContextMenuEvent::Mouse, position, viewer->mapToGlobal(position));
        QApplication::sendEvent(viewer, &event);
    };
    contextAction("analyze.measure"); ASSERT_TRUE(finishJobs(window));
    auto *model = window.panel("measurements")->findChild<QTableView *>()->model();
    ASSERT_EQ(model->rowCount(), 1);
    EXPECT_EQ(model->data(model->index(0, 1)).toString(), roi.name);
    EXPECT_EQ(model->data(model->index(0, 2)).toInt(), 2);
    EXPECT_DOUBLE_EQ(model->data(model->index(0, 4)).toDouble(), 1211.5);
    contextAction("analyze.histogram"); ASSERT_TRUE(finishJobs(window));
    auto *series = window.panel("histogram")->findChild<QLineSeries *>(); ASSERT_NE(series, nullptr);
    double samples = 0;
    for (const auto &point : series->points()) samples += point.y();
    EXPECT_DOUBLE_EQ(samples, 2);
    ASSERT_FALSE(series->points().isEmpty());
    EXPECT_GT(series->points().front().x(), 1211); EXPECT_LT(series->points().back().x(), 1212);
    QTimer dismiss;
    bool propertiesOpened = false;
    QString error;
    QObject::connect(&dismiss, &QTimer::timeout, [&] {
        if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            error = message->text(); message->accept();
        } else if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                   dialog && dialog->objectName() == "selectionPropertiesDialog") {
            propertiesOpened = true;
            EXPECT_EQ(dialog->findChild<QLineEdit *>("roiName")->text(), roi.name); dialog->reject();
        }
    });
    dismiss.start(1);
    contextAction("selection.properties");
    EXPECT_TRUE(propertiesOpened); EXPECT_TRUE(error.isEmpty()) << error.toStdString();
    EXPECT_EQ(doc->rois.front().channel, 1u); EXPECT_EQ(doc->rois.front().z, 0u);
    EXPECT_FALSE(doc->modified);
}

TEST(Desktop, ScatterPlotsUseNumericMeasurementColumnsAndKeepASnapshot)
{
    MainWindow window;
    auto *action = window.findChild<QAction *>("analyze.scatter");
    ASSERT_NE(action, nullptr);
    EXPECT_TRUE(action->isEnabled());
    EXPECT_FALSE(window.commands().command("analyze.scatter").needsImage);
    ImageMetadata metadata;
    metadata.axes = {{"X", .5, 0, "um"}, {"Y", 1, 0, "um"}};
    metadata.intensityScale = 2; metadata.intensityOffset = 3;
    auto image = Image::allocate(PixelType::UInt16, {3, 1}, metadata);
    for (std::size_t x = 0; x < 3; ++x)
        image.setValue(std::array<std::size_t, 2>{x, 0}, 10 * (x + 1));
    auto *doc = window.addDocument(image);
    for (int width : {3, 1, 2}) {
        Roi roi; roi.points = {{0, 0}, {double(width), 1}};
        doc->setSelection(roi);
        window.commands().execute("analyze.measure");
        ASSERT_TRUE(finishJobs(window));
    }
    QTimer::singleShot(0, &window, [] {
        auto *form = qobject_cast<QDialog *>(QApplication::activeModalWidget()); ASSERT_NE(form, nullptr);
        auto *x = form->findChild<QComboBox *>("x"); ASSERT_NE(x, nullptr);
        auto *y = form->findChild<QComboBox *>("y"); ASSERT_NE(y, nullptr);
        EXPECT_EQ(x->currentText(), "Area");
        EXPECT_EQ(y->currentText(), "Mean");
        EXPECT_EQ(x->count(), 9);
        for (auto *axis : {x, y}) {
            EXPECT_GE(axis->findText("Slice"), 0);
            EXPECT_GE(axis->findText("Index"), 0);
        }
        EXPECT_EQ(x->findText("Image"), -1);
        form->accept();
    });
    window.executeCommand("analyze.scatter");
    auto *plot = window.findChild<QDialog *>("scatterPlotWindow"); ASSERT_NE(plot, nullptr);
    auto *series = plot->findChild<QScatterSeries *>(); ASSERT_NE(series, nullptr);
    const QList<QPointF> expected{{1.5, 43}, {.5, 23}, {1, 33}};
    EXPECT_EQ(series->points(), expected);
    const auto axes = plot->findChildren<QValueAxis *>(); ASSERT_EQ(axes.size(), 2);
    EXPECT_EQ(axes[0]->titleText(), "Area"); EXPECT_EQ(axes[1]->titleText(), "Mean");
    auto *table = window.panel("measurements")->findChild<QTableView *>(); ASSERT_NE(table, nullptr);
    auto *model = table->model();
    model->setData(model->index(0, 4), std::numeric_limits<double>::quiet_NaN());
    EXPECT_EQ(series->points(), expected);
    window.commands().execute("analyze.scatter", {{"x", "Pixels"}, {"y", "Mean"}});
    const auto plots = window.findChildren<QDialog *>("scatterPlotWindow"); ASSERT_EQ(plots.size(), 2);
    EXPECT_EQ(plots.back()->findChild<QScatterSeries *>()->points(), (QList<QPointF>{{1, 23}, {2, 33}}));
    EXPECT_THROW(window.commands().execute("analyze.scatter", {{"x", "Image"}}), std::invalid_argument);
    auto *menu = window.findChild<QMenu *>("windowMenu"); ASSERT_NE(menu, nullptr);
    menu->aboutToShow(); plot->hide();
    QAction *plotAction = nullptr;
    for (auto *entry : menu->actions()) if (entry->text() == plot->windowTitle()) plotAction = entry;
    ASSERT_NE(plotAction, nullptr); plotAction->trigger(); EXPECT_TRUE(plot->isVisible());
    const auto capture = qEnvironmentVariable("IMAGECXX_PLOT_CAPTURE_PATH");
    if (!capture.isEmpty()) {
        EXPECT_TRUE(waitFor([plot] { return plot->isVisible(); }));
        QElapsedTimer timer; timer.start();
        while (timer.elapsed() < 200) { QCoreApplication::processEvents(); QThread::msleep(2); }
        EXPECT_TRUE(plot->grab().save(capture + "measurement-scatter.png"));
    }
    EXPECT_FALSE(doc->modified);
    EXPECT_DOUBLE_EQ(doc->image.sample(2, 0), 30);
}

TEST(Desktop, ScatterPlotsUseRecordedSlicesAndMeasurementRowIndicesOnEitherAxis)
{
    MainWindow window;
    auto image = Image::allocate(PixelType::UInt8, {1, 1, 4});
    for (std::size_t z = 0; z < 4; ++z)
        image.setValue(std::array<std::size_t, 3>{0, 0, z}, 10 * (z + 1));
    auto *doc = window.addDocument(image);
    for (int z : {2, 0, 2, 3}) {
        doc->viewer->setSlice(z);
        if (z != 3) {
            Roi roi; roi.name = "Repeated ROI"; roi.z = z; roi.points = {{0, 0}, {1, 1}};
            doc->setSelection(roi);
        } else doc->setSelection(std::nullopt);
        window.commands().execute("analyze.measure");
        doc->viewer->setSlice((z + 1) % 4);
        ASSERT_TRUE(finishJobs(window));
    }
    window.commands().execute("analyze.scatter", {{"x", "Slice"}, {"y", "Index"}});
    auto *plot = window.findChild<QDialog *>("scatterPlotWindow"); ASSERT_NE(plot, nullptr);
    auto *series = plot->findChild<QScatterSeries *>(); ASSERT_NE(series, nullptr);
    const QList<QPointF> expected{{3, 1}, {1, 2}, {3, 3}, {4, 4}};
    EXPECT_EQ(series->points(), expected);
    const auto axes = plot->findChildren<QValueAxis *>(); ASSERT_EQ(axes.size(), 2);
    EXPECT_EQ(axes[0]->titleText(), "Slice"); EXPECT_EQ(axes[1]->titleText(), "Index");

    auto *model = window.panel("measurements")->findChild<QTableView *>()->model();
    model->setData(model->index(1, 4), std::numeric_limits<double>::quiet_NaN());
    window.commands().execute("analyze.scatter", {{"x", "Index"}, {"y", "Mean"}});
    auto plots = window.findChildren<QDialog *>("scatterPlotWindow"); ASSERT_EQ(plots.size(), 2);
    EXPECT_EQ(plots.back()->findChild<QScatterSeries *>()->points(), (QList<QPointF>{{1, 30}, {3, 30}, {4, 40}}));
    window.commands().execute("analyze.scatter", {{"x", "Mean"}, {"y", "Slice"}});
    plots = window.findChildren<QDialog *>("scatterPlotWindow"); ASSERT_EQ(plots.size(), 3);
    EXPECT_EQ(plots.back()->findChild<QScatterSeries *>()->points(), (QList<QPointF>{{30, 3}, {30, 3}, {40, 4}}));
    EXPECT_EQ(series->points(), expected);
}

TEST(Desktop, ScatterPlotsReportMissingMeasurementsAndFinitePairs)
{
    MainWindow window;
    QString error;
    QTimer dismiss;
    QObject::connect(&dismiss, &QTimer::timeout, [&] {
        if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            error = message->text(); message->accept();
        }
    });
    dismiss.start(1);
    window.commands().execute("analyze.scatter");
    EXPECT_EQ(error, "Measure an image or selection first.");
    window.addDocument(Image::allocate(PixelType::UInt8, {1, 1}));
    window.commands().execute("analyze.measure"); ASSERT_TRUE(finishJobs(window));
    auto *model = window.panel("measurements")->findChild<QTableView *>()->model();
    model->setData(model->index(0, 4), std::numeric_limits<double>::infinity());
    window.commands().execute("analyze.scatter");
    EXPECT_EQ(error, "The selected columns have no finite measurement pairs.");
    EXPECT_EQ(window.findChild<QDialog *>("scatterPlotWindow"), nullptr);
}

TEST(Desktop, RoiMeasurementPlotsPreserveCalibrationScopeAndExportSnapshotStatistics)
{
    for (bool overTime : {true, false}) {
        SCOPED_TRACE(overTime);
        MainWindow window;
        ImageMetadata metadata;
        metadata.name = "=Sample,\"A\"";
        metadata.axes = {{"X", .5, 0, "um"}, {"Y", .25, 0, "um"}, {"Z", 2, 10, "um"},
                         {"C", 1, 0, ""}, {"T", .5, 7, "s"}};
        metadata.intensityScale = 2; metadata.intensityOffset = 3; metadata.intensityUnit = "a.u.";
        auto image = Image::allocate(PixelType::UInt16, {3, 2, 3, 2, 3}, metadata);
        for (std::size_t t = 0; t < 3; ++t) for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t z = 0; z < 3; ++z) for (std::size_t y = 0; y < 2; ++y)
                for (std::size_t x = 0; x < 3; ++x)
                    image.setValue(std::array<std::size_t, 5>{x, y, z, c, t},
                        y == 0 && x < 2 ? 100 * t + 10 * c + z + 2 * x : 10000);
        auto *doc = window.addDocument(image);
        doc->setPosition(0, 0, 1, 1, 1);
        Roi roi; roi.name = "ROI,\"one\""; roi.points = {{0, 0}, {2, 1}};
        roi.channel = 1; roi.time = 1;
        doc->setSelection(roi);
        EXPECT_EQ(window.findChild<QDialog *>("roiMeasurementsWindow"), nullptr);
        // Exercise the generated parameter form used by the menu command.
        QTimer::singleShot(0, &window, [overTime] {
            auto *form = qobject_cast<QDialog *>(QApplication::activeModalWidget()); ASSERT_NE(form, nullptr);
            auto *axis = form->findChild<QComboBox *>("axis"); ASSERT_NE(axis, nullptr);
            EXPECT_EQ(axis->currentText(), "Time");
            axis->setCurrentText(overTime ? "Time" : "Z"); form->accept();
        });
        window.executeCommand("analyze.roiPlot");
        // Results must describe the requested image and ROI, even after subsequent edits.
        doc->image.setValue(std::array<std::size_t, 5>{0, 0, 1, 1, 0}, 60000);
        doc->setSelection({}); doc->setPosition(0, 0, 0);
        ASSERT_TRUE(finishJobs(window));
        auto *plot = window.findChild<QDialog *>("roiMeasurementsWindow"); ASSERT_NE(plot, nullptr);
        auto *series = plot->findChild<QLineSeries *>(); ASSERT_NE(series, nullptr);
        const auto points = series->points(); ASSERT_EQ(points.size(), 3);
        const auto axes = plot->findChildren<QValueAxis *>(); ASSERT_EQ(axes.size(), 2);
        const QString xTitle = overTime ? "Time [s]" : "Z [um]";
        EXPECT_EQ(axes[0]->titleText(), xTitle);
        EXPECT_EQ(axes[1]->titleText(), "Mean intensity [a.u.]");
        EXPECT_TRUE(plot->windowTitle().contains(overTime ? "C2, Z2" : "C2, T2"));
        for (int i = 0; i < 3; ++i) {
            EXPECT_DOUBLE_EQ(points[i].x(), overTime ? 7 + .5 * i : 10 + 2 * i);
            EXPECT_DOUBLE_EQ(points[i].y(), overTime ? 27 + 200 * i : 225 + 2 * i);
        }
        // Completed plots remain accessible through the Window menu.
        auto *menu = window.findChild<QMenu *>("windowMenu"); ASSERT_NE(menu, nullptr);
        menu->aboutToShow(); plot->hide();
        QAction *plotAction = nullptr;
        for (auto *action : menu->actions()) if (action->text() == plot->windowTitle()) plotAction = action;
        ASSERT_NE(plotAction, nullptr); plotAction->trigger(); EXPECT_TRUE(plot->isVisible());
        QTemporaryDir directory; ASSERT_TRUE(directory.isValid());
        const auto path = directory.filePath("measurements.csv");
        auto *exportButton = plot->findChild<QPushButton *>("exportRoiMeasurements"); ASSERT_NE(exportButton, nullptr);
        QTimer picker;
        QObject::connect(&picker, &QTimer::timeout, &window, [path] {
            auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            dialog->setDirectory(QFileInfo(path).absolutePath()); dialog->selectFile(QFileInfo(path).fileName());
            if (auto *name = dialog->findChild<QLineEdit *>("fileNameEdit")) name->setText(QFileInfo(path).fileName());
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        });
        picker.start(1); exportButton->click(); picker.stop();
        QFile csv(path); ASSERT_TRUE(csv.open(QIODevice::ReadOnly));
        const auto rows = QString::fromUtf8(csv.readAll()).split('\n'); ASSERT_EQ(rows.size(), 5);
        EXPECT_EQ(rows[0], QString("\"Image\",\"ROI\",\"Z\",\"Channel\",\"Frame\",\"%1\",\"Count\",\"Area\",\"Mean\","
            "\"StdDev\",\"Min\",\"Max\",\"Sum\",\"Area unit\",\"Intensity unit\"").arg(xTitle));
        for (int i = 0; i < 3; ++i) {
            const int mean = overTime ? 27 + 200 * i : 225 + 2 * i;
            const auto &row = rows[i + 1];
            ASSERT_TRUE(row.startsWith('"') && row.endsWith('"'));
            const auto fields = row.mid(1, row.size() - 2).split("\",\""); ASSERT_EQ(fields.size(), 15);
            EXPECT_EQ(fields[0], "Text: =Sample,\"\"A\"\""); EXPECT_EQ(fields[1], "ROI,\"\"one\"\"");
            EXPECT_EQ(fields[2].toInt(), overTime ? 2 : i + 1); EXPECT_EQ(fields[3].toInt(), 2);
            EXPECT_EQ(fields[4].toInt(), overTime ? i + 1 : 2);
            EXPECT_DOUBLE_EQ(fields[5].toDouble(), points[i].x());
            EXPECT_EQ(fields[6].toInt(), 2); EXPECT_DOUBLE_EQ(fields[7].toDouble(), .25);
            EXPECT_DOUBLE_EQ(fields[8].toDouble(), points[i].y());
            EXPECT_NEAR(fields[9].toDouble(), 2, 1e-12);
            EXPECT_DOUBLE_EQ(fields[10].toDouble(), mean - 2); EXPECT_DOUBLE_EQ(fields[11].toDouble(), mean + 2);
            EXPECT_NEAR(fields[12].toDouble(), mean * 2, 1e-12);
            EXPECT_EQ(fields[13], "um²"); EXPECT_EQ(fields[14], "a.u.");
        }
        QPointer<QDialog> closed(plot); plot->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        EXPECT_TRUE(closed.isNull());
    }
}

TEST(Desktop, RoiMeasurementPlotsUseIndicesWithoutCalibrationAndSupportAreaShapes)
{
    for (const auto type : {RoiType::Rectangle, RoiType::Ellipse, RoiType::Polygon}) {
        for (bool overTime : {true, false}) {
            MainWindow window;
            auto image = Image::allocate(PixelType::Float64, {2, 2, 2, 1, 2});
            for (std::size_t t = 0; t < 2; ++t) for (std::size_t z = 0; z < 2; ++z)
                for (std::size_t y = 0; y < 2; ++y) for (std::size_t x = 0; x < 2; ++x)
                    image.setValue(std::array<std::size_t, 5>{x, y, z, 0, t}, .123456789012345 + 10 * t + z);
            auto *doc = window.addDocument(image);
            Roi roi; roi.type = type;
            roi.points = type == RoiType::Polygon ? std::vector<QPointF>{{0, 0}, {2, 0}, {2, 2}, {0, 2}}
                                                 : std::vector<QPointF>{{0, 0}, {2, 2}};
            doc->setSelection(roi);
            window.commands().execute("analyze.roiPlot", {{"axis", overTime ? "Time" : "Z"}});
            ASSERT_TRUE(finishJobs(window));
            auto *plot = window.findChild<QDialog *>("roiMeasurementsWindow"); ASSERT_NE(plot, nullptr);
            auto *series = plot->findChild<QLineSeries *>(); ASSERT_NE(series, nullptr);
            const auto points = series->points(); ASSERT_EQ(points.size(), 2);
            EXPECT_DOUBLE_EQ(points[0].x(), 1); EXPECT_DOUBLE_EQ(points[1].x(), 2);
            EXPECT_DOUBLE_EQ(points[0].y(), .123456789012345);
            EXPECT_DOUBLE_EQ(points[1].y(), .123456789012345 + (overTime ? 10 : 1));
            const auto axes = plot->findChildren<QValueAxis *>(); ASSERT_EQ(axes.size(), 2);
            EXPECT_EQ(axes[0]->titleText(), overTime ? "Frame" : "Slice");
            EXPECT_EQ(axes[1]->titleText(), "Mean intensity");
        }
    }
}

TEST(Desktop, RoiMeasurementPlotsValidateSelectionsAxesAndHonorCancellation)
{
    MainWindow window;
    auto *doc = window.addDocument(Image::allocate(PixelType::UInt8, {4, 4, 2, 1, 2}));
    QString error;
    QTimer dismiss;
    QObject::connect(&dismiss, &QTimer::timeout, [&] {
        if (auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            error = dialog->text(); dialog->accept();
        }
    });
    dismiss.start(1);
    window.commands().execute("analyze.roiPlot"); EXPECT_EQ(error, "Select an area ROI first.");
    Roi roi; roi.type = RoiType::Line; roi.points = {{0, 0}, {2, 2}}; doc->setSelection(roi);
    error.clear(); window.commands().execute("analyze.roiPlot"); EXPECT_EQ(error, "Select an area ROI first.");
    EXPECT_FALSE(window.jobs().busy());
    EXPECT_THROW(window.commands().execute("analyze.roiPlot", {{"axis", "Channel"}}), std::invalid_argument);
    roi.type = RoiType::Rectangle; doc->setSelection(roi);
    window.commands().execute("analyze.roiPlot"); window.jobs().cancelAll();
    ASSERT_TRUE(finishJobs(window));
    EXPECT_EQ(window.findChild<QDialog *>("roiMeasurementsWindow"), nullptr);
    doc = window.addDocument(Image::allocate(PixelType::UInt8, {4, 4})); doc->setSelection(roi);
    error.clear(); window.commands().execute("analyze.roiPlot"); EXPECT_EQ(error, "The image needs at least two time frames.");
    error.clear(); window.commands().execute("analyze.roiPlot", {{"axis", "Z"}});
    EXPECT_EQ(error, "The image needs at least two Z slices.");
    EXPECT_FALSE(window.jobs().busy());
    EXPECT_EQ(window.findChild<QDialog *>("roiMeasurementsWindow"), nullptr);
}

TEST(Desktop, RoiRenameSurvivesAnAsynchronousPanelRefresh)
{
    MainWindow window;
    auto *document = window.addDocument(Image::allocate(PixelType::UInt8, {8, 8}));
    Roi roi; roi.name = "Before"; roi.points = {{0, 0}, {4, 4}};
    document->rois.push_back(roi); document->changed();
    document->setSelection(roi);
    window.showPanel("rois");
    auto *list = window.panel("rois")->findChild<QListWidget *>("roiList"); ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->count(), 1);
    QTimer::singleShot(0, &window, [document] {
        document->changed();
        for (auto *widget : QApplication::topLevelWidgets()) if (auto *dialog = qobject_cast<QInputDialog *>(widget)) {
            dialog->setTextValue("Renamed"); dialog->accept(); return;
        }
    });
    list->itemDoubleClicked(list->item(0));
    EXPECT_EQ(document->rois.front().name, "Renamed"); EXPECT_FALSE(document->modified);
    ASSERT_TRUE(document->selection);
    EXPECT_EQ(document->selection->name, "Renamed");
    window.commands().execute("analyze.measure");
    ASSERT_TRUE(finishJobs(window));
    window.showPanel("measurements");
    auto *table = window.panel("measurements")->findChild<QTableView *>();
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->model()->data(table->model()->index(0, 1)).toString(), "Renamed");
    auto edited = *document->selection;
    edited.points[1] += QPointF(1, 0);
    document->viewer->roiEdited(edited);
    EXPECT_EQ(document->rois.front().name, "Renamed");
}

TEST(Desktop, SelectionPropertiesApplyAreaGeometryAndAppearanceWithoutEditingPixels)
{
    for (const auto type : {RoiType::Rectangle, RoiType::Ellipse}) {
        MainWindow window;
        auto *doc = window.addDocument(Image::allocate(PixelType::UInt16, {16, 16, 3, 2, 2}));
        doc->image.setValue(std::array<std::size_t, 5>{3, 4, 2, 1, 1}, 43210);
        Roi roi; roi.type = type; roi.name = "Before"; roi.points = {{7, 8}, {1, 2}};
        roi.z = 0; roi.channel = 1; roi.time = 1;
        doc->rois.push_back(roi); doc->setSelection(roi); doc->viewer->setSlice(2, 1, 1);
        auto *action = window.findChild<QAction *>("selection.properties"); ASSERT_NE(action, nullptr);
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); ASSERT_NE(dialog, nullptr);
            EXPECT_EQ(dialog->objectName(), "selectionPropertiesDialog");
            auto *name = dialog->findChild<QLineEdit *>("roiName"); ASSERT_NE(name, nullptr); name->setText("Measured area");
            auto *x = dialog->findChild<QDoubleSpinBox *>("roiX"); ASSERT_NE(x, nullptr); EXPECT_DOUBLE_EQ(x->value(), 1);
            auto *y = dialog->findChild<QDoubleSpinBox *>("roiY"); ASSERT_NE(y, nullptr); EXPECT_DOUBLE_EQ(y->value(), 2);
            auto *width = dialog->findChild<QDoubleSpinBox *>("roiWidth"); ASSERT_NE(width, nullptr);
            auto *height = dialog->findChild<QDoubleSpinBox *>("roiHeight"); ASSERT_NE(height, nullptr);
            EXPECT_DOUBLE_EQ(width->value(), 6); EXPECT_DOUBLE_EQ(height->value(), 6);
            x->setValue(2.25); y->setValue(3.5); width->setValue(4.5); height->setValue(5.25);
            auto *visible = dialog->findChild<QCheckBox *>("roiVisible"); ASSERT_NE(visible, nullptr); visible->setChecked(false);
            auto *color = dialog->findChild<QPushButton *>("roiColor"); ASSERT_NE(color, nullptr);
            QTimer::singleShot(0, dialog, [] {
                auto *picker = qobject_cast<QColorDialog *>(QApplication::activeModalWidget()); ASSERT_NE(picker, nullptr);
                picker->setCurrentColor(QColor(12, 34, 56, 128)); picker->accept();
            });
            color->click();
            auto *buttons = dialog->findChild<QDialogButtonBox *>(); ASSERT_NE(buttons, nullptr);
            buttons->button(QDialogButtonBox::Apply)->click();
            EXPECT_TRUE(dialog->isVisible());
            EXPECT_EQ(doc->rois.front().name, "Measured area");
            EXPECT_EQ(doc->rois.front().bounds(), QRectF(2.25, 3.5, 4.5, 5.25));
            EXPECT_EQ(doc->rois.front().color, QColor(12, 34, 56, 128).rgba());
            EXPECT_FALSE(doc->rois.front().visible);
            ASSERT_TRUE(doc->selection); EXPECT_EQ(doc->selection->points, doc->rois.front().points);
            name->setText("Discard this"); width->setValue(9);
            buttons->button(QDialogButtonBox::Cancel)->click();
        });
        action->trigger();
        ASSERT_EQ(doc->rois.size(), 1u); const auto &saved = doc->rois.front();
        EXPECT_EQ(saved.id, roi.id); EXPECT_EQ(saved.type, type);
        EXPECT_EQ(saved.name, "Measured area"); EXPECT_EQ(saved.bounds(), QRectF(2.25, 3.5, 4.5, 5.25));
        EXPECT_EQ(saved.z, 0u); EXPECT_EQ(saved.channel, 1u); EXPECT_EQ(saved.time, 1u);
        EXPECT_FALSE(doc->modified); EXPECT_FALSE(doc->history.canUndo());
        EXPECT_DOUBLE_EQ(doc->image.sample(3, 4, 2, 1, 1), 43210);
        window.commands().execute("analyze.measure"); ASSERT_TRUE(finishJobs(window));
        window.showPanel("measurements");
        auto *results = window.panel("measurements")->findChild<QTableView *>(); ASSERT_NE(results, nullptr);
        EXPECT_EQ(results->model()->data(results->model()->index(0, 1)).toString(), "Measured area");
        if (type == RoiType::Rectangle) {
            EXPECT_EQ(results->model()->data(results->model()->index(0, 2)).toULongLong(), 30u);
            EXPECT_DOUBLE_EQ(results->model()->data(results->model()->index(0, 8)).toDouble(), 43210);
        }
    }
}

TEST(Desktop, SelectionPropertiesEditLineEndpointsAndRejectZeroLength)
{
    MainWindow window;
    auto *doc = window.addDocument(Image::allocate(PixelType::UInt8, {16, 16}));
    Roi roi; roi.type = RoiType::Line; roi.points = {{9, 8}, {2, 3}}; doc->setSelection(roi);
    QTimer::singleShot(0, &window, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); ASSERT_NE(dialog, nullptr);
        auto *x1 = dialog->findChild<QDoubleSpinBox *>("roiX1"); ASSERT_NE(x1, nullptr);
        auto *y1 = dialog->findChild<QDoubleSpinBox *>("roiY1"); ASSERT_NE(y1, nullptr);
        auto *x2 = dialog->findChild<QDoubleSpinBox *>("roiX2"); ASSERT_NE(x2, nullptr);
        auto *y2 = dialog->findChild<QDoubleSpinBox *>("roiY2"); ASSERT_NE(y2, nullptr);
        EXPECT_DOUBLE_EQ(x1->value(), 9); EXPECT_DOUBLE_EQ(y1->value(), 8);
        EXPECT_DOUBLE_EQ(x2->value(), 2); EXPECT_DOUBLE_EQ(y2->value(), 3);
        x1->setValue(2); y1->setValue(3);
        auto *buttons = dialog->findChild<QDialogButtonBox *>(); ASSERT_NE(buttons, nullptr);
        EXPECT_FALSE(buttons->button(QDialogButtonBox::Ok)->isEnabled());
        EXPECT_FALSE(buttons->button(QDialogButtonBox::Apply)->isEnabled());
        auto *error = dialog->findChild<QLabel *>("roiGeometryError"); ASSERT_NE(error, nullptr); EXPECT_TRUE(error->isVisible());
        EXPECT_EQ(doc->selection->points, roi.points);
        x1->setValue(12.125); y1->setValue(10.75); x2->setValue(0); y2->setValue(0);
        EXPECT_TRUE(buttons->button(QDialogButtonBox::Ok)->isEnabled()); EXPECT_FALSE(error->isVisible());
        buttons->button(QDialogButtonBox::Ok)->click();
    });
    window.commands().execute("selection.properties");
    ASSERT_TRUE(doc->selection);
    EXPECT_EQ(doc->selection->points, (std::vector<QPointF>{{12.125, 10.75}, {0, 0}}));
    EXPECT_EQ(doc->selection->id, roi.id); EXPECT_EQ(doc->selection->type, RoiType::Line);
    EXPECT_TRUE(doc->rois.empty()); EXPECT_FALSE(doc->modified);
}

TEST(Desktop, SelectionPropertiesPreserveUneditedCoordinatesAndCancelAppearance)
{
    for (const auto type : {RoiType::Rectangle, RoiType::Ellipse, RoiType::Line, RoiType::Polygon}) {
        MainWindow window;
        auto *doc = window.addDocument(Image::allocate(PixelType::UInt8, {16, 16}));
        Roi roi; roi.type = type; roi.points = {{1.123456789, 2.987654321}, {8.765432109, 9.123456789}};
        if (type == RoiType::Polygon) roi.points.push_back({2.123456789, 12.987654321});
        doc->setSelection(roi);
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); ASSERT_NE(dialog, nullptr);
            auto *name = dialog->findChild<QLineEdit *>("roiName"); ASSERT_NE(name, nullptr); name->setText("Keep precise geometry");
            if (type == RoiType::Polygon) EXPECT_TRUE(dialog->findChildren<QDoubleSpinBox *>().isEmpty());
            auto *buttons = dialog->findChild<QDialogButtonBox *>(); ASSERT_NE(buttons, nullptr);
            buttons->button(QDialogButtonBox::Ok)->click();
        });
        window.commands().execute("selection.properties");
        ASSERT_TRUE(doc->selection); EXPECT_EQ(doc->selection->name, "Keep precise geometry");
        EXPECT_EQ(doc->selection->points, roi.points);
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); ASSERT_NE(dialog, nullptr);
            dialog->findChild<QLineEdit *>("roiName")->setText("Discard");
            dialog->findChild<QCheckBox *>("roiVisible")->setChecked(false);
            auto *color = dialog->findChild<QPushButton *>("roiColor"); ASSERT_NE(color, nullptr);
            QTimer::singleShot(0, dialog, [] {
                auto *picker = qobject_cast<QColorDialog *>(QApplication::activeModalWidget()); ASSERT_NE(picker, nullptr);
                picker->setCurrentColor(Qt::red); picker->reject();
            });
            color->click(); EXPECT_EQ(color->text(), QColor::fromRgba(roi.color).name(QColor::HexArgb));
            dialog->reject();
        });
        window.commands().execute("selection.properties");
        ASSERT_TRUE(doc->selection); EXPECT_EQ(doc->selection->name, "Keep precise geometry");
        EXPECT_EQ(doc->selection->points, roi.points); EXPECT_EQ(doc->selection->color, roi.color);
        EXPECT_TRUE(doc->selection->visible); EXPECT_TRUE(doc->rois.empty()); EXPECT_FALSE(doc->modified);
    }
}

TEST(Desktop, SelectionPropertiesRequireAnActiveSelection)
{
    MainWindow window;
    window.commands().execute("selection.properties");
    auto *doc = window.addDocument(Image::allocate(PixelType::UInt8, {16, 16}));
    QString error;
    QTimer dismiss;
    QObject::connect(&dismiss, &QTimer::timeout, [&] {
        if (auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            error = dialog->text(); dialog->accept();
        }
    });
    dismiss.start(1);
    window.commands().execute("selection.properties"); EXPECT_EQ(error, "Select an ROI first.");
    Roi roi; roi.points = {{1, 1}, {4, 4}}; doc->rois.push_back(roi); doc->setSelection(roi);
    window.commands().execute("selection.none");
    error.clear(); window.commands().execute("selection.properties"); EXPECT_EQ(error, "Select an ROI first.");
    EXPECT_EQ(window.findChild<QDialog *>("selectionPropertiesDialog"), nullptr);
    EXPECT_EQ(doc->rois.front().points, roi.points);
}

TEST(Desktop, GelCommandsPlotSnapshotMeasureLabelRemoveAndExportBands)
{
    MainWindow window;
    auto image = Image::allocate(PixelType::UInt16, {6, 9, 2, 2, 2});
    image.metadata().name = "=Gel";
    image.metadata().axes[1].spacing = .25; image.metadata().axes[1].unit = "mm";
    image.metadata().intensityScale = 2; image.metadata().intensityOffset = 3; image.metadata().intensityUnit = "a.u.";
    const std::array signal{0, 2, 4, 2, 0, 0, 6, 0, 0};
    for (std::size_t y = 0; y < 9; ++y) for (std::size_t x = 0; x < 6; ++x)
        image.setValue(std::array<std::size_t, 5>{x, y, 1, 1, 1}, 100 - signal[y] * (x >= 3 ? 2 : 1));
    auto *doc = window.addDocument(image); doc->setPosition(0, 0, 1, 1, 1);
    Roi roi; roi.points = {{0, 0}, {2, 9}}; roi.z = roi.channel = roi.time = 1;
    doc->setSelection(roi); window.commands().execute("gel.first");
    roi.points = {{3, 0}, {5, 9}}; doc->setSelection(roi); window.commands().execute("gel.next");
    ASSERT_EQ(doc->gelLanes.size(), 2); EXPECT_TRUE(doc->rois.empty()); EXPECT_FALSE(doc->modified);
    window.commands().execute("gel.plot");
    doc->image.setValue(std::array<std::size_t, 5>{0, 2, 1, 1, 1}, 60000);
    doc->setPosition(0, 0, 0); doc->setSelection({});
    ASSERT_TRUE(finishJobs(window));
    auto *plot = window.findChild<QDialog *>("gelWindow"); ASSERT_NE(plot, nullptr);
    EXPECT_TRUE(plot->windowTitle().contains("Z2, C2, T2"));
    auto *series = plot->findChild<QLineSeries *>(); ASSERT_NE(series, nullptr);
    ASSERT_EQ(series->points().size(), 9); EXPECT_EQ(series->points()[2], QPointF(.5, -195));
    auto *first = plot->findChild<QSpinBox *>("gelBandStart"); ASSERT_NE(first, nullptr);
    auto *last = plot->findChild<QSpinBox *>("gelBandEnd"); ASSERT_NE(last, nullptr);
    auto *measure = plot->findChild<QPushButton *>("gelMeasure"); ASSERT_NE(measure, nullptr);
    auto *lane = plot->findChild<QComboBox *>("gelLane"); ASSERT_NE(lane, nullptr);
    auto *table = plot->findChild<QTableView *>("gelResults"); ASSERT_NE(table, nullptr);
    auto *model = table->model();
    last->setValue(5); measure->click();
    QString bandError;
    QTimer dismissBand;
    QObject::connect(&dismissBand, &QTimer::timeout, [&] {
        if (auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            bandError = dialog->text(); dialog->accept();
        }
    });
    dismissBand.start(1); measure->click(); dismissBand.stop();
    EXPECT_TRUE(bandError.contains("overlaps")); EXPECT_EQ(model->rowCount(), 1);
    first->setValue(6); last->setValue(8); measure->click();
    ASSERT_EQ(model->rowCount(), 2);
    EXPECT_DOUBLE_EQ(model->index(0, 6).data().toDouble(), 4);
    EXPECT_DOUBLE_EQ(model->index(1, 6).data().toDouble(), 3);
    EXPECT_NEAR(model->index(0, 7).data().toDouble(), 400. / 7, 1e-12);
    lane->setCurrentIndex(1); last->setValue(5); measure->click();
    ASSERT_EQ(model->rowCount(), 3); EXPECT_DOUBLE_EQ(model->index(2, 6).data().toDouble(), 8);
    EXPECT_DOUBLE_EQ(model->index(2, 7).data().toDouble(), 100);
    table->selectRow(1); plot->findChild<QPushButton *>("gelRemove")->click();
    ASSERT_EQ(model->rowCount(), 2); EXPECT_DOUBLE_EQ(model->index(0, 7).data().toDouble(), 100);
    window.commands().execute("gel.label");
    auto *quick = plot->findChild<QQuickWidget *>(); ASSERT_NE(quick, nullptr);
    auto *root = quick->rootObject(); ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->property("bandLabels").toList().size(), 1);
    EXPECT_TRUE(root->property("bandVisible").toBool());
    EXPECT_EQ(root->property("bandStart").toPointF(), QPointF(0, -203));
    EXPECT_EQ(root->property("bandEnd").toPointF(), QPointF(1, -203));
    if (const auto path = qEnvironmentVariable("IMAGECXX_GEL_CAPTURE_PATH"); !path.isEmpty()) {
        QElapsedTimer timer; timer.start();
        while (timer.elapsed() < 200) { QCoreApplication::processEvents(); QThread::msleep(2); }
        EXPECT_TRUE(plot->grab().save(path));
    }
    auto *menu = window.findChild<QMenu *>("windowMenu"); ASSERT_NE(menu, nullptr);
    menu->aboutToShow();
    bool listed = false; for (auto *action : menu->actions()) if (action->text() == plot->windowTitle()) listed = true;
    EXPECT_TRUE(listed);
    QTemporaryDir directory; ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath("gel.csv");
    QTimer picker;
    QObject::connect(&picker, &QTimer::timeout, &window, [path] {
        auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget()); if (!dialog) return;
        dialog->setDirectory(QFileInfo(path).absolutePath()); dialog->selectFile(QFileInfo(path).fileName());
        if (auto *name = dialog->findChild<QLineEdit *>("fileNameEdit")) name->setText(QFileInfo(path).fileName());
        QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
    });
    picker.start(1); plot->findChild<QPushButton *>("gelExport")->click(); picker.stop();
    QFile csv(path); ASSERT_TRUE(csv.open(QIODevice::ReadOnly));
    const auto contents = csv.readAll(); EXPECT_TRUE(contents.contains("Percent of lane"));
    EXPECT_TRUE(contents.contains("\"Text: =Gel"));
    EXPECT_TRUE(contents.contains("\"4\",\"100\",\"a.u.·mm\""));
    EXPECT_EQ(contents.count('\n'), 3);
}

TEST(Desktop, GelCommandsValidateLaneScopeKeepDocumentsIndependentAndCancel)
{
    MainWindow window;
    auto *doc = window.addDocument(Image::allocate(PixelType::UInt8, {8, 8, 2}));
    QString error;
    QTimer dismiss;
    QObject::connect(&dismiss, &QTimer::timeout, [&] {
        if (auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) { error = dialog->text(); dialog->accept(); }
    }); dismiss.start(1);
    window.commands().execute("gel.first"); EXPECT_TRUE(error.contains("rectangular"));
    Roi roi; roi.points = {{0, 0}, {2, 8}}; doc->setSelection(roi);
    window.commands().execute("gel.next"); EXPECT_TRUE(error.contains("first lane"));
    window.commands().execute("gel.first"); ASSERT_EQ(doc->gelLanes.size(), 1);
    roi.points = {{1, 0}, {3, 8}}; doc->setSelection(roi);
    window.commands().execute("gel.next"); EXPECT_TRUE(error.contains("overlaps"));
    roi.points = {{4, 0}, {7, 8}}; doc->setSelection(roi);
    window.commands().execute("gel.next"); EXPECT_TRUE(error.contains("same size"));
    roi.points = {{4, 0}, {6, 8}}; doc->setSelection(roi); doc->setPosition(0, 0, 1);
    window.commands().execute("gel.next"); EXPECT_TRUE(error.contains("same size"));
    auto *other = window.addDocument(Image::allocate(PixelType::UInt8, {8, 8}));
    window.commands().execute("gel.plot"); EXPECT_TRUE(error.contains("first lane"));
    EXPECT_TRUE(other->gelLanes.empty()); EXPECT_EQ(doc->gelLanes.size(), 1);
    window.commands().execute("gel.options", {{"orientation", "Horizontal"}, {"darkBands", false}});
    EXPECT_TRUE(other->gelHorizontal); EXPECT_FALSE(other->gelDarkBands);
    QTimer::singleShot(0, &window, [] {
        auto *form = qobject_cast<QDialog *>(QApplication::activeModalWidget()); ASSERT_NE(form, nullptr);
        auto *direction = form->findChild<QComboBox *>("orientation"); ASSERT_NE(direction, nullptr);
        auto *dark = form->findChild<QCheckBox *>("darkBands"); ASSERT_NE(dark, nullptr);
        EXPECT_EQ(direction->currentText(), "Horizontal"); EXPECT_FALSE(dark->isChecked()); form->accept();
    });
    window.executeCommand("gel.options");
    roi.points = {{0, 0}, {8, 2}}; other->setSelection(roi); window.commands().execute("gel.first");
    window.commands().execute("gel.plot"); window.jobs().cancelAll(); ASSERT_TRUE(finishJobs(window));
    EXPECT_EQ(window.findChild<QDialog *>("gelWindow"), nullptr);
}

