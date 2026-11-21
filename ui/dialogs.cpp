#include "ui/dialogs.h"
#include "mainwindow.h"
#include "app/job_scheduler.h"
#include "core/memory.h"
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QIcon>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMessageBox>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <tuple>
#include <type_traits>

namespace imagecxx {
std::optional<ResliceOptions> showResliceDialog(QWidget *parent, const Image &input)
{
    QSettings settings;
    settings.beginGroup("reslice/v1");
    ResliceOptions options;
    options.outputSpacing = settings.value("outputSpacing", input.metadata().axes.size() > 2 ? input.metadata().axes[2].spacing : 1).toDouble();
    if (input.depth() < 2)
        throw std::invalid_argument(MainWindow::tr("Reslice requires a Z stack with at least two slices").toStdString());
    QDialog dialog(parent);
    dialog.setObjectName("resliceDialog");
    dialog.setWindowTitle(MainWindow::tr("Reslice"));
    dialog.setMinimumWidth(360);
    auto *layout = new QFormLayout(&dialog);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setVerticalSpacing(14);
    auto *spacingLabel = new QLabel(&dialog);
    auto *spacing = new QDoubleSpinBox(&dialog);
    spacing->setObjectName("resliceSpacing");
    spacing->setDecimals(6);
    spacing->setRange(.000001, 1e9);
    spacing->setValue(options.outputSpacing);
    spacing->setMinimumWidth(130);
    spacing->setToolTip(MainWindow::tr("Distance between output planes in the swept source axis's units."));
    layout->addRow(spacingLabel, spacing);
    auto *start = new QComboBox(&dialog);
    start->setObjectName("resliceStart");
    start->addItems({MainWindow::tr("Top"), MainWindow::tr("Left"), MainWindow::tr("Bottom"), MainWindow::tr("Right")});
    start->setCurrentIndex(std::clamp(settings.value("start", 0).toInt(), 0, 3));
    layout->addRow(MainWindow::tr("Start at:"), start);
    auto *flip = new QCheckBox(MainWindow::tr("Flip vertically"), &dialog);
    flip->setObjectName("resliceFlip");
    auto *rotate = new QCheckBox(MainWindow::tr("Rotate 90 degrees"), &dialog);
    rotate->setObjectName("resliceRotate");
    auto *avoid = new QCheckBox(MainWindow::tr("Avoid interpolation"), &dialog);
    avoid->setObjectName("resliceAvoidInterpolation");
    flip->setChecked(settings.value("flipVertically", false).toBool());
    rotate->setChecked(settings.value("rotate90", false).toBool());
    avoid->setChecked(settings.value("avoidInterpolation", false).toBool());
    layout->addRow(flip);
    layout->addRow(rotate);
    layout->addRow(avoid);
    auto *hint = new QLabel(MainWindow::tr("(use 1 pixel spacing)"), &dialog);
    layout->addRow(hint);
    const auto &axes = input.metadata().axes;
    layout->addRow(new QLabel(MainWindow::tr("Voxel size: %1 × %2 × %3 %4").arg(axes[0].spacing, 0, 'g', 6)
        .arg(axes[1].spacing, 0, 'g', 6).arg(axes[2].spacing, 0, 'g', 6).arg(axes[0].unit), &dialog));
    auto *size = new QLabel(&dialog);
    size->setObjectName("resliceOutputSize");
    size->setWordWrap(true);
    layout->addRow(size);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Help, &dialog);
    for (auto *button : buttons->buttons()) button->setIcon(QIcon{});
    layout->addRow(buttons);
    const auto update = [&] {
        options.start = static_cast<ResliceStart>(start->currentIndex());
        options.outputSpacing = spacing->value();
        options.flipVertically = flip->isChecked();
        options.rotate90 = rotate->isChecked();
        options.avoidInterpolation = avoid->isChecked();
        spacing->setEnabled(!options.avoidInterpolation);
        const auto sweep = start->currentIndex() % 2 == 0 ? 1 : 0;
        spacingLabel->setText(MainWindow::tr("Output spacing (%1):").arg(options.avoidInterpolation ? MainWindow::tr("pixels") : axes[sweep].unit));
        try {
            const auto geometry = resliceGeometry(input, options);
            const auto available = detail::availableMemoryBytes();
            QStringList dimensions;
            for (auto length : geometry.shape) dimensions << QString::number(length);
            size->setText(MainWindow::tr("Output size: %1 MiB\n%2\nAvailable memory: %3 MiB").arg(double(geometry.byteCount) / (1024 * 1024), 0, 'f', 2)
                .arg(dimensions.join(QStringLiteral(" × "))).arg(double(available.value_or(0)) / (1024 * 1024), 0, 'f', 0));
            size->setToolTip(MainWindow::tr("Estimated output and working memory: %1 MiB. 10% of available memory is kept free. Memory is checked again when processing starts.")
                .arg(double(geometry.requiredMemoryBytes) / (1024 * 1024), 0, 'f', 2));
            detail::checkMemoryBudget(geometry.requiredMemoryBytes, available);
            buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
        } catch (const std::exception &error) {
            size->setText(QString::fromUtf8(error.what()));
            buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
        }
    };
    QObject::connect(spacing, &QDoubleSpinBox::valueChanged, &dialog, update);
    QObject::connect(start, &QComboBox::currentIndexChanged, &dialog, update);
    QObject::connect(flip, &QCheckBox::toggled, &dialog, update);
    QObject::connect(rotate, &QCheckBox::toggled, &dialog, update);
    double interpolatedSpacing = spacing->value();
    if (avoid->isChecked()) spacing->setValue(1);
    QObject::connect(avoid, &QCheckBox::toggled, &dialog, [&](bool checked) {
        const QSignalBlocker blocker(spacing);
        if (checked) { interpolatedSpacing = spacing->value(); spacing->setValue(1); }
        else spacing->setValue(interpolatedSpacing);
        update();
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::helpRequested, &dialog, [&dialog] {
        QMessageBox::information(&dialog, MainWindow::tr("Reslice help"),
            MainWindow::tr("Top and Bottom create XZ planes; Left and Right create YZ planes through the full image. "
                   "Output spacing is the distance between those planes. Z is interpolated at the profile axis's pixel spacing.\n\n"
                   "Flip vertically reverses the source Z order. Rotate exchanges the two plane axes, as in ImageJ. "
                   "Avoid interpolation copies source pixels exactly and keeps their original spacing.\n\n"
                   "Channels and time points are kept separately. The result opens in a new image window."));
    });
    update();
    spacing->selectAll();
    spacing->setFocus();
    if (dialog.exec() != QDialog::Accepted) return {};
    settings.setValue("outputSpacing", options.avoidInterpolation ? interpolatedSpacing : options.outputSpacing);
    settings.setValue("start", start->currentIndex());
    settings.setValue("flipVertically", options.flipVertically);
    settings.setValue("rotate90", options.rotate90);
    settings.setValue("avoidInterpolation", options.avoidInterpolation);
    settings.sync();
    return options;
}

std::optional<ProjectionOptions> showProjectionDialog(QWidget *parent, const Image &input, JobScheduler &jobs)
{
    QSettings settings;
    settings.beginGroup("projection3d/v1");
    settings.remove("lower");
    settings.remove("upper");
    ProjectionOptions options;
    QDialog dialog(parent);
    dialog.setObjectName("projection3dDialog");
    dialog.setWindowTitle(MainWindow::tr("3D Projection"));
    auto *layout = new QFormLayout(&dialog);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setVerticalSpacing(10);
    auto *method = new QComboBox(&dialog);
    method->setObjectName("projectionMethod");
    method->setFixedWidth(165);
    method->addItems({MainWindow::tr("Nearest Point"), MainWindow::tr("Brightest Point"), MainWindow::tr("Mean Value")});
    method->setCurrentIndex(std::clamp(settings.value("method", 1).toInt(), 0, 2));
    layout->addRow(MainWindow::tr("Projection method:"), method);
    auto *axis = new QComboBox(&dialog);
    axis->setObjectName("projectionAxis");
    axis->setFixedWidth(165);
    axis->addItems({MainWindow::tr("X-Axis"), MainWindow::tr("Y-Axis"), MainWindow::tr("Z-Axis")});
    axis->setCurrentIndex(std::clamp(settings.value("axis", 1).toInt(), 0, 2));
    layout->addRow(MainWindow::tr("Axis of rotation:"), axis);
    QMap<QString, QDoubleSpinBox *> numbers;
    const auto number = [&](const QString &key, const QString &label, double value, double low, double high, int decimals = 0) {
        auto *spin = new QDoubleSpinBox(&dialog);
        spin->setObjectName(key);
        spin->setDecimals(decimals);
        spin->setRange(low, high);
        spin->setFixedWidth(165);
        spin->findChild<QLineEdit *>()->setTextMargins(6, 0, 0, 0);
        spin->setValue(settings.value(key, value).toDouble());
        layout->addRow(label, spin);
        numbers.insert(key, spin);
        return spin;
    };
    const auto &axes = input.metadata().axes;
    auto *spacing = number("sliceSpacing", MainWindow::tr("Slice spacing (pixels):"), axes.size() > 2 ? axes[2].spacing / axes[0].spacing : 1, .000001, 1e9, 6);
    auto *initial = number("initialAngle", MainWindow::tr("Initial angle (0-359 degrees):"), 0, 0, 359);
    auto *total = number("totalRotation", MainWindow::tr("Total rotation (0-360 degrees):"), 360, 0, 360);
    auto *increment = number("angleIncrement", MainWindow::tr("Rotation angle increment:"), 10, 1, 360);
    const auto range = std::make_shared<std::array<double, 2>>(std::array{
        std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()});
    auto *lower = new QLineEdit(&dialog);
    lower->setObjectName("lower");
    auto *upper = new QLineEdit(&dialog);
    upper->setObjectName("upper");
    for (auto *bound : {lower, upper}) { bound->setFixedWidth(165); bound->setEnabled(false); }
    auto *lowerSlider = new QSlider(Qt::Horizontal, &dialog);
    lowerSlider->setObjectName("projectionLowerSlider");
    auto *upperSlider = new QSlider(Qt::Horizontal, &dialog);
    upperSlider->setObjectName("projectionUpperSlider");
    for (const auto &[label, slider, bound] : {
             std::tuple{MainWindow::tr("Lower transparency bound:"), lowerSlider, lower},
             std::tuple{MainWindow::tr("Upper transparency bound:"), upperSlider, upper}}) {
        slider->setAccessibleName(label);
        slider->setRange(0, 1000);
        slider->setFixedWidth(165);
        slider->setEnabled(false);
        layout->addRow(label, bound);
        layout->addRow(QString{}, slider);
        QObject::connect(slider, &QSlider::valueChanged, &dialog, [range, bound](int value) {
            bound->setText(QString::number(std::lerp((*range)[0], (*range)[1], value / 1000.),
                'g', std::numeric_limits<double>::max_digits10));
        });
    }
    lower->setToolTip(MainWindow::tr("Inclusive bounds in raw pixel values. Values outside this range are transparent."));
    upper->setToolTip(lower->toolTip());
    auto *opacity = number("opacity", MainWindow::tr("Opacity (0-100%):"), 0, 0, 100);
    auto *surface = number("surfaceDepthCue", MainWindow::tr("Surface depth-cueing (0-100%):"), 100, 0, 100);
    auto *interior = number("interiorDepthCue", MainWindow::tr("Interior depth-cueing (0-100%):"), 50, 0, 100);
    auto *interpolate = new QCheckBox(MainWindow::tr("Interpolate"), &dialog);
    interpolate->setObjectName("projectionInterpolate");
    interpolate->setChecked(settings.value("interpolate", false).toBool());
    layout->addRow(interpolate);
    auto *size = new QLabel(&dialog);
    size->setObjectName("projectionOutputSize");
    size->setWordWrap(true);
    layout->addRow(size);
    size->setText(MainWindow::tr("Reading image intensity range..."));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Help, &dialog);
    for (auto *button : buttons->buttons()) button->setIcon(QIcon{});
    layout->addRow(buttons);
    const auto update = [&] {
        if (!lower->isEnabled()) {
            buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
            return;
        }
        options.method = static_cast<ProjectionMethod>(method->currentIndex());
        options.rotationAxis = axis->currentIndex();
        options.sliceSpacing = spacing->value();
        options.initialAngle = int(initial->value());
        options.totalRotation = int(total->value());
        options.angleIncrement = int(increment->value());
        bool lowerValid = false, upperValid = false;
        options.lower = lower->text().toDouble(&lowerValid);
        options.upper = upper->text().toDouble(&upperValid);
        for (const auto &[slider, value, valid] : {
                 std::tuple{lowerSlider, options.lower, lowerValid}, std::tuple{upperSlider, options.upper, upperValid}}) {
            const QSignalBlocker blocker(slider);
            const bool hasRange = (*range)[1] > (*range)[0];
            slider->setEnabled(hasRange);
            if (!valid || !std::isfinite(value)) continue;
            const double clamped = std::clamp(value, (*range)[0], (*range)[1]);
            const double span = (*range)[1] - (*range)[0];
            const double fraction = !hasRange ? 0 : std::isfinite(span) ? (clamped - (*range)[0]) / span
                : (clamped / 2 - (*range)[0] / 2) / ((*range)[1] / 2 - (*range)[0] / 2);
            slider->setValue(int(std::lround(fraction * 1000)));
        }
        options.opacity = int(opacity->value());
        options.surfaceDepthCue = int(surface->value());
        options.interiorDepthCue = int(interior->value());
        options.interpolate = interpolate->isChecked();
        try {
            if (!lowerValid || !upperValid)
                throw std::invalid_argument(MainWindow::tr("Enter numeric transparency bounds").toStdString());
            const auto geometry = projectionGeometry(input, options);
            detail::checkMemoryBudget(geometry.requiredMemoryBytes, detail::availableMemoryBytes());
            size->setText(MainWindow::tr("Output: %1 × %2 × %3 angles, %4 MiB").arg(geometry.shape[0]).arg(geometry.shape[1])
                .arg(geometry.shape[2]).arg(double(geometry.byteCount) / (1024 * 1024), 0, 'f', 2));
            buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
        } catch (const std::exception &error) {
            size->setText(QString::fromUtf8(error.what()));
            buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
        }
    };
    QObject::connect(lower, &QLineEdit::textChanged, &dialog, update);
    QObject::connect(upper, &QLineEdit::textChanged, &dialog, update);
    for (auto *spin : numbers) QObject::connect(spin, &QDoubleSpinBox::valueChanged, &dialog, update);
    QObject::connect(method, &QComboBox::currentIndexChanged, &dialog, update);
    QObject::connect(axis, &QComboBox::currentIndexChanged, &dialog, update);
    QObject::connect(interpolate, &QCheckBox::toggled, &dialog, update);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::helpRequested, &dialog, [&dialog] {
        QMessageBox::information(&dialog, MainWindow::tr("3D Project help"),
            MainWindow::tr("Projects the full Z stack at each rotation angle. A full turn omits the duplicate 360-degree view.\n\n"
                   "Nearest Point shows the front surface; Brightest Point selects the highest raw value; Mean Value averages visible samples. "
                   "Opacity blends the front surface with the selected projection. Depth cueing darkens more distant points. "
                   "Interpolate uses linear sampling between voxels; otherwise sampling uses the nearest voxel.\n\n"
                   "Bounds start at the image's minimum and maximum finite raw values and are reset each time this dialog opens. "
                   "Output keeps the pixel type, channels and time points, with pixel coordinates and an angle axis. "
                   "The result opens in a new image window."));
    });
    update();
    std::stop_source rangeCancellation;
    QObject::connect(&dialog, &QDialog::finished, &dialog, [rangeCancellation]() mutable { rangeCancellation.request_stop(); });
    jobs.submit(MainWindow::tr("Reading projection intensity range"), [input, range, rangeCancellation](auto stop, auto progress) {
        const auto checkStop = [&] {
            if (stop.stop_requested() || rangeCancellation.stop_requested())
                throw std::runtime_error(MainWindow::tr("Intensity range calculation cancelled").toStdString());
        };
        double lo = std::numeric_limits<double>::infinity(), hi = -lo;
        const auto components = componentCount(input.pixelType());
        if (const auto *base = input.data()) {
            const auto scanTyped = [&]<typename T>() {
                if (input.contiguous()) {
                    const std::size_t total = input.pixelCount() * components;
                    constexpr std::size_t chunk = 65536;
                    if constexpr (std::is_integral_v<T>) {
                        T minVal = std::numeric_limits<T>::max(), maxVal = std::numeric_limits<T>::lowest();
                        for (std::size_t offset = 0; offset < total; offset += chunk) {
                            checkStop();
                            const std::size_t end = std::min(total, offset + chunk);
                            for (std::size_t i = offset; i < end; ++i) {
                                T value;
                                std::memcpy(&value, base + i * sizeof(T), sizeof(T));
                                if (value < minVal) minVal = value;
                                if (value > maxVal) maxVal = value;
                            }
                            progress(double(end) / double(total));
                        }
                        if (total > 0) { lo = double(minVal); hi = double(maxVal); }
                    } else {
                        for (std::size_t offset = 0; offset < total; offset += chunk) {
                            checkStop();
                            const std::size_t end = std::min(total, offset + chunk);
                            for (std::size_t i = offset; i < end; ++i) {
                                T value;
                                std::memcpy(&value, base + i * sizeof(T), sizeof(T));
                                if (std::isfinite(value)) {
                                    if (double(value) < lo) lo = double(value);
                                    if (double(value) > hi) hi = double(value);
                                }
                            }
                            progress(double(end) / double(total));
                        }
                    }
                } else {
                    const auto &strides = input.strides();
                    const std::size_t sX = strides[0], sY = strides[1];
                    const std::size_t sZ = strides.size() > 2 ? strides[2] : 0;
                    const std::size_t sC = strides.size() > 3 ? strides[3] : 0;
                    const std::size_t sT = strides.size() > 4 ? strides[4] : 0;
                    const auto totalRows = input.height() * input.depth() * input.channels() * input.frames();
                    const auto step = std::max<std::size_t>(1, totalRows / 100);
                    std::size_t rows = 0;
                    for (std::size_t t = 0; t < input.frames(); ++t) for (std::size_t c = 0; c < input.channels(); ++c)
                        for (std::size_t z = 0; z < input.depth(); ++z) for (std::size_t y = 0; y < input.height(); ++y) {
                            checkStop();
                            const auto *row = base + t * sT + c * sC + z * sZ + y * sY;
                            for (std::size_t x = 0; x < input.width(); ++x)
                                for (std::size_t k = 0; k < components; ++k) {
                                    T value;
                                    std::memcpy(&value, row + x * sX + k * sizeof(T), sizeof(T));
                                    if constexpr (std::is_integral_v<T>) {
                                        if (double(value) < lo) lo = double(value);
                                        if (double(value) > hi) hi = double(value);
                                    } else if (std::isfinite(value)) {
                                        if (double(value) < lo) lo = double(value);
                                        if (double(value) > hi) hi = double(value);
                                    }
                                }
                            if (++rows % step == 0 || rows == totalRows) progress(double(rows) / double(totalRows));
                        }
                }
            };
            switch (input.pixelType()) {
            case PixelType::UInt8: case PixelType::RGB8: case PixelType::RGBA8:
                scanTyped.template operator()<std::uint8_t>(); break;
            case PixelType::UInt16: scanTyped.template operator()<std::uint16_t>(); break;
            case PixelType::Int16: scanTyped.template operator()<std::int16_t>(); break;
            case PixelType::Float32: scanTyped.template operator()<float>(); break;
            case PixelType::Float64: scanTyped.template operator()<double>(); break;
            }
        } else {
            std::size_t rows = 0;
            const auto totalRows = input.height() * input.depth() * input.channels() * input.frames();
            const auto step = std::max<std::size_t>(1, totalRows / 100);
            for (std::size_t t = 0; t < input.frames(); ++t) for (std::size_t c = 0; c < input.channels(); ++c)
                for (std::size_t z = 0; z < input.depth(); ++z) for (std::size_t y = 0; y < input.height(); ++y) {
                    checkStop();
                    for (std::size_t x = 0; x < input.width(); ++x)
                        for (std::size_t k = 0; k < components; ++k) {
                            const double value = input.sample(x, y, z, c, t, k);
                            if (!std::isfinite(value)) continue;
                            lo = std::min(lo, value);
                            hi = std::max(hi, value);
                        }
                    if (++rows % step == 0 || rows == totalRows) progress(double(rows) / double(totalRows));
                }
        }
        (*range)[0] = lo;
        (*range)[1] = hi;
        if (!std::isfinite((*range)[0]))
            throw std::invalid_argument(MainWindow::tr("The image has no finite intensity values").toStdString());
    }, [guard = QPointer<QDialog>(&dialog), range, lower, upper, size, update](auto error) {
        if (!guard) return;
        if (error) {
            try { std::rethrow_exception(error); }
            catch (const std::exception &exception) { size->setText(QString::fromUtf8(exception.what())); }
            return;
        }
        const QSignalBlocker blockLower(lower), blockUpper(upper);
        lower->setText(QString::number((*range)[0], 'g', std::numeric_limits<double>::max_digits10));
        upper->setText(QString::number((*range)[1], 'g', std::numeric_limits<double>::max_digits10));
        lower->setEnabled(true);
        upper->setEnabled(true);
        update();
    });
    if (dialog.exec() != QDialog::Accepted) return {};
    for (auto it = numbers.cbegin(); it != numbers.cend(); ++it) settings.setValue(it.key(), it.value()->value());
    settings.setValue("method", method->currentIndex());
    settings.setValue("axis", axis->currentIndex());
    settings.setValue("interpolate", options.interpolate);
    settings.sync();
    return options;
}

