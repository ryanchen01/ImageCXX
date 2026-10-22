#pragma once
#include "core/history.h"
#include "core/roi.h"
#include "viewer/image_viewer.h"
#include <QWidget>
#include <QList>
#include <array>
class QSlider;
class QCloseEvent;
class QEvent;
class QLabel;
class QComboBox;
namespace imagecxx {
class DocumentWidget : public QWidget {
    Q_OBJECT
public:
    enum class ViewMode { Single, Linked, Orthogonal };
    Q_ENUM(ViewMode)
    explicit DocumentWidget(Image image, QWidget *parent = nullptr);
    Image image;
    std::vector<Roi> rois;
    std::optional<Roi> selection;
    std::optional<Roi> previousSelection;
    bool selectionCleared = false;
    void setSelection(std::optional<Roi> roi);
    void editRoi(Roi roi);
    void refreshRois();
    ImageHistory history;
    ImageViewer *viewer;
    ViewMode viewMode() const { return viewMode_; }
    void setViewMode(ViewMode mode);
    void showViews();
    const QList<ImageViewer *> &views() const { return views_; }
    std::array<std::size_t, 3> spatialPosition() const { return spatial_; }
    void setPosition(std::size_t x, std::size_t y, std::size_t z,
                     std::size_t channel = 0, std::size_t time = 0);
    quint64 revision = 0;
    bool modified = false;
    QString title() const;
    void replaceImage(Image replacement, const QString &label);
    void refresh();
signals:
    void changed();
    void activated();
    void closing();
    void pathsDropped(const QStringList &paths);
    void pixelHovered(std::size_t x, std::size_t y, std::size_t z, double value);
    void viewError(const QString &message);
    void displayChanged();
protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
private:
    ViewMode viewMode_ = ViewMode::Single;
    QComboBox *modeChoice_;
    QList<ImageViewer *> views_;
    std::array<std::size_t, 3> spatial_{};
    std::array<double, 3> center_{};
    bool synchronizing_ = false;
    std::array<std::size_t, 3> viewAxes(int index) const;
    void connectViewer(ImageViewer *view, int index);
    void updateNavigation(std::size_t channel, std::size_t time);
    void synchronizeDisplay(ImageViewer *source);
    void synchronizeTransform(ImageViewer *source, int index);
    QSlider *z_;
    QSlider *channel_;
    QSlider *time_;
    QLabel *position_;
    std::array<QLabel *, 3> axisLabels_{};
};
} // namespace imagecxx
