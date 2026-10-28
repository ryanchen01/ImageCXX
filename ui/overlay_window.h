#pragma once
#include "document_widget.h"

class QDialog;
class QTreeWidget;
class QSlider;
class QLabel;
class QComboBox;
class QSpinBox;

namespace imagecxx {
class OverlayWindow : public ImageWindow {
    Q_OBJECT
public:
    explicit OverlayWindow(DocumentWidget *document, QWidget *parent = nullptr);
    ~OverlayWindow() override;
    DocumentWidget *document() const { return document_; }
    void refreshDocuments(const QList<DocumentWidget *> &documents);
    void showEditor();
signals:
    void activated();
    void viewError(const QString &message);
protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
private:
    QPointer<DocumentWidget> document_;
    QDialog *editor_;
    QTreeWidget *images_;
    QComboBox *lut_;
    QSpinBox *opacity_;
    QSlider *opacitySlider_;
    QSlider *slice_;
    QLabel *position_;
    bool synchronizing_ = false;
    quint64 revision_;
    void refreshEditor();
    void removeDocument(DocumentWidget *document);
    void updateLayers();
};
} // namespace imagecxx
