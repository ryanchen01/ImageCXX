#pragma once
#include "core/history.h"
#include "core/roi.h"
#include "image_window.h"
#include <QWidget>
#include <QList>
#include <array>
class QSlider;
class QCloseEvent;
class QEvent;
class QLabel;
class QVBoxLayout;
namespace imagecxx {
class DocumentWidget : public ImageWindow {
    Q_OBJECT
public:
    enum class ViewMode { Single, Linked, Orthogonal };
    Q_ENUM(ViewMode)
    explicit DocumentWidget(Image image, QWidget *parent = nullptr);
    ~DocumentWidget() override;
    Image image;
    std::vector<Roi> rois;
    std::optional<Roi> selection;
    std::optional<Roi> previousSelection;
    bool selectionCleared = false;
    void setSelection(std::optional<Roi> roi);
    void editRoi(Roi roi);
    void refreshRois();
    ImageHistory history;
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
    QList<ImageViewer *> views_;
    struct NavigationControls {
        QSlider *slice;
        QLabel *axis;
        QLabel *position;
    };
    QList<NavigationControls> navigation_;
    std::array<std::size_t, 3> spatial_{};
    std::array<double, 3> center_{};
    bool synchronizing_ = false;
    std::array<std::size_t, 3> viewAxes(int index) const;
    void connectViewer(ImageViewer *view, int index);
    void addNavigationControls(ImageViewer *view, int index, QVBoxLayout *layout);
    void updateNavigation(std::size_t channel, std::size_t time);
    void synchronizeDisplay(ImageViewer *source);
    void synchronizeTransform(ImageViewer *source, int index);
};
} // namespace imagecxx
