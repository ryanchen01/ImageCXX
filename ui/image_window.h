#pragma once
#include "viewer/image_viewer.h"
#include <QWidget>

namespace imagecxx {
void keepWindowOnScreen(QWidget *window);

class ImageWindow : public QWidget {
public:
    explicit ImageWindow(QWidget *parent = nullptr);
    void resizeToImage(double pixelAspectRatio = 0.0);
    ImageViewer *viewer;
protected:
    void showEvent(QShowEvent *event) override;
};
} // namespace imagecxx
