#pragma once
#include "core/history.h"
#include "core/roi.h"
#include "viewer/image_viewer.h"
#include <QWidget>
#include <array>
class QSlider;
class QCloseEvent;
class QEvent;
class QLabel;
namespace imagecxx {
class DocumentWidget : public QWidget {
    Q_OBJECT
public:
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
protected:
    bool event(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
private:
    QSlider *z_;
    QSlider *channel_;
    QSlider *time_;
    QLabel *position_;
    std::array<QLabel *, 3> axisLabels_{};
};
} // namespace imagecxx