std::optional<RawImportOptions> showRawImportDialog(QWidget *parent, const QString &path)
{
    QSettings settings;
    settings.beginGroup("rawImport/v1");
    QDialog dialog(parent);
    dialog.setObjectName("rawImportDialog");
    dialog.setWindowTitle(MainWindow::tr("Import raw: %1").arg(QFileInfo(path).fileName()));
    auto *layout = new QFormLayout(&dialog);
    auto *type = new QComboBox(&dialog);
    type->setObjectName("rawPixelType");
    const std::array types{PixelType::UInt8, PixelType::Int16, PixelType::UInt16,
                           PixelType::Float32, PixelType::Float64, PixelType::RGB8, PixelType::RGBA8};
    const QStringList labels{MainWindow::tr("8-bit"), MainWindow::tr("16-bit Signed"), MainWindow::tr("16-bit Unsigned"),
                             MainWindow::tr("32-bit Real"), MainWindow::tr("64-bit Real"), MainWindow::tr("24-bit RGB"), MainWindow::tr("32-bit RGBA")};
    for (std::size_t i = 0; i < types.size(); ++i) type->addItem(labels[int(i)], pixelTypeName(types[i]));
    type->setCurrentIndex(2);
    layout->addRow(MainWindow::tr("Image type"), type);
    const QStringList dimensionNames{"rawWidth", "rawHeight", "rawImages"};
    const QStringList dimensionLabels{MainWindow::tr("Width"), MainWindow::tr("Height"), MainWindow::tr("Number of images")};
    std::array<QSpinBox *, 3> dimensions;
    for (std::size_t i = 0; i < dimensions.size(); ++i) {
        auto *spin = new QSpinBox(&dialog);
        spin->setObjectName(dimensionNames[int(i)]);
        spin->setRange(1, std::numeric_limits<int>::max());
        spin->setValue(i < 2 ? 512 : 1);
        dimensions[i] = spin;
        layout->addRow(dimensionLabels[int(i)], spin);
    }
    dimensions[2]->setToolTip(MainWindow::tr("Maximum number of images to import. Stops at the last complete image."));
    auto *offset = new QLineEdit("0", &dialog);
    offset->setObjectName("rawOffset");
    layout->addRow(MainWindow::tr("Offset to first image, bytes"), offset);
    auto *endian = new QComboBox(&dialog);
    endian->setObjectName("rawByteOrder");
    endian->addItem(MainWindow::tr("Native"), "native");
    endian->addItem(MainWindow::tr("Little endian"), "little");
    endian->addItem(MainWindow::tr("Big endian"), "big");
    layout->addRow(MainWindow::tr("Byte order"), endian);

    auto *advanced = new QToolButton(&dialog);
    advanced->setObjectName("rawAdvanced");
    advanced->setText(MainWindow::tr("Advanced"));
    advanced->setCheckable(true);
    advanced->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    advanced->setArrowType(Qt::RightArrow);
    layout->addRow(advanced);
    auto *advancedPanel = new QWidget(&dialog);
    auto *advancedLayout = new QFormLayout(advancedPanel);
    advancedLayout->setContentsMargins(0, 0, 0, 0);
    auto *channels = new QSpinBox(advancedPanel);
    channels->setObjectName("rawChannels");
    channels->setRange(1, std::numeric_limits<int>::max());
    auto *frames = new QSpinBox(advancedPanel);
    frames->setObjectName("rawFrames");
    frames->setRange(1, std::numeric_limits<int>::max());
    auto *spacing = new QLineEdit("1,1,1", advancedPanel);
    spacing->setObjectName("rawSpacing");
    auto *unit = new QLineEdit("px", advancedPanel);
    unit->setObjectName("rawUnit");
    auto *mapped = new QCheckBox(MainWindow::tr("Use memory mapping when possible"), advancedPanel);
    mapped->setObjectName("rawMemoryMap");
    mapped->setChecked(true);
    advancedLayout->addRow(MainWindow::tr("Channels"), channels);
    advancedLayout->addRow(MainWindow::tr("Time frames"), frames);
    advancedLayout->addRow(MainWindow::tr("Spacing X,Y,Z"), spacing);
    advancedLayout->addRow(MainWindow::tr("Spatial unit"), unit);
    advancedLayout->addRow(mapped);
    layout->addRow(advancedPanel);
    advancedPanel->hide();
    QObject::connect(advanced, &QToolButton::toggled, &dialog, [advanced, advancedPanel, &dialog](bool expanded) {
        advanced->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        advancedPanel->setVisible(expanded);
        dialog.adjustSize();
    });

    const QMap<QString, QSpinBox *> integerOptions{{"width", dimensions[0]}, {"height", dimensions[1]},
        {"maximumImages", dimensions[2]}, {"channels", channels}, {"frames", frames}};
    const QMap<QString, QLineEdit *> textOptions{{"headerBytes", offset}, {"spacing", spacing}, {"unit", unit}};
    for (auto it = integerOptions.cbegin(); it != integerOptions.cend(); ++it)
        it.value()->setValue(settings.value(it.key(), it.value()->value()).toInt());
    for (auto it = textOptions.cbegin(); it != textOptions.cend(); ++it)
        it.value()->setText(settings.value(it.key(), it.value()->text()).toString());
    const auto savedType = type->findData(settings.value("pixelType", type->currentData()));
    if (savedType >= 0) type->setCurrentIndex(savedType);
    const auto savedOrder = endian->findData(settings.value("byteOrder", endian->currentData()));
    if (savedOrder >= 0) endian->setCurrentIndex(savedOrder);
    mapped->setChecked(settings.value("memoryMap", true).toBool());
    advanced->setChecked(settings.value("advanced", false).toBool());

    auto *error = new QLabel(&dialog);
    error->setObjectName("rawImportError");
    error->setWordWrap(true);
    error->setStyleSheet("color: #B3261E;");
    layout->addRow(error);
    error->hide();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    RawImportOptions options;
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        try {
            bool ok;
            const auto header = offset->text().trimmed().toULongLong(&ok);
            if (!ok || header > static_cast<quint64>(std::numeric_limits<qint64>::max()))
                throw std::invalid_argument(MainWindow::tr("Offset must be a non-negative whole number of bytes.").toStdString());
            QJsonArray shape{dimensions[0]->value(), dimensions[1]->value()};
            if (dimensions[2]->value() > 1 || channels->value() > 1 || frames->value() > 1)
                shape.append(1);
            if (channels->value() > 1 || frames->value() > 1) shape.append(channels->value());
            if (frames->value() > 1) shape.append(frames->value());
            options = rawImportOptionsFromJson({{"pixelType", type->currentData().toString()},
                {"shape", shape}, {"headerBytes", static_cast<qint64>(header)},
                {"byteOrder", endian->currentData().toString()}, {"memoryMap", mapped->isChecked()}});
            std::size_t bytes = bytesPerPixel(options.pixelType);
            for (const auto size : options.shape) bytes *= size;
            const auto fileBytes = static_cast<quint64>(QFileInfo(path).size());
            if (header > fileBytes || bytes > fileBytes - header)
                throw std::invalid_argument(MainWindow::tr("The file is too small for these dimensions, image type and offset.").toStdString());
            const auto imageCount = std::min<quint64>(dimensions[2]->value(), (fileBytes - header) / bytes);
            if (options.shape.size() > 2) options.shape[2] = static_cast<std::size_t>(imageCount);
            const QStringList axisNames{"X", "Y", "Z", "C", "T"};
            const auto spacings = spacing->text().split(',');
            for (std::size_t i = 0; i < options.shape.size(); ++i) {
                ok = true;
                const double value = i < 3 && i < std::size_t(spacings.size()) ? spacings[int(i)].toDouble(&ok) : 1;
                if (!ok || !std::isfinite(value) || value <= 0)
                    throw std::invalid_argument(MainWindow::tr("Spacing must be positive.").toStdString());
                options.metadata.axes.push_back({axisNames[int(i)], value, 0, i < 3 ? unit->text() : "index"});
            }
            dialog.accept();
        } catch (const std::exception &e) {
            error->setText(QString::fromUtf8(e.what()));
            error->show();
            dialog.adjustSize();
        }
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return {};
    for (auto it = integerOptions.cbegin(); it != integerOptions.cend(); ++it)
        settings.setValue(it.key(), it.value()->value());
    for (auto it = textOptions.cbegin(); it != textOptions.cend(); ++it)
        settings.setValue(it.key(), it.value()->text());
    settings.setValue("pixelType", type->currentData());
    settings.setValue("byteOrder", endian->currentData());
    settings.setValue("memoryMap", mapped->isChecked());
    settings.setValue("advanced", advanced->isChecked());
    settings.sync();
    return options;
}

