#pragma once
#include "viewer/image_viewer.h"
#include <QList>
#include <QWidget>
#include <array>

class QLabel;
class QSlider;

namespace imagecxx {
void keepWindowOnScreen(QWidget *window);
void activateApplicationWindow(QWidget *window, const QString &activationToken = {}, const QString &startupId = {});

struct SliceControls {
    QSlider *slice;
    QLabel *axis;
    QLabel *position;
};

std::array<std::size_t, 3> orthogonalAxes(bool orthogonal, int index);
SliceControls addSliceControls(class ImageWindow *window);
void updateSliceControls(const Image &image, bool orthogonal, const QList<ImageViewer *> &views,
                         const QList<SliceControls> &controls, std::array<std::size_t, 3> spatial,
                         std::size_t channel, std::size_t time, const QString &baseTitle);
void synchronizeOrthogonalTransform(const Image &image, bool orthogonal, const QList<ImageViewer *> &views,
                                    ImageViewer *source, int index, std::array<double, 3> &center);

class ImageWindow : public QWidget {
public:
    explicit ImageWindow(QWidget *parent = nullptr);
    void resizeToImage(double pixelAspectRatio = 0.0);
    ImageViewer *viewer;
protected:
    void showEvent(QShowEvent *event) override;
};
} // namespace imagecxx
