#pragma once
#include "core/history.h"
#include "core/roi.h"
#include "core/annotation.h"
#include "image_window.h"
#include <QWidget>
#include <QList>
#include <QPointer>
#include <array>
class QCloseEvent;
class QEvent;
namespace imagecxx {
std::optional<std::array<double, 3>> mapImagePosition(const Image &source, const Image &target,
                                                   std::array<double, 3> position);
class DocumentWidget : public ImageWindow {
    Q_OBJECT
public:
    explicit DocumentWidget(Image image, QWidget *parent = nullptr);
    ~DocumentWidget() override;
    Image image;
    std::vector<Roi> rois;
    std::vector<Annotation> annotations;
    bool annotationsVisible = true;
    std::vector<Roi> gelLanes;
    bool gelHorizontal = false;
    bool gelDarkBands = true;
    std::optional<Roi> selection;
    std::optional<Roi> previousSelection;
    bool selectionCleared = false;
    void setSelection(std::optional<Roi> roi);
    void editRoi(Roi roi);
    void refreshRois();
    ImageHistory history;
    bool orthogonalViews() const { return orthogonal_; }
    void setOrthogonalViews(bool enabled);
    bool isLinked() const { return !linkedDocuments_.isEmpty(); }
    void linkTo(QList<DocumentWidget *> documents);
    void unlink();
    const QList<QPointer<DocumentWidget>> &linkedDocuments() const { return linkedDocuments_; }
    void showViews();
    const QList<ImageViewer *> &views() const { return views_; }
    std::array<std::size_t, 3> spatialPosition() const { return spatial_; }
    void setPosition(std::size_t x, std::size_t y, std::size_t z,
                     std::size_t channel = 0, std::size_t time = 0, bool propagate = true);
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
    void toolDimensionsChanged(const QString &text);
    void pixelHovered(std::size_t x, std::size_t y, std::size_t z, double value);
    void viewError(const QString &message);
    void displayChanged();
    void positionChanged();
protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
private:
    bool orthogonal_ = false;
    QList<QPointer<DocumentWidget>> linkedDocuments_;
    QList<ImageViewer *> views_;
    QList<SliceControls> navigation_;
    std::array<std::size_t, 3> spatial_{};
    std::array<double, 3> center_{};
    bool synchronizing_ = false;
    void connectViewer(ImageViewer *view, int index);
    void addNavigationControls(ImageWindow *window, int index);
    void updateNavigation(std::size_t channel, std::size_t time);
    void synchronizeDisplay(ImageViewer *source);
    void synchronizeTransform(ImageViewer *source, int index);
};
} // namespace imagecxx
