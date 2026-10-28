#pragma once

#include "core/image.h"
#include "core/roi.h"
#include <QWidget>
#include <QList>
#include <QPointer>
#include <QTransform>
#include <memory>
#include <utility>

namespace imagecxx {

// The Qt RHI implementation and GPU resources stay private to this module.
class ImageViewer final : public QWidget {
    Q_OBJECT
public:
    enum class Tool { Pointer, Pan, Rectangle, Ellipse, Polygon, Line };
    Q_ENUM(Tool)

    explicit ImageViewer(QWidget *parent = nullptr);
    ~ImageViewer() override;
    // Keep a manual inversion choice when replacing pixels in the same document.
    void setImage(const Image &image, bool preserveInversion = false);
    const Image &image() const;
    void setSlice(std::size_t z, std::size_t channel = 0, std::size_t time = 0);
    std::size_t slice() const;
    std::size_t channel() const;
    std::size_t time() const;
    void setWindow(double minimum, double maximum);
    double windowMinimum() const;
    double windowMaximum() const;
    std::pair<double, double> sliceRange() const;
    // Fit the current slice once, keeping the range when navigating.
    void autoWindow();
    void setGamma(double gamma);
    double gamma() const;
    void setInverted(bool inverted);
    bool isInverted() const;
    void setLut(const QString &name);
    QString lut() const;
    void setLinearInterpolation(bool enabled);
    bool linearInterpolation() const;
    void setComposite(bool enabled);
    bool composite() const;
    struct OverlayLayer {
        QPointer<ImageViewer> viewer;
        QString lut;
        double opacity = 1;
        QTransform transform;
    };
    // Layers are drawn from bottom to top in this viewer's image coordinates.
    void setOverlayLayers(const QList<OverlayLayer> &layers);
    static constexpr double minimumZoom = 1e-6;
    static constexpr double maximumZoom = 256.0;
    void fitToWindow();
    void setZoom(double scale);
    double zoom() const;
    bool fitsWindow() const;
    QPointF viewCenter() const;
    void setViewTransform(double scale, QPointF center);
    void setPixelAspectRatio(double heightOverWidth);
    void setTool(Tool tool);
    Tool tool() const;
    void setRois(const std::vector<Roi> &rois);
    void setSelection(std::optional<Roi> roi);
    void setMask(const Image &mask);
    void setCrosshairVisible(bool visible);
    void setScaleBarVisible(bool visible);
    bool crosshairVisible() const;
    bool scaleBarVisible() const;
    void setViewLabel(const QString &label);
    // A linked intersection uses image coordinates. Pointer-tool drags move it;
    // middle-button and Space drags continue to pan the image.
    void setNavigationCrosshair(std::optional<QPointF> position);
    // Limits for retained decoded slices and GPU textures. Oversized slices fail.
    void setCacheBudgets(std::size_t decodedBytes, std::size_t gpuBytes);
    std::size_t decodedCacheBytes() const;
    std::size_t gpuCacheBytes() const;
    QPointF imageToWidget(QPointF point) const;
    QPointF widgetToImage(QPointF point) const;

Q_SIGNALS:
    void sliceChanged(std::size_t z, std::size_t channel, std::size_t time);
    void pixelHovered(int x, int y, double originalValue);
    void roiCreated(const imagecxx::Roi &roi);
    void roiSelected(const imagecxx::Roi &roi);
    void roiEdited(const imagecxx::Roi &roi);
    void selectionCleared();
    void displayRangeChanged(double minimum, double maximum);
    void zoomChanged(double scale);
    void viewTransformChanged();
    void displaySettingsChanged();
    void toolChanged();
    void overlaysChanged(bool crosshair, bool scaleBar);
    void positionPicked(QPointF point);
    void loadingChanged(bool loading);
    void sliceReady();
    void loadFailed(const QString &message);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private:
    class Private;
    std::unique_ptr<Private> d;
    void requestSlice();
    void updateCanvas();
    void completeRoi();
    Qt::CursorShape toolCursor() const;
    void updateRoiDrag(QPointF point, Qt::KeyboardModifiers modifiers);
};

} // namespace imagecxx

Q_DECLARE_METATYPE(imagecxx::Roi)