void showSelectionPropertiesDialog(QWidget *parent, const Roi &roi, const std::function<void(const Roi &)> &apply)
{
    QDialog dialog(parent);
    dialog.setObjectName("selectionPropertiesDialog");
    dialog.setWindowTitle(MainWindow::tr("Selection properties"));
    dialog.setMinimumWidth(380);
    auto *layout = new QFormLayout(&dialog);
    auto *name = new QLineEdit(roi.name, &dialog);
    name->setObjectName("roiName");
    layout->addRow(MainWindow::tr("Name"), name);
    QColor outline = QColor::fromRgba(roi.color);
    auto *color = new QPushButton(&dialog);
    color->setObjectName("roiColor");
    const auto updateColor = [&] {
        QPixmap swatch(20, 20);
        swatch.fill(outline);
        color->setIcon(QIcon(swatch));
        color->setText(outline.name(QColor::HexArgb));
    };
    updateColor();
    layout->addRow(MainWindow::tr("Outline color"), color);
    QObject::connect(color, &QPushButton::clicked, &dialog, [&] {
        const auto chosen = QColorDialog::getColor(outline, &dialog, MainWindow::tr("ROI outline color"), QColorDialog::ShowAlphaChannel);
        if (chosen.isValid()) { outline = chosen; updateColor(); }
    });
    auto *visible = new QCheckBox(&dialog);
    visible->setObjectName("roiVisible");
    visible->setChecked(roi.visible);
    layout->addRow(MainWindow::tr("Visible"), visible);

    const bool line = roi.type == RoiType::Line;
    const bool area = roi.type == RoiType::Rectangle || roi.type == RoiType::Ellipse;
    QList<QDoubleSpinBox *> geometry;
    bool geometryChanged = false;
    if (line || area) {
        const auto bounds = roi.bounds();
        const QStringList labels = line ? QStringList{MainWindow::tr("X1"), MainWindow::tr("Y1"), MainWindow::tr("X2"), MainWindow::tr("Y2")}
                                        : QStringList{MainWindow::tr("X"), MainWindow::tr("Y"), MainWindow::tr("Width"), MainWindow::tr("Height")};
        const QStringList ids = line ? QStringList{"roiX1", "roiY1", "roiX2", "roiY2"}
                                     : QStringList{"roiX", "roiY", "roiWidth", "roiHeight"};
        const std::array<double, 4> values = line
            ? std::array{roi.points[0].x(), roi.points[0].y(), roi.points[1].x(), roi.points[1].y()}
            : std::array{bounds.x(), bounds.y(), bounds.width(), bounds.height()};
        for (int i = 0; i < 4; ++i) {
            auto *spin = new QDoubleSpinBox(&dialog);
            spin->setObjectName(ids[i]);
            spin->setDecimals(6);
            spin->findChild<QLineEdit *>()->setTextMargins(6, 0, 0, 0);
            spin->setRange(area && i >= 2 ? 0.5 : std::numeric_limits<double>::lowest(),
                           std::numeric_limits<double>::max());
            spin->setSuffix(MainWindow::tr(" px"));
            spin->setValue(values[i]);
            layout->addRow(labels[i], spin);
            geometry.append(spin);
        }
    }
    auto *error = new QLabel(&dialog);
    error->setObjectName("roiGeometryError");
    error->setWordWrap(true);
    layout->addRow(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    const auto editedRoi = [&] {
        auto edited = roi;
        edited.name = name->text();
        edited.color = outline.rgba();
        edited.visible = visible->isChecked();
        // Appearance edits must not round existing subpixel coordinates.
        if (geometryChanged) {
            const QPointF first(geometry[0]->value(), geometry[1]->value());
            const QPointF second(geometry[2]->value(), geometry[3]->value());
            edited.points = {first, line ? second : first + second};
        }
        return edited;
    };
    const auto validate = [&] {
        const auto edited = editedRoi();
        bool valid = true;
        if (geometryChanged) {
            for (const auto &point : edited.points) valid &= std::isfinite(point.x()) && std::isfinite(point.y());
            if (line) valid &= std::hypot(edited.points[1].x() - edited.points[0].x(),
                                          edited.points[1].y() - edited.points[0].y()) >= 0.5;
        }
        error->setText(valid ? QString{} : line ? MainWindow::tr("The line must be at least 0.5 pixels long and have finite coordinates.")
                                                : MainWindow::tr("The selection must have finite coordinates."));
        error->setVisible(!valid);
        buttons->button(QDialogButtonBox::Ok)->setEnabled(valid);
        buttons->button(QDialogButtonBox::Apply)->setEnabled(valid);
    };
    for (auto *spin : geometry) QObject::connect(spin, &QDoubleSpinBox::valueChanged, &dialog, [&] {
        geometryChanged = true;
        validate();
    });
    validate();
    QObject::connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog, [&] { apply(editedRoi()); });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] { apply(editedRoi()); dialog.accept(); });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}

} // namespace imagecxx
