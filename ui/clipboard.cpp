#include "ui/clipboard.h"
#include "app/job_scheduler.h"
#include "core/memory.h"
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QCoreApplication>
#include <QMimeData>
#include <QPointer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <stop_token>

namespace imagecxx {
namespace {
// Keep scientific pixels in process while offering PNG to other applications.
class ImageMimeData : public QMimeData {
public:
    ImageMimeData() { setData("application/x-imagecxx-image", {}); }
    std::shared_ptr<const Image> image;
};

QImage displayRaster(const Image &image, double minimum, double maximum, bool inverted, std::stop_token stop = {})
{
    QImage raster(int(image.width()), int(image.height()), QImage::Format_RGBA8888);
    if (raster.isNull())
        throw std::runtime_error(QCoreApplication::translate("MainWindow", "Cannot allocate display image").toStdString());
    const auto components = componentCount(image.pixelType());
    for (int y = 0; y < raster.height(); ++y) {
        if (stop.stop_requested()) return {};
        for (int x = 0; x < raster.width(); ++x) {
            auto *pixel = raster.scanLine(y) + 4 * x;
            for (int c = 0; c < 4; ++c) {
                double value = c == 3 && components != 4 ? 255 : image.sample(x, y, 0, 0, 0, components >= 3 ? c : 0);
                if (c < 3 && components < 3) value = maximum > minimum ? 255 * (value - minimum) / (maximum - minimum) : 0;
                value = std::clamp(std::isfinite(value) ? value : 0., 0., 255.);
                pixel[c] = uchar(inverted && c < 3 ? 255. - value : value);
            }
        }
    }
    return raster;
}
} // namespace

QString propertyClipboardText(QTreeWidget *tree, int column, const QTreeWidgetItem *clicked)
{
    const bool copySelection = !clicked || clicked->isSelected();
    QStringList lines;
    for (QTreeWidgetItemIterator it(tree, QTreeWidgetItemIterator::NotHidden); *it; ++it) {
        const auto *item = *it;
        if (copySelection ? !item->isSelected() : item != clicked) continue;
        if (column >= 0) lines.append(item->text(column));
        else {
            QStringList fields{item->text(0), item->text(1)};
            for (auto &field : fields) {
                field.replace('"', "\"\"");
                field = QLatin1Char('"') + field + QLatin1Char('"');
            }
            lines.append(fields.join(','));
        }
    }
    return lines.join('\n');
}

Image imageFromRaster(QImage raster)
{
    if (raster.isNull()) return {};
    PixelType type;
    if (raster.format() == QImage::Format_Grayscale16) type = PixelType::UInt16;
    else if (raster.isGrayscale() && !raster.hasAlphaChannel()) {
        type = PixelType::UInt8;
        raster = raster.convertToFormat(QImage::Format_Grayscale8);
    } else if (raster.hasAlphaChannel()) {
        type = PixelType::RGBA8;
        raster = raster.convertToFormat(QImage::Format_RGBA8888);
    } else {
        type = PixelType::RGB8;
        raster = raster.convertToFormat(QImage::Format_RGB888);
    }
    if (raster.isNull())
        throw std::runtime_error(QCoreApplication::translate("MainWindow", "Cannot convert clipboard image").toStdString());
    auto image = Image::allocate(type, {std::size_t(raster.width()), std::size_t(raster.height())});
    auto *data = image.mutableData();
    const auto rowBytes = image.width() * bytesPerPixel(type);
    for (int y = 0; y < raster.height(); ++y) std::memcpy(data + y * rowBytes, raster.constScanLine(y), rowBytes);
    return image;
}

void copyImageToClipboard(QObject *owner, const Image &slice, double minimum, double maximum, bool inverted,
                          JobScheduler &jobs, const std::function<void(const QString &)> &onPngUnavailable)
{
    auto mime = std::make_unique<ImageMimeData>();
    mime->image = std::make_shared<Image>(slice.clone());
    const std::weak_ptr<const Image> snapshot = mime->image;
    const QPointer<ImageMimeData> pending = mime.get();
    std::stop_source cancellation;
    QObject::connect(mime.get(), &QObject::destroyed, owner, [cancellation]() mutable { cancellation.request_stop(); });
    QApplication::clipboard()->setMimeData(mime.release());
    auto output = std::make_shared<std::pair<QImage, QByteArray>>();
    jobs.submit(QCoreApplication::translate("MainWindow", "Preparing clipboard PNG"),
        [snapshot, minimum, maximum, inverted, output, cancellation](auto stop, auto progress) mutable {
            std::stop_callback cancelled(stop, [cancellation]() mutable { cancellation.request_stop(); });
            const auto token = cancellation.get_token();
            const auto held = snapshot.lock();
            if (!held || token.stop_requested()) return;
            const auto &image = *held;
            // Include the source raster, scaled raster, encoded PNG capacity and codec working buffers.
            const long double scale = std::max(1.L, 320.L / std::max(image.width(), image.height()));
            const auto width = std::ceil(image.width() * scale), height = std::ceil(image.height() * scale);
            const auto required = 4.L * image.width() * image.height() + 12.L * width * height + 8 * 1024 * 1024;
            constexpr std::size_t exportBudget = 512 * 1024 * 1024;
            if (width > std::numeric_limits<int>::max() || height > std::numeric_limits<int>::max() || required > exportBudget)
                throw std::length_error(QCoreApplication::translate("MainWindow", "PNG export exceeds the clipboard memory limit.").toStdString());
            detail::checkMemoryBudget(std::size_t(required), detail::availableMemoryBytes());
            auto &[raster, bytes] = *output;
            raster = displayRaster(image, minimum, maximum, inverted, token);
            if (token.stop_requested()) return;
            progress(.5);
            if (raster.width() < 320 && raster.height() < 320)
                raster = raster.scaled(QSize(320, 320), Qt::KeepAspectRatio, Qt::FastTransformation);
            if (token.stop_requested()) return;
            QBuffer png(&bytes);
            if (!png.open(QIODevice::WriteOnly) || !raster.save(&png, "PNG"))
                throw std::runtime_error(QCoreApplication::translate("MainWindow", "Cannot encode clipboard PNG").toStdString());
            progress(1);
        }, [pending, output, cancellation, onPngUnavailable](auto error) {
            if (!pending || cancellation.stop_requested() || QApplication::clipboard()->mimeData() != pending.data()) return;
            if (error) {
                QString reason;
                try { std::rethrow_exception(error); }
                catch (const std::exception &exception) { reason = QString::fromUtf8(exception.what()); }
                catch (...) { reason = QCoreApplication::translate("MainWindow", "Unknown error."); }
                if (onPngUnavailable) onPngUnavailable(reason);
                return;
            }
            auto ready = std::make_unique<ImageMimeData>();
            ready->image = pending->image;
            ready->setData("image/png", output->second);
            ready->setImageData(output->first);
            QApplication::clipboard()->setMimeData(ready.release());
        });
}

Image readImageFromClipboard(bool systemImage)
{
    const auto *mime = QApplication::clipboard()->mimeData();
    if (!mime) return {};
    if (const auto *native = dynamic_cast<const ImageMimeData *>(mime); native && !systemImage)
        return *native->image;
    QImage raster;
    if (mime->hasFormat("image/png")) raster.loadFromData(mime->data("image/png"), "PNG");
    if (raster.isNull()) raster = QApplication::clipboard()->image();
    return imageFromRaster(std::move(raster));
}

} // namespace imagecxx
