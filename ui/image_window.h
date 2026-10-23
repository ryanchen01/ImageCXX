#pragma once
#include "viewer/image_viewer.h"
#include <QWidget>

namespace imagecxx {
class ImageWindow : public QWidget {
public:
    explicit ImageWindow(QWidget *parent = nullptr);
    void resizeToImage(double pixelAspectRatio = 1.0);
    ImageViewer *viewer;
};
} // namespace imagecxx
