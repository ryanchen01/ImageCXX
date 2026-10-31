#pragma once
#include "app/command_registry.h"
#include "app/job_scheduler.h"
#include "ui/document_widget.h"
#include <QMainWindow>
#include <QColor>
#include <QMap>
#include <QPointer>
class QMenu;
class QActionGroup;
class QTreeWidget;
class QLineEdit;
class QListWidget;
class QTableView;
class QStandardItemModel;
class QDoubleSpinBox;
class QSpinBox;
class QSlider;
class QCheckBox;
class QLabel;
class QProgressBar;
class QCloseEvent;
namespace imagecxx { class PlotWidget; }

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
    void openPaths(const QStringList &paths);
    imagecxx::DocumentWidget *activeDocument() const;
    QList<imagecxx::DocumentWidget *> documents() const;
    imagecxx::DocumentWidget *addDocument(imagecxx::Image image);
    QWidget *panel(const QString &id) const;
    void showPanel(const QString &id);
    const imagecxx::CommandRegistry &commands() const { return commands_; }
    imagecxx::JobScheduler &jobs() { return jobs_; }
    void executeCommand(const QString &id);
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
private:
    imagecxx::CommandRegistry commands_;
    imagecxx::JobScheduler jobs_;
    QList<QPointer<imagecxx::DocumentWidget>> documents_;
    QPointer<imagecxx::DocumentWidget> active_;
    QMap<QString, QWidget *> panels_;
    QMap<QString, QByteArray> panelGeometries_;
    QTreeWidget *metadata_ = nullptr;
    QLineEdit *metadataSearch_ = nullptr;
    QListWidget *rois_ = nullptr;
    QListWidget *history_ = nullptr;
    QTableView *resultsView_ = nullptr;
    QStandardItemModel *results_ = nullptr;
    int exportedMeasurementRows_ = 0;
    QDoubleSpinBox *minimum_ = nullptr;
    QDoubleSpinBox *maximum_ = nullptr;
    QDoubleSpinBox *gamma_ = nullptr;
    QSpinBox *channelIndex_ = nullptr;
    QSpinBox *timeIndex_ = nullptr;
    QSlider *minimumSlider_ = nullptr;
    QSlider *maximumSlider_ = nullptr;
    QSlider *gammaSlider_ = nullptr;
    double sliderMinimum_ = 0;
    double sliderMaximum_ = 1;
    QMenu *lutMenu_;
    QAction *invertLut_;
    QCheckBox *linear_ = nullptr;
    QCheckBox *composite_ = nullptr;
    QLabel *pixelStatus_;
    QLabel *jobStatus_;
    QProgressBar *progress_;
    imagecxx::PlotWidget *histogramPlot_ = nullptr;
    imagecxx::PlotWidget *profilePlot_ = nullptr;
    QActionGroup *tools_ = nullptr;
    QAction *crosshair_ = nullptr;
    QAction *scaleBar_ = nullptr;
    QList<QAction *> imageActions_;
    QStringList recentFiles_;
    QMenu *recentMenu_ = nullptr;
    QMenu *windowMenu_ = nullptr;
    bool loadingPanels_ = false;
    QPointer<imagecxx::DocumentWidget> propertiesDocument_;
    std::optional<imagecxx::ImageMetadata> propertiesMetadata_;
    std::vector<std::size_t> propertiesShape_;
    imagecxx::PixelType propertiesType_ = imagecxx::PixelType::UInt8;
    QString propertiesStorage_;
    QMap<QKeySequence, QAction *> shortcutActions_;
    QColor drawingColor_ = Qt::white;
    QString repeatCommand_;
    QVariantMap repeatParameters_;
    void importRaw(const QString &path);
    void pasteImage(bool systemImage, bool newImage);
    void openFiles(const QStringList &paths);
    void setActiveDocument(imagecxx::DocumentWidget *document);
    void clearDocuments();
    void ensurePanel(const QString &id);
    void updatePanels();
    void filterProperties();
    void updateDisplayControls();
    void updateRecentFiles();
    void updateWindowMenu();
    QByteArray panelLayout() const;
    void restorePanelLayout(const QByteArray &layout);
    void showError(const QString &message);
    bool showJobError(std::exception_ptr error);
    bool confirmDiscardMeasurements();
    std::optional<QVariantMap> parameterForm(const imagecxx::Command &command, bool batch = false);
    std::optional<imagecxx::Roi> selectedRoi() const;
    void showPlot(bool profile);
    QWidget *plotWindow(bool profile);
};
