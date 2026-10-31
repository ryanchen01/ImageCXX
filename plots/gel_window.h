#pragma once
#include "core/roi.h"
#include <QDialog>
class QComboBox;
class QSpinBox;
class QStandardItemModel;
namespace imagecxx {
class PlotWidget;
class GelWindow : public QDialog {
    Q_OBJECT
public:
    GelWindow(std::vector<LineProfile> profiles, QString title, bool darkBands, QWidget *parent);
    void labelPeaks();
private:
    std::vector<LineProfile> profiles_;
    PlotWidget *plot_;
    QComboBox *lane_;
    QSpinBox *first_;
    QSpinBox *last_;
    QStandardItemModel *results_;
    void updateAnnotations();
};
}
