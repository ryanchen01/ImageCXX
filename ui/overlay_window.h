#pragma once
#include "document_widget.h"
#include <array>
#include <memory>
#include <unordered_map>

class QDialog;
class QTreeWidget;
class QSlider;
class QComboBox;
class QSpinBox;

namespace imagecxx {
class OverlayWindow : public ImageWindow {
    Q_OBJECT
public:
    explicit OverlayWindow(DocumentWidget *document, QWidget *parent = nullptr);
    ~OverlayWindow() override;
    DocumentWidget *document() const { return document_; }
    bool orthogonalViews() const { return orthogonal_; }
    void setOrthogonalViews(bool enabled);
    void showViews();
    const QList<ImageViewer *> &views() const { return views_; }
    std::array<std::size_t, 3> spatialPosition() const { return spatial_; }
    void setPosition(std::size_t x, std::size_t y, std::size_t z,
                     std::size_t channel = 0, std::size_t time = 0);
    void refreshDocuments(const QList<DocumentWidget *> &documents);
    void showEditor();
signals:
    void activated();
    void visibilityChanged();
    void viewError(const QString &message);
protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
private:
    struct LayerPlanes {
        quint64 revision = 0;
        std::array<std::unique_ptr<ImageViewer>, 2> planes;
        QMetaObject::Connection displayConnection;
    };
    QPointer<DocumentWidget> document_;
    QDialog *editor_;
    QTreeWidget *images_;
    QComboBox *lut_;
    QSpinBox *opacity_;
    QSlider *opacitySlider_;
    bool orthogonal_ = false;
    QList<ImageViewer *> views_;
    QList<SliceControls> navigation_;
    std::array<std::size_t, 3> spatial_{};
    std::array<double, 3> center_{};
    std::unordered_map<DocumentWidget *, LayerPlanes> layerPlanes_;
    bool synchronizing_ = false;
    quint64 revision_;
    QString baseTitle() const;
    void addNavigationControls(ImageWindow *window, int index);
    void connectViewer(ImageViewer *view, int index);
    void updateNavigation(std::size_t channel, std::size_t time);
    void syncLayerDisplay(DocumentWidget *document);
    void refreshEditor();
    void removeDocument(DocumentWidget *document);
    void updateLayers();
};
} // namespace imagecxx
