#include "mainwindow.h"
#include "app/batch.h"
#include "app/processing_commands.h"
#include "core/memory.h"
#include "io/image_io.h"
#include "processing/processing.h"
#include "ui/clipboard.h"
#include "ui/dialogs.h"
#include "ui/overlay_window.h"
#include "ui/theme.h"
#include <QAbstractSpinBox>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QCompleter>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QIcon>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLockFile>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPrintDialog>
#include <QPrintPreviewDialog>
#include <QPrinter>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QSet>
#include <QSettings>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTextEdit>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

using namespace imagecxx;

namespace {
Image cropToBounds(const Image &image, const QRectF &bounds)
{
    const auto clipped = bounds.normalized().toAlignedRect().intersected(QRect(0, 0, int(image.width()), int(image.height())));
    if (clipped.isEmpty()) return {};
    std::vector<std::size_t> start(image.shape().size(), 0), extent = image.shape();
    start[0] = std::size_t(clipped.x());
    start[1] = std::size_t(clipped.y());
    extent[0] = std::size_t(clipped.width());
    extent[1] = std::size_t(clipped.height());
    return image.region(start, extent);
}
} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), jobs_(nullptr, 2)
{
    setObjectName("ImageCXXMainWindow");
    setWindowTitle(tr("ImageCXX"));
    resize(600, 90);
    setMinimumWidth(560);
    setMaximumHeight(120);
    setAcceptDrops(true);
    setStyleSheet(mainWindowStyleSheet());

    pixelStatus_ = new QLabel(tr("Ready"), this);
    jobStatus_ = new QLabel(this);
    jobStatus_->setMaximumWidth(155);
    progress_ = new QProgressBar(this);
    progress_->setFixedWidth(70);
    progress_->setRange(0, 1000);
    progress_->setTextVisible(false);
    progress_->hide();
    auto *cancelButton = new QPushButton(tr("Cancel"), this);
    cancelButton->setVisible(false);
    statusBar()->setSizeGripEnabled(false);
    statusBar()->addWidget(pixelStatus_, 1);
    statusBar()->addPermanentWidget(jobStatus_);
    statusBar()->addPermanentWidget(progress_);
    statusBar()->addPermanentWidget(cancelButton);
    connect(cancelButton, &QPushButton::clicked, &jobs_, &JobScheduler::cancelAll);
    pixelStatus_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    jobStatus_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    connect(&jobs_, &JobScheduler::started, this, [this](quint64, const QString &title) {
        jobStatus_->setText(title);
        jobStatus_->setToolTip(title);
        progress_->setValue(0);
    });
    connect(&jobs_, &JobScheduler::progress, this, [this](quint64, double value) { progress_->setValue(qRound(value * 1000)); });
    connect(&jobs_, &JobScheduler::busyChanged, this, [this, cancelButton](bool busy) {
        progress_->setVisible(busy);
        cancelButton->setVisible(busy);
        if (!busy) jobStatus_->clear();
    });

    commands_.add({"file.open", tr("Open images..."), QT_TRANSLATE_NOOP("CommandCategories", "File"), QKeySequence::Open, false, {}, [this](const QVariantMap &) {
        openPaths(QFileDialog::getOpenFileNames(this, tr("Open images"), {}, imageFileFilter()));
    }});
    commands_.add({"file.folder", tr("Open folder..."), QT_TRANSLATE_NOOP("CommandCategories", "File"), {}, false, {}, [this](const QVariantMap &) {
        const auto path = QFileDialog::getExistingDirectory(this, tr("Open image folder"));
        if (!path.isEmpty()) openPaths({path});
    }});
    commands_.add({"file.raw", tr("Import raw binary..."), QT_TRANSLATE_NOOP("CommandCategories", "File"), {}, false, {}, [this](const QVariantMap &) {
        const auto path = QFileDialog::getOpenFileName(this, tr("Import raw binary"));
        if (path.isEmpty()) return;
        importRaw(path);
    }});
    commands_.add({"file.dicomSeries", tr("Open DICOM series..."), QT_TRANSLATE_NOOP("CommandCategories", "File"), {}, false, {}, [this](const QVariantMap &) {
        openPaths(QFileDialog::getOpenFileNames(this, tr("Select DICOM files"), {}, tr("DICOM (*.dcm *.dicom);;All files (*)")));
    }});
    commands_.add({"file.save", tr("Save image as..."), QT_TRANSLATE_NOOP("CommandCategories", "File"), QKeySequence::Save, true, {}, [this](const QVariantMap &) {
        const QPointer<DocumentWidget> document = activeDocument();
        if (!document) return;
        QFileDialog dialog(this, tr("Save image"), tr("Exported image.tif"), imageSaveFilter());
        dialog.setAcceptMode(QFileDialog::AcceptSave);
        dialog.setOption(QFileDialog::DontUseNativeDialog);
        QCheckBox includeMetadata(tr("Include full metadata"), &dialog);
        includeMetadata.setObjectName("includeFullMetadata");
        dialog.layout()->addWidget(&includeMetadata);
        if (dialog.exec() != QDialog::Accepted) return;
        const auto path = dialog.selectedFiles().value(0);
        if (path.isEmpty() || !document) return;
        const QFileInfo destination(path);
        QString key = destination.canonicalFilePath();
        if (key.isEmpty()) key = QDir(destination.dir().canonicalPath()).filePath(destination.fileName());
        const QFileInfo reservedFile(key);
        const auto name = reservedFile.fileName().normalized(QString::NormalizationForm_C);
        const auto lockName = name.left(32) + "." + QString::number(qHash(name.toCaseFolded()), 16) + ".imagecxx-ui-save.lock";
        const auto reservation = std::make_shared<QLockFile>(reservedFile.dir().filePath(lockName));
        reservation->setStaleLockTime(0); // A large save can take longer than Qt's default 30 seconds.
        if (!reservation->tryLock(0)) {
            showError(reservation->error() == QLockFile::LockFailedError
                ? tr("This file is still being saved. Wait for that save to finish before saving it again.")
                : tr("Cannot save in this folder. Check its permissions and free space."));
            return;
        }
        const Image image = document->image;
        const QPointer<DocumentWidget> target = document;
        const auto revision = document->revision;
        const bool includeFullMetadata = includeMetadata.isChecked();
        jobs_.submit(tr("Saving image"), [image, path, includeFullMetadata](auto stop, auto progress) {
            writeImage(image, path, stop, progress, includeFullMetadata);
        }, [this, target, revision, path, reservation, includeFullMetadata](auto error) {
            reservation->unlock();
            if (showJobError(error)) return;
            if (includeFullMetadata && target && target->revision == revision) {
                target->modified = false;
                target->image.metadata().sourcePath = QFileInfo(path).absoluteFilePath();
                target->image.metadata().name = QFileInfo(path).fileName();
                emit target->changed();
            }
            statusBar()->showMessage(tr("Saved %1").arg(path), 5000);
        });
    }});
    commands_.add({"edit.undo", tr("Undo processing"), QT_TRANSLATE_NOOP("CommandCategories", "Edit"), QKeySequence::Undo, true, {}, [this](const QVariantMap &) {
        if (auto *doc = activeDocument()) if (auto image = doc->history.undo(doc->image)) {
            doc->image = std::move(*image);
            doc->modified = true;
            ++doc->revision;
            doc->refresh();
            updatePanels();
        }
    }});
    commands_.add({"edit.redo", tr("Redo processing"), QT_TRANSLATE_NOOP("CommandCategories", "Edit"), QKeySequence::Redo, true, {}, [this](const QVariantMap &) {
        if (auto *doc = activeDocument()) if (auto image = doc->history.redo(doc->image)) {
            doc->image = std::move(*image);
            doc->modified = true;
            ++doc->revision;
            doc->refresh();
            updatePanels();
        }
    }});
    registerProcessingCommands(commands_, [this](const QString &id, const QVariantMap &parameters) {
        auto *document = activeDocument();
        if (!document) return;
        const auto scope = parameters.value("scope").toString();
        Image input = document->image;
        const auto z = document->viewer->slice(), c = document->viewer->channel(), t = document->viewer->time();
        if (scope != "Full image") input = input.slice(z, c, t);
        if (scope == "ROI bounds") {
            const auto roi = selectedRoi();
            if (!roi || roi->type == RoiType::Line) { showError(tr("Select an area ROI first.")); return; }
            input = cropToBounds(input, roi->bounds());
            if (input.empty()) { showError(tr("The ROI is outside this image.")); return; }
        }
        auto options = processingOptions(id, parameters);
        if (options.outputSpacing.size() > 2 && input.depth() == 1) options.outputSpacing.resize(2);
        const auto revision = document->revision;
        const QPointer<DocumentWidget> target = document;
        const auto label = operationName(options.operation);
        const bool newDocument = options.operation == Operation::Resample
            || input.byteCount() > document->history.byteBudget();
        auto output = std::make_shared<Image>();
        jobs_.submit(label, [input, options, output](auto stop, auto progress) { *output = process(input, options, stop, progress); },
            [this, target, revision, scope, newDocument, label, output](auto error) {
                if (showJobError(error)) return;
                if (!target || target->revision != revision || scope != "Full image" || newDocument) {
                    output->metadata().name += " · " + label;
                    output->metadata().sourcePath.clear();
                    auto *doc = addDocument(*output);
                    doc->modified = true;
                } else {
                    target->replaceImage(*output, label);
                    target->image.metadata().sourcePath.clear();
                }
                updatePanels();
            });
    });
    commands_.add({"stacks.reslice", tr("Reslice..."), QT_TRANSLATE_NOOP("CommandCategories", "Stacks"), {}, true, {}, [this](const QVariantMap &) {
        const auto *document = activeDocument();
        if (!document) return;
        const Image input = document->image;
        const auto lut = document->viewer->lut();
        const bool inverted = document->viewer->isInverted();
        const auto options = showResliceDialog(this, input);
        if (!options) return;
        auto output = std::make_shared<Image>();
        jobs_.submit(tr("Reslice"), [input, options = *options, output](auto stop, auto progress) {
            *output = reslice(input, options, stop, progress);
        }, [this, output, lut, inverted](auto error) {
            if (showJobError(error)) return;
            addDocument(*output, lut, inverted)->modified = true;
            updatePanels();
        });
    }});
    commands_.add({"stacks.project3d", tr("3D Project..."), QT_TRANSLATE_NOOP("CommandCategories", "Stacks"), {}, true, {}, [this](const QVariantMap &) {
        const auto *document = activeDocument();
        if (!document) return;
        const Image input = document->image;
        const auto lut = document->viewer->lut();
        const bool inverted = document->viewer->isInverted();
        const auto options = showProjectionDialog(this, input, jobs_);
        if (!options) return;
        auto output = std::make_shared<Image>();
        jobs_.submit(tr("3D Project"), [input, options = *options, output](auto stop, auto progress) {
            *output = project3D(input, options, stop, progress);
        }, [this, output, lut, inverted](auto error) {
            if (showJobError(error)) return;
            addDocument(*output, lut, inverted)->modified = true;
            updatePanels();
        });
    }});

    registerAnalysisCommands();

    commands_.add({"process.batch", tr("Batch process files..."), QT_TRANSLATE_NOOP("CommandCategories", "Process"), {}, false, {}, [this](const QVariantMap &) {
        QStringList labels, ids;
        CommandRegistry processing;
        registerProcessingCommands(processing, [](const QString &, const QVariantMap &) {});
        for (const auto &command : processing.commands()) { labels << command.label; ids << command.id; }
        bool ok;
        const auto label = QInputDialog::getItem(this, tr("Batch processing"), tr("Operation"), labels, 0, false, &ok);
        if (!ok) return;
        const auto id = ids[labels.indexOf(label)];
        const auto &command = commands_.command(id);
        const auto parameters = parameterForm(command, true);
        if (!parameters) return;
        const auto inputs = QFileDialog::getOpenFileNames(this, tr("Select batch input files"), {}, imageFileFilter());
        if (inputs.isEmpty()) return;
        const auto directory = QFileDialog::getExistingDirectory(this, tr("Select output directory"));
        if (directory.isEmpty()) return;
        auto output = std::make_shared<BatchResult>();
        jobs_.submit(tr("Batch processing"), [inputs, directory, id, parameters, output](auto stop, auto progress) { *output = runBatch(inputs, directory, id, *parameters, stop, progress); },
            [this, output](auto error) {
                if (showJobError(error)) return;
                if (!output->errors.isEmpty()) showError(tr("Saved %1 outputs.\n\n%2").arg(output->outputs.size()).arg(output->errors.join('\n')));
                else statusBar()->showMessage(tr("Batch complete: %1 files saved.").arg(output->outputs.size()), 10000);
            });
    }});

    for (const bool cut : {false, true}) {
        commands_.add({cut ? "edit.cut" : "edit.copy", cut ? tr("Cut") : tr("Copy"), QT_TRANSLATE_NOOP("CommandCategories", "Edit"),
            QKeySequence(cut ? "X" : "C"), true, {}, [this, cut](const QVariantMap &) {
            const auto *doc = activeDocument();
            if (!doc) return;
            auto image = doc->image.slice(doc->viewer->slice(), doc->viewer->channel(), doc->viewer->time());
            if (const auto roi = selectedRoi()) {
                image = cropToBounds(image, roi->bounds());
                if (image.empty()) return;
            }
            copyImageToClipboard(this, image, doc->viewer->windowMinimum(), doc->viewer->windowMaximum(),
                doc->viewer->isInverted(), jobs_, [this](const QString &reason) {
                    statusBar()->showMessage(tr("Image copied internally. PNG export unavailable: %1").arg(reason), 15000);
                });
            if (cut) executeCommand("edit.clear");
        }});
    }
    commands_.add({"edit.paste", tr("Paste"), QT_TRANSLATE_NOOP("CommandCategories", "Edit"), QKeySequence("V"), false, {}, [this](const QVariantMap &) {
        pasteImage(false, false);
    }});
    commands_.add({"edit.pasteSpecial", tr("Paste special..."), QT_TRANSLATE_NOOP("CommandCategories", "Edit"), {}, false,
        {{"source", tr("Source"), ParameterKind::Choice, QT_TRANSLATE_NOOP("CommandChoices", "Automatic"), 0, 0, {QT_TRANSLATE_NOOP("CommandChoices", "Automatic"), QT_TRANSLATE_NOOP("CommandChoices", "System image")}},
         {"destination", tr("Destination"), ParameterKind::Choice, QT_TRANSLATE_NOOP("CommandChoices", "Current image"), 0, 0, {QT_TRANSLATE_NOOP("CommandChoices", "Current image"), QT_TRANSLATE_NOOP("CommandChoices", "New image")}}},
        [this](const QVariantMap &parameters) {
            pasteImage(parameters["source"].toString() == "System image", parameters["destination"].toString() == "New image");
    }});
    for (const bool capture : {false, true}) {
        commands_.add({capture ? "file.capture" : "file.clipboard", capture ? tr("Capture screen") : tr("New image from clipboard"),
            "File", QKeySequence(capture ? "Shift+G" : "Shift+V"), false, {}, [this, capture](const QVariantMap &) {
            if (!capture) { pasteImage(true, true); return; }
            auto image = imageFromRaster(screen()->grabWindow(0).toImage());
            if (image.empty()) return;
            image.metadata().name = tr("Screen");
            addDocument(std::move(image))->modified = true;
            updatePanels();
        }});
    }
    for (const bool preview : {false, true}) {
        commands_.add({preview ? "file.printPreview" : "file.print", preview ? tr("Print preview...") : tr("Print..."),
            QT_TRANSLATE_NOOP("CommandCategories", "File"), preview ? QKeySequence{} : QKeySequence("P"), true, {}, [this, preview](const QVariantMap &) {
            const auto *doc = activeDocument();
            if (!doc) return;
            const auto raster = doc->viewer->renderedImage();
            const auto &axes = doc->image.metadata().axes;
            const double aspect = axes[0].unit == axes[1].unit ? axes[1].spacing / axes[0].spacing : 1.;
            const auto paint = [raster, aspect](QPrinter *printer) {
                QPainter painter(printer);
                QSizeF size(raster.width(), raster.height() * aspect);
                size.scale(painter.viewport().size(), Qt::KeepAspectRatio);
                painter.drawImage(QRectF(QPointF(0, 0), size), raster);
            };
            QPrinter printer(QPrinter::HighResolution);
            if (preview) {
                QPrintPreviewDialog dialog(&printer, this);
                dialog.setWindowTitle(tr("Print preview"));
                connect(&dialog, &QPrintPreviewDialog::paintRequested, &dialog, paint);
                dialog.exec();
            } else {
                QPrintDialog dialog(&printer, this);
                if (dialog.exec() == QDialog::Accepted) paint(&printer);
            }
        }});
    }
    commands_.add({"file.text", tr("New text window"), QT_TRANSLATE_NOOP("CommandCategories", "File"), QKeySequence("Shift+N"), false, {}, [this](const QVariantMap &) {
        auto *dialog = new QDialog(this, Qt::Window);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(tr("Text"));
        dialog->resize(600, 420);
        auto *layout = new QVBoxLayout(dialog);
        layout->addWidget(new QPlainTextEdit(dialog));
        dialog->show();
    }});
    commands_.add({"file.next", tr("Open next image"), QT_TRANSLATE_NOOP("CommandCategories", "File"), QKeySequence("Shift+O"), true, {}, [this](const QVariantMap &) {
        const auto *doc = activeDocument();
        if (!doc || doc->image.metadata().sourcePath.isEmpty()) return;
        const QFileInfo source(doc->image.metadata().sourcePath);
        const auto paths = source.dir().entryInfoList({"*.tif", "*.tiff", "*.png", "*.jpg", "*.jpeg", "*.bmp", "*.gif",
            "*.dcm", "*.dicom", "*.mha", "*.mhd", "*.nii", "*.nii.gz", "*.nrrd"}, QDir::Files, QDir::Name | QDir::IgnoreCase);
        for (qsizetype i = 0; i < paths.size(); ++i) if (paths[i].absoluteFilePath() == source.absoluteFilePath()) {
            const bool previous = QApplication::keyboardModifiers().testFlag(Qt::AltModifier);
            const auto next = i + (previous ? -1 : 1);
            if (next >= 0 && next < paths.size()) openPaths({paths[next].absoluteFilePath()});
            break;
        }
    }});
    commands_.add({"file.revert", tr("Revert"), QT_TRANSLATE_NOOP("CommandCategories", "File"), QKeySequence("R"), true, {}, [this](const QVariantMap &) {
        auto *doc = activeDocument();
        if (!doc || doc->image.metadata().sourcePath.isEmpty()) return;
        if (doc->modified && !QApplication::keyboardModifiers().testFlag(Qt::AltModifier) && QMessageBox::question(doc, tr("Revert"), tr("Discard pixel changes and reload the saved image?")) != QMessageBox::Yes) return;
        const auto path = doc->image.metadata().sourcePath;
        const auto revision = doc->revision;
        const QPointer<DocumentWidget> target = doc;
        auto output = std::make_shared<Image>();
        jobs_.submit(tr("Reverting"), [path, output](auto stop, auto progress) { *output = readImage(path, {}, stop, progress); },
            [this, target, revision, output](auto error) {
                if (showJobError(error)) return;
                if (target && target->revision == revision) {
                    target->replaceImage(*output, tr("Revert"));
                    target->modified = false;
                    updatePanels();
                }
            });
    }});
    auto *animation = new QTimer(this);
    animation->setInterval(100);
    connect(animation, &QTimer::timeout, this, [this, animation] {
        auto *doc = activeDocument();
        if (!doc) { animation->stop(); return; }
        auto *viewer = doc->viewer;
        if (doc->image.frames() > 1) viewer->setSlice(viewer->slice(), viewer->channel(), (viewer->time() + 1) % doc->image.frames());
        else viewer->setSlice((viewer->slice() + 1) % doc->image.depth(), viewer->channel(), viewer->time());
    });
    commands_.add({"image.animate", tr("Start/stop animation"), QT_TRANSLATE_NOOP("CommandCategories", "View"), QKeySequence(Qt::Key_Backslash), true, {}, [animation](const QVariantMap &) {
        if (animation->isActive()) animation->stop(); else animation->start();
    }});
    commands_.add({"edit.color", tr("Drawing color..."), QT_TRANSLATE_NOOP("CommandCategories", "Edit"), {}, false, {}, [this](const QVariantMap &) {
        const auto color = QColorDialog::getColor(drawingColor_, this, tr("Drawing color"));
        if (!color.isValid()) return;
        drawingColor_ = color;
        QPixmap swatch(24, 24);
        swatch.fill(color);
        findChild<QAction *>("edit.color")->setIcon(QIcon(swatch));
    }});
    for (const auto &[operation, kind, label] : {
             std::tuple{QStringLiteral("clear"), PixelEdit::Clear, tr("Clear")},
             std::tuple{QStringLiteral("fill"), PixelEdit::Fill, tr("Fill")},
             std::tuple{QStringLiteral("draw"), PixelEdit::Draw, tr("Draw")},
             std::tuple{QStringLiteral("invert"), PixelEdit::Invert, tr("Invert pixels")},
             std::tuple{QStringLiteral("smooth"), PixelEdit::Smooth, tr("Smooth")}}) {
        commands_.add({"edit." + operation, label, "Edit", {}, true, {}, [this, operation, kind](const QVariantMap &) {
            auto *doc = activeDocument();
            if (!doc) return;
            const auto roi = selectedRoi();
            if (kind == PixelEdit::Draw && !roi) { showError(tr("Select an ROI first.")); return; }
            if (doc->image.shape().size() > 5) { showError(tr("Pixel editing supports up to five dimensions.")); return; }
            if (roi && roi->type == RoiType::Line && kind != PixelEdit::Draw) { showError(tr("Select an area ROI first.")); return; }
            const auto input = doc->image;
            const auto z = doc->viewer->slice(), c = doc->viewer->channel(), t = doc->viewer->time();
            const auto revision = doc->revision;
            const QPointer<DocumentWidget> target = doc;
            const auto color = drawingColor_.rgba();
            auto output = std::make_shared<Image>();
            jobs_.submit(operation, [input, output, roi, z, c, t, kind, color](auto stop, auto progress) {
                *output = editPixels(input, kind, roi, z, c, t, color, stop, progress);
            }, [this, target, revision, output, operation](auto error) {
                if (showJobError(error)) return;
                if (target && target->revision == revision) target->replaceImage(*output, operation);
                else { output->metadata().sourcePath.clear(); addDocument(*output)->modified = true; }
                updatePanels();
            });
        }});
    }
    commands_.add({"file.new", tr("New image..."), QT_TRANSLATE_NOOP("CommandCategories", "File"), QKeySequence("N"), false,
        {{"width", tr("Width"), ParameterKind::Integer, 512, 1, 16384, {}},
         {"height", tr("Height"), ParameterKind::Integer, 512, 1, 16384, {}},
         {"depth", tr("Slices"), ParameterKind::Integer, 1, 1, 10000, {}}}, [this](const QVariantMap &p) {
        const std::vector<std::size_t> shape{p.value("width").toUInt(), p.value("height").toUInt(), p.value("depth").toUInt()};
        std::size_t bytes = 1;
        for (const auto length : shape) {
            if (length > std::numeric_limits<std::size_t>::max() / bytes)
                throw std::length_error(QCoreApplication::translate("MainWindow", "New image exceeds the addressable size").toStdString());
            bytes *= length;
        }
        detail::checkMemoryBudget(bytes, detail::availableMemoryBytes());
        auto output = std::make_shared<Image>();
        jobs_.submit(tr("Creating image"), [shape, bytes, output](auto stop, auto progress) {
            if (stop.stop_requested()) throw std::runtime_error(QCoreApplication::translate("MainWindow", "Image creation cancelled").toStdString());
            detail::checkMemoryBudget(bytes, detail::availableMemoryBytes());
            ImageMetadata metadata;
            metadata.name = tr("Untitled");
            *output = Image::allocate(PixelType::UInt8, shape, std::move(metadata));
            if (stop.stop_requested()) throw std::runtime_error(QCoreApplication::translate("MainWindow", "Image creation cancelled").toStdString());
            progress(1);
        }, [this, output](auto error) {
            if (showJobError(error)) return;
            addDocument(*output)->modified = true;
            updatePanels();
        });
    }});
    for (const bool crop : {false, true}) {
        commands_.add({crop ? "image.crop" : "image.duplicate", crop ? tr("Crop") : tr("Duplicate"), QT_TRANSLATE_NOOP("CommandCategories", "Edit"),
            QKeySequence(crop ? "Shift+X" : "Shift+D"), true, {}, [this, crop](const QVariantMap &) {
            auto *doc = activeDocument();
            if (!doc) return;
            Image output = doc->image;
            if (const auto roi = selectedRoi()) {
                if (roi->type == RoiType::Line) { showError(tr("Select an area ROI first.")); return; }
                output = cropToBounds(output, roi->bounds());
                if (output.empty()) { showError(tr("Select an area ROI first.")); return; }
            } else if (crop) { showError(tr("Select an area ROI first.")); return; }
            output = output.clone();
            output.metadata().sourcePath.clear();
            if (crop) { doc->setSelection({}); doc->rois.clear(); doc->replaceImage(std::move(output), tr("Crop")); }
            else { output.metadata().name += tr(" copy"); addDocument(std::move(output))->modified = true; }
            updatePanels();
        }});
    }
    commands_.add({"selection.all", tr("Select all"), QT_TRANSLATE_NOOP("CommandCategories", "Edit"), QKeySequence("A"), true, {}, [this](const QVariantMap &) {
        auto *doc = activeDocument();
        if (!doc) return;
        doc->previousSelection = selectedRoi();
        Roi roi;
        roi.name = tr("All");
        roi.points = {{0, 0}, {double(doc->image.width()), double(doc->image.height())}};
        roi.z = doc->viewer->slice();
        roi.channel = doc->viewer->channel();
        roi.time = doc->viewer->time();
        doc->setSelection(roi);
    }});
    commands_.add({"selection.none", tr("Select none"), QT_TRANSLATE_NOOP("CommandCategories", "Edit"), QKeySequence("Shift+A"), true, {}, [this](const QVariantMap &) {
        auto *doc = activeDocument();
        if (!doc) return;
        doc->previousSelection = selectedRoi();
        doc->setSelection({});
    }});
    commands_.add({"selection.restore", tr("Restore selection"), QT_TRANSLATE_NOOP("CommandCategories", "Edit"), QKeySequence("Shift+E"), true, {}, [this](const QVariantMap &) {
        auto *doc = activeDocument();
        if (doc && doc->previousSelection) doc->setSelection(doc->previousSelection);
    }});
    commands_.add({"selection.properties", tr("Selection properties..."), QT_TRANSLATE_NOOP("CommandCategories", "Edit"), {}, true, {}, [this](const QVariantMap &) {
        const QPointer<DocumentWidget> doc = activeDocument();
        const auto roi = selectedRoi();
        if (!doc) return;
        if (!roi) { showError(tr("Select an ROI first.")); return; }
        showSelectionPropertiesDialog(this, *roi, [doc](const Roi &edited) {
            if (doc) doc->editRoi(edited);
        });
    }});
    for (const auto &entry : {std::pair{"view.display", "display"}, {"view.info", "properties"},
                              {"view.properties", "properties"}, {"view.channels", "display"}}) {
        const QMap<QString, QString> labels{{"view.display", tr("Display")}, {"view.info", tr("Show info")},
            {"view.properties", tr("Image properties")}, {"view.channels", tr("Channels")}};
        commands_.add({entry.first, labels.value(entry.first), QT_TRANSLATE_NOOP("CommandCategories", "View"), {}, QString(entry.first) != "view.display", {}, [this, panel = QString(entry.second)](const QVariantMap &) { showPanel(panel); }});
    }
    commands_.add({"overlay.add", tr("Add selection to overlay"), QT_TRANSLATE_NOOP("CommandCategories", "Edit"), QKeySequence("B"), true, {}, [this](const QVariantMap &) {
        const QPointer<DocumentWidget> doc = activeDocument();
        const auto roi = selectedRoi();
        if (!doc) return;
        if (!roi) { showError(tr("Select an ROI first.")); return; }
        Annotation annotation{*roi, roi->name};
        annotation.geometry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        annotation.geometry.color = drawingColor_.rgba();
        if (QApplication::keyboardModifiers().testFlag(Qt::AltModifier)) {
            QDialog dialog(this);
            dialog.setObjectName("annotationOptions");
            dialog.setWindowTitle(tr("Overlay annotation"));
            auto *layout = new QFormLayout(&dialog);
            auto *label = new QLineEdit(annotation.text, &dialog);
            label->setObjectName("annotationText");
            layout->addRow(tr("Label"), label);
            auto *color = new QPushButton(tr("Color..."), &dialog);
            layout->addRow(color);
            connect(color, &QPushButton::clicked, &dialog, [&] {
                const auto chosen = QColorDialog::getColor(QColor::fromRgba(annotation.geometry.color), &dialog, tr("Annotation color"));
                if (chosen.isValid()) annotation.geometry.color = chosen.rgba();
            });
            auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
            layout->addRow(buttons);
            connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
            connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            if (dialog.exec() != QDialog::Accepted || !doc) return;
            annotation.text = label->text();
        }
        doc->annotations.push_back(std::move(annotation));
        doc->annotationsVisible = true;
        doc->refreshRois();
        doc->changed();
    }});
    commands_.add({"overlay.toggle", tr("Show/hide annotations"), QT_TRANSLATE_NOOP("CommandCategories", "Edit"), {}, true, {}, [this](const QVariantMap &) {
        if (auto *doc = activeDocument()) { doc->annotationsVisible = !doc->annotationsVisible; doc->refreshRois(); }
    }});
    commands_.add({"overlay.removeLast", tr("Remove last annotation"), QT_TRANSLATE_NOOP("CommandCategories", "Edit"), {}, true, {}, [this](const QVariantMap &) {
        if (auto *doc = activeDocument(); doc && !doc->annotations.empty()) { doc->annotations.pop_back(); doc->refreshRois(); doc->changed(); }
    }});
    commands_.add({"selection.add", tr("Add to ROI manager"), QT_TRANSLATE_NOOP("CommandCategories", "Analyze"), QKeySequence("T"), true, {}, [this](const QVariantMap &) {
        auto *doc = activeDocument();
        const auto roi = selectedRoi();
        if (!doc || !roi) return;
        if (std::none_of(doc->rois.begin(), doc->rois.end(), [&](const Roi &saved) { return saved.id == roi->id; })) {
            auto saved = *roi;
            saved.name = tr("ROI %1").arg(doc->rois.size() + 1);
            doc->rois.push_back(std::move(saved));
        }
        showPanel("rois");
    }});
    commands_.add({"view.fit", tr("Fit to window"), QT_TRANSLATE_NOOP("CommandCategories", "View"), QKeySequence("Ctrl+0"), true, {}, [this](const QVariantMap &) {
        if (auto *overlay = activeOverlay()) overlay->viewer->fitToWindow();
        else if (auto *doc = activeDocument()) doc->viewer->fitToWindow();
    }});
    commands_.add({"view.single", tr("Single view"), QT_TRANSLATE_NOOP("CommandCategories", "View"), {}, true, {}, [this](const QVariantMap &) {
        QPointer<DocumentWidget> doc = activeDocument();
        if (!doc) return;
        if (auto *overlay = activeOverlay()) overlay->close();
        activeOverlay_.clear();
        doc->unlink();
        doc->setOrthogonalViews(false);
        updatePanels();
    }});
    commands_.add({"view.linked", tr("Linked view"), QT_TRANSLATE_NOOP("CommandCategories", "View"), {}, true, {}, [this](const QVariantMap &) {
        QPointer<DocumentWidget> doc = activeDocument();
        if (!doc) return;
        QList<QPointer<DocumentWidget>> candidates;
        QDialog picker(this);
        picker.setWindowTitle(tr("Linked view"));
        auto *layout = new QVBoxLayout(&picker);
        auto *images = new QListWidget(&picker);
        images->setObjectName("linkedImages");
        images->setAccessibleName(tr("Linked images"));
        layout->addWidget(images);
        for (auto *other : documents()) if (other != doc) {
            candidates.append(other);
            auto *item = new QListWidgetItem(tr("%1. %2").arg(candidates.size()).arg(other->title()), images);
            item->setToolTip(other->image.metadata().sourcePath);
            item->setCheckState(doc->linkedDocuments().contains(other) ? Qt::Checked : Qt::Unchecked);
        }
        if (candidates.isEmpty()) return;
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &picker);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &picker, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &picker, &QDialog::reject);
        picker.resize(400, 300);
        if (picker.exec() == QDialog::Accepted && doc) {
            QList<DocumentWidget *> selected;
            for (int i = 0; i < candidates.size(); ++i)
                if (candidates[i] && images->item(i)->checkState() == Qt::Checked) selected.append(candidates[i]);
            activeOverlay_.clear();
            doc->linkTo(selected);
        }
        updatePanels();
    }});
    commands_.add({"view.orthogonal", tr("Orthogonal views"), QT_TRANSLATE_NOOP("CommandCategories", "View"), {}, true, {}, [this](const QVariantMap &) {
        if (auto *overlay = activeOverlay()) {
            if (!overlay->orthogonalViews()) {
                overlay->setOrthogonalViews(true);
            } else {
                bool hidden = false;
                for (int i = 1; i < overlay->views().size(); ++i)
                    hidden |= !overlay->views()[i]->window()->isVisible();
                if (hidden) overlay->showViews();
                else overlay->setOrthogonalViews(false);
            }
            updatePanels();
            return;
        }
        QPointer<DocumentWidget> doc = activeDocument();
        if (!doc) return;
        QList<DocumentWidget *> group{doc};
        for (const auto &other : doc->linkedDocuments()) if (other) group.append(other);
        const bool anyOrthogonal = std::any_of(group.begin(), group.end(),
            [](DocumentWidget *member) { return member->orthogonalViews(); });
        if (!anyOrthogonal) {
            for (auto *member : group)
                if (member->image.depth() > 1) member->setOrthogonalViews(true);
        } else {
            bool hidden = false;
            for (auto *member : group)
                for (int i = 1; i < member->views().size(); ++i) {
                    auto *window = member->views()[i]->window();
                    if (window->property("sliceReady").toBool() && !window->isVisible()) hidden = true;
                }
            if (hidden) {
                for (auto *member : group) if (member->orthogonalViews()) member->showViews();
            } else {
                for (auto *member : group) member->setOrthogonalViews(false);
                if (doc->isLinked()) doc->viewer->fitToWindow();
            }
        }
        updatePanels();
    }});
    commands_.add({"view.overlay", tr("Overlay view"), QT_TRANSLATE_NOOP("CommandCategories", "View"), {}, true, {}, [this](const QVariantMap &) {
        auto *document = activeDocument();
        if (!document) return;
        OverlayWindow *overlay = nullptr;
        for (auto *window : findChildren<OverlayWindow *>(QString{}, Qt::FindDirectChildrenOnly))
            if (window->document() == document) overlay = window;
        if (!overlay) {
            overlay = new OverlayWindow(document, this);
            overlay->setScreen(document->screen());
            overlay->move(document->pos() + QPoint(28, 28));
            connect(overlay, &OverlayWindow::activated, this, [this, overlay] { setActiveOverlay(overlay); });
            connect(overlay, &OverlayWindow::visibilityChanged, this, [this, overlay] {
                if (activeOverlay_ == overlay && !overlay->isVisible()) activeOverlay_.clear();
                updatePanels();
            });
            connect(overlay, &OverlayWindow::viewError, this, [this](const QString &error) { statusBar()->showMessage(error, 15000); });
        }
        overlay->refreshDocuments(documents());
        overlay->showEditor();
        setActiveOverlay(overlay);
    }});
    commands_.add({"process.repeat", tr("Repeat command"), QT_TRANSLATE_NOOP("CommandCategories", "Process"), QKeySequence("Shift+R"), true, {}, [this](const QVariantMap &) {
        if (!repeatCommand_.isEmpty()) commands_.execute(repeatCommand_, repeatParameters_);
    }});
    commands_.add({"window.close", tr("Close window"), QT_TRANSLATE_NOOP("CommandCategories", "Window"), QKeySequence::Close, false, {}, [this](const QVariantMap &) {
        auto *window = QApplication::activeWindow();
        if (window && window != this) window->close(); else if (active_) active_->close();
    }});
    commands_.add({"window.showAll", tr("Show all windows"), QT_TRANSLATE_NOOP("CommandCategories", "Window"), QKeySequence("Shift+F"), false, {}, [this](const QVariantMap &) {
        showNormal();
        for (auto *doc : documents()) doc->showViews();
        for (auto *overlay : findChildren<OverlayWindow *>(QString{}, Qt::FindDirectChildrenOnly)) overlay->showViews();
        for (auto *panel : panels_) if (panel->isVisible() || panel->isMinimized()) panel->showNormal();
    }});
    commands_.add({"window.next", tr("Next image window"), QT_TRANSLATE_NOOP("CommandCategories", "Window"), QKeySequence("Tab"), true, {}, [this](const QVariantMap &) {
        const auto list = documents();
        if (list.isEmpty()) return;
        auto *next = list[(list.indexOf(activeDocument()) + 1) % list.size()];
        setActiveDocument(next);
        next->showNormal();
        next->raise();
        next->activateWindow();
        next->viewer->setFocus();
    }});
    commands_.add({"window.launcher", tr("ImageCXX toolbar"), QT_TRANSLATE_NOOP("CommandCategories", "Window"), QKeySequence("Return"), false, {}, [this](const QVariantMap &) {
        showNormal();
        raise();
        activateWindow();
    }});

    commands_.add({"view.controlPanel", tr("Control panel..."), QT_TRANSLATE_NOOP("CommandCategories", "View"), QKeySequence("Shift+U"), false, {}, [this](const QVariantMap &) {
        showPanel("controlPanel");
    }});

    QMap<QString, QMenu *> menus;
    for (const auto &category : {QT_TRANSLATE_NOOP("CommandCategories", "File"),
            QT_TRANSLATE_NOOP("CommandCategories", "Edit"), QT_TRANSLATE_NOOP("CommandCategories", "Process"),
            QT_TRANSLATE_NOOP("CommandCategories", "Analyze"), QT_TRANSLATE_NOOP("CommandCategories", "View")})
        menus.insert(category, menuBar()->addMenu(QCoreApplication::translate("CommandCategories", category)));
    const QMap<QString, QStringList> bindings{
            {"file.open", {"O"}}, {"file.new", {"N"}}, {"file.save", {"S"}},
            {"file.next", {"Shift+O", "Alt+Shift+O"}}, {"file.revert", {"R", "Alt+R"}},
            {"edit.copy", {"C", "Alt+C", "Ctrl+Alt+C"}}, {"image.duplicate", {"Shift+D", "Alt+Shift+D"}},
            {"edit.undo", {"Z"}}, {"edit.redo", {"Ctrl+Y"}},
            {"overlay.add", {"B", "Alt+B", "Ctrl+Alt+B"}}, {"analyze.measure", {"M"}}, {"analyze.histogram", {"H"}}, {"analyze.profile", {"K"}},
            {"edit.clear", {"Backspace", "Delete"}}, {"edit.fill", {"F"}}, {"edit.draw", {"D"}},
            {"edit.invert", {"Shift+I"}}, {"edit.smooth", {"Shift+S"}}, {"process.threshold", {"Shift+T"}}, {"process.resample", {"E"}},
            {"view.display", {"Ctrl+J", "Shift+C"}}, {"view.info", {"I"}}, {"view.properties", {"Shift+P"}},
            {"view.channels", {"Shift+Z"}}, {"selection.add", {"T"}}, {"window.close", {"W"}},
            {"window.showAll", {"Shift+F", "]"}}, {"window.launcher", {"Return", "Enter"}}
        };
    QMap<QString, QAction *> actions;
    for (const auto &command : commands_.commands()) {
        auto *action = new QAction(command.label, this);
        action->setObjectName(command.id);
        actions.insert(command.id, action);

        QList<QKeySequence> shortcuts;
        if (command.id == "edit.redo") shortcuts.append(QKeySequence("Ctrl+Y"));
        else if (!command.shortcut.isEmpty()) shortcuts.append(command.shortcut);
        for (const auto &binding : bindings.value(command.id)) {
            const QKeySequence key(binding);
            if (!shortcuts.contains(key)) shortcuts.append(key);
        }
        // Qt maps ControlModifier to Command on macOS, and AltModifier to Option.
        const auto bareKeys = shortcuts;
        for (const auto &key : bareKeys) {
            if (key.count() == 1 && !(key[0].keyboardModifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
                && key[0].key() >= Qt::Key_A && key[0].key() <= Qt::Key_Z) {
                const QKeySequence modified(key[0].toCombined() | Qt::CTRL);
                if (!shortcuts.contains(modified)) shortcuts.append(modified);
            }
        }
        action->setShortcuts(shortcuts);
        for (const auto &key : shortcuts) shortcutActions_.insert(key, action);
        action->setShortcutContext(Qt::ApplicationShortcut);
        const QMap<QString, QString> icons{{"file.open", "open"}, {"analyze.measure", "measure"},
            {"analyze.histogram", "histogram"}, {"edit.undo", "undo"}};
        if (icons.contains(command.id)) action->setIcon(QIcon(":/icons/" + icons.value(command.id) + ".svg"));
        if (command.id == "edit.color") {
            QPixmap swatch(24, 24);
            swatch.fill(drawingColor_);
            action->setIcon(QIcon(swatch));
        }
        connect(action, &QAction::triggered, this, [this, id = command.id] { executeCommand(id); });
        if (command.needsImage) imageActions_.append(action);
    }
    auto *quit = new QAction(tr("Quit"), this);
    quit->setShortcut(QKeySequence::Quit);
    quit->setShortcutContext(Qt::ApplicationShortcut);
    connect(quit, &QAction::triggered, this, &QWidget::close);
    auto *search = new QAction(tr("Find command..."), this);
    search->setObjectName("commands.find");
    search->setShortcuts({QKeySequence("L"), QKeySequence("Ctrl+L")});
    for (const auto &key : search->shortcuts()) shortcutActions_.insert(key, search);
    search->setShortcutContext(Qt::ApplicationShortcut);
    crosshair_ = new QAction(tr("Crosshair"), this);
    crosshair_->setCheckable(true);
    connect(crosshair_, &QAction::toggled, this, [this](bool value) {
        if (auto *overlay = activeOverlay()) overlay->viewer->setCrosshairVisible(value);
        else if (auto *doc = activeDocument()) doc->viewer->setCrosshairVisible(value);
    });
    scaleBar_ = new QAction(tr("Scale bar"), this);
    scaleBar_->setCheckable(true);
    scaleBar_->setChecked(false);
    connect(scaleBar_, &QAction::toggled, this, [this](bool value) {
        if (auto *overlay = activeOverlay()) overlay->viewer->setScaleBarVisible(value);
        else if (auto *doc = activeDocument()) doc->viewer->setScaleBarVisible(value);
    });
    const auto addItems = [&actions](QMenu *menu, std::initializer_list<const char *> ids) {
        for (const char *id : ids) {
            if (!id) menu->addSeparator();
            else {
                auto *action = actions.value(QLatin1String(id));
                Q_ASSERT(action);
                menu->addAction(action);
            }
        }
    };
    addItems(menus["File"], {"file.new", "file.clipboard", "file.text", "file.capture", nullptr,
        "file.open", "file.folder", "file.dicomSeries", "file.raw"});
    recentMenu_ = menus["File"]->addMenu(tr("Recent files"));
    recentMenu_->setObjectName("recentMenu");
    addItems(menus["File"], {"file.next", nullptr,
        "file.save", "file.revert", nullptr,
        "file.print", "file.printPreview", nullptr});
    menus["File"]->addAction(quit);

    addItems(menus["Edit"], {"edit.undo", "edit.redo", nullptr,
        "edit.cut", "edit.copy", "edit.paste", "edit.pasteSpecial", nullptr,
        "edit.clear", "edit.fill", "edit.draw", "edit.invert", "edit.smooth", "edit.color", nullptr,
        "image.crop", "image.duplicate", nullptr});
    auto *selectionMenu = menus["Edit"]->addMenu(tr("Selection"));
    selectionMenu->setObjectName("selectionMenu");
    addItems(selectionMenu, {"selection.all", "selection.none", "selection.restore", nullptr, "selection.properties"});
    auto *overlayMenu = menus["Edit"]->addMenu(tr("Overlay"));
    overlayMenu->setObjectName("overlayMenu");
    addItems(overlayMenu, {"overlay.add", "overlay.toggle", "overlay.removeLast"});
    menus["Edit"]->addSeparator();
    menus["Edit"]->addAction(search);

    auto *stacksMenu = new QMenu(tr("Stacks"), this);
    stacksMenu->setObjectName("stacksMenu");
    addItems(stacksMenu, {"stacks.reslice", "stacks.project3d"});
    addItems(menus["Process"], {"process.repeat", nullptr,
        "process.gaussian", "process.median", "process.threshold", "process.dilate", "process.erode", "process.resample", nullptr});
    menus["Process"]->addMenu(stacksMenu);
    menus["Process"]->addSeparator();
    addItems(menus["Process"], {"process.batch"});

    addItems(menus["Analyze"], {"analyze.measure", "analyze.multiMeasure", "selection.add", "results.export", nullptr,
        "analyze.histogram", "analyze.histogram.log", "analyze.histogram.options",
        "analyze.profile", "analyze.profile.options",
        "analyze.roiPlot", "analyze.scatter", nullptr,
        "analyze.mask"});
    auto *gelsMenu = menus["Analyze"]->addMenu(tr("Gels"));
    gelsMenu->setObjectName("gelsMenu");
    addItems(gelsMenu, {"gel.first", "gel.next", "gel.plot", "gel.label", nullptr, "gel.options"});

    addItems(menus["View"], {"view.fit", "image.animate", nullptr,
        "view.single", "view.linked", "view.orthogonal", "view.overlay", nullptr});
    menus["View"]->addAction(crosshair_);
    menus["View"]->addAction(scaleBar_);
    menus["View"]->addSeparator();
    addItems(menus["View"], {"view.display", "view.properties", "view.controlPanel"});
    // Keep shortcut-only panel aliases attached to the window so Qt dispatches their shortcuts.
    addAction(actions.value("view.channels"));
    addAction(actions.value("view.info"));
    auto *query = new QLineEdit(this);
    query->setObjectName("commandSearch");
    query->setPlaceholderText(tr("Find command..."));
    query->setAccessibleName(tr("Find command"));
    query->setToolTip(search->text() + " (" + QKeySequence("Ctrl+L").toString(QKeySequence::NativeText) + ")");
    query->setClearButtonEnabled(true);
    query->setFixedWidth(220);
    statusBar()->addPermanentWidget(query);
    auto *searchModel = new QStandardItemModel(query);
    for (const auto &command : commands_.commands()) {
        auto *item = new QStandardItem(QCoreApplication::translate("CommandCategories", command.category.toUtf8().constData()) + "  /  " + command.label);
        item->setData(command.id, Qt::UserRole);
        item->setEditable(false);
        item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        auto *action = findChild<QAction *>(command.id);
        item->setEnabled(action->isEnabled());
        connect(action, &QAction::changed, query, [action, item] { item->setEnabled(action->isEnabled()); });
        searchModel->appendRow(item);
    }
    auto *completer = new QCompleter(searchModel, query);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    completer->setMaxVisibleItems(10);
    completer->popup()->setMinimumWidth(420);
    completer->popup()->setStyleSheet(R"(
        QListView { background: #FDFBF7; color: #18191B; border: 1px solid #CFC7BA; border-radius: 6px; padding: 4px; outline: 0; }
        QListView::item { padding: 5px 10px; border-radius: 4px; }
        QListView::item:hover { background: #EFEAE0; }
        QListView::item:selected { background: #DDD5C8; color: #18191B; }
    )");
    query->setCompleter(completer);
    const auto runSearch = [this, query](const QModelIndex &index) {
        if (!(index.flags() & Qt::ItemIsEnabled)) return;
        const auto id = index.data(Qt::UserRole).toString();
        // Let QLineEdit finish inserting the completion before clearing it or opening a command dialog.
        QTimer::singleShot(0, this, [this, query, id] { query->clear(); executeCommand(id); });
    };
    connect(completer, qOverload<const QModelIndex &>(&QCompleter::activated), this, runSearch);
    connect(query, &QLineEdit::returnPressed, this, [query, completer, runSearch] {
        if (query->text().isEmpty() || completer->popup()->currentIndex().isValid()) return;
        completer->setCompletionPrefix(query->text());
        runSearch(completer->currentIndex());
    });
    connect(search, &QAction::triggered, this, [this, query, completer] {
        showNormal();
        raise();
        activateWindow();
        query->setFocus(Qt::ShortcutFocusReason);
        query->selectAll();
        completer->setCompletionPrefix(query->text());
        completer->complete();
    });
    windowMenu_ = menuBar()->addMenu(tr("Window"));
    windowMenu_->setObjectName("windowMenu");
    connect(windowMenu_, &QMenu::aboutToShow, this, &MainWindow::updateWindowMenu);
    auto *helpMenu = menuBar()->addMenu(tr("Help"));
    helpMenu->setObjectName("helpMenu");
    auto *checkUpdates = helpMenu->addAction(tr("Check for Updates..."));
    checkUpdates->setObjectName("help.checkUpdates");
    checkUpdates->setMenuRole(QAction::NoRole);
    checkUpdates->setEnabled(false);
    auto *toolbar = addToolBar(tr("Image tools"));
    toolbar->setObjectName("imageTools");
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolbar->setIconSize(QSize(24, 24));
    toolbar->addAction(findChild<QAction *>("file.open"));
    toolbar->addSeparator();
    tools_ = new QActionGroup(this);
    const std::vector<std::pair<QString, ImageViewer::Tool>> tools{{tr("Pointer"), ImageViewer::Tool::Pointer},
        {tr("Rectangle"), ImageViewer::Tool::Rectangle}, {tr("Ellipse"), ImageViewer::Tool::Ellipse},
        {tr("Polygon"), ImageViewer::Tool::Polygon}, {tr("Line"), ImageViewer::Tool::Line}};
    for (const auto &[name, tool] : tools) {
        auto *action = toolbar->addAction(name);
        const QStringList iconNames{"pointer", "rectangle", "ellipse", "polygon", "line"};
        action->setIcon(QIcon(":/icons/" + iconNames[int(tool)] + ".svg"));
        action->setCheckable(true);
        tools_->addAction(action);
        if (tool == ImageViewer::Tool::Pointer) action->setToolTip(tr("Click or drag to move the crosshair in orthogonal views. Middle-button or Space + drag to pan. Pinch to zoom."));
        else if (tool == ImageViewer::Tool::Polygon) action->setToolTip(tr("Draw a polygon. Double-click to finish."));
        action->setData(static_cast<int>(tool));
        if (tool == ImageViewer::Tool::Pointer) action->setChecked(true);
        connect(action, &QAction::triggered, this, [this, tool] { if (auto *doc = activeDocument()) doc->viewer->setTool(tool); });
    }
    toolbar->addSeparator();
    toolbar->addAction(findChild<QAction *>("analyze.measure"));
    toolbar->addAction(findChild<QAction *>("analyze.histogram"));
    toolbar->addAction(findChild<QAction *>("edit.undo"));
    toolbar->addAction(findChild<QAction *>("edit.color"));
    toolbar->addSeparator();
    lutMenu_ = new QMenu(tr("LUT"), toolbar);
    lutMenu_->setObjectName("lutMenu");
    invertLut_ = lutMenu_->addAction(tr("Invert LUT"));
    invertLut_->setCheckable(true);
    connect(invertLut_, &QAction::triggered, this, [this](bool inverted) {
        if (loadingPanels_) return;
        if (auto *doc = activeDocument()) doc->viewer->setInverted(inverted);
        updateDisplayControls();
    });
    lutMenu_->addSeparator();
    auto *luts = new QActionGroup(lutMenu_);
    for (const auto &name : QStringList{QT_TRANSLATE_NOOP("ColorMaps", "grayscale"), QT_TRANSLATE_NOOP("ColorMaps", "hot"), QT_TRANSLATE_NOOP("ColorMaps", "viridis")}) {
        auto *action = lutMenu_->addAction(QCoreApplication::translate("ColorMaps", name.toUtf8().constData()));
        action->setCheckable(true);
        action->setData(name);
        luts->addAction(action);
    }
    toolbar->addAction(stacksMenu->menuAction());
    toolbar->addAction(lutMenu_->menuAction());
    auto *lutButton = qobject_cast<QToolButton *>(toolbar->widgetForAction(lutMenu_->menuAction()));
    lutButton->setObjectName("lutButton");
    lutButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    lutButton->setPopupMode(QToolButton::InstantPopup);
    lutButton->setToolTip(tr("Color map"));
    lutButton->setStyleSheet(R"(
        QToolButton#lutButton { min-width: 24px; min-height: 24px; }
        QToolButton#lutButton::menu-indicator { image: none; width: 0px; }
    )");
    connect(lutMenu_, &QMenu::aboutToShow, this, &MainWindow::updateDisplayControls);
    auto *stacksButton = qobject_cast<QToolButton *>(toolbar->widgetForAction(stacksMenu->menuAction()));
    stacksButton->setObjectName("stacksButton");
    stacksButton->setText(tr("Stk"));
    stacksButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    stacksButton->setPopupMode(QToolButton::InstantPopup);
    stacksButton->setAccessibleName(tr("Stacks"));
    stacksButton->setToolTip(tr("Stacks"));
    stacksButton->setStyleSheet(R"(
        QToolButton#stacksButton { min-width: 24px; min-height: 24px; padding: 5px; }
        QToolButton#stacksButton::menu-indicator { image: none; width: 0px; }
    )");
    connect(luts, &QActionGroup::triggered, this, [this](QAction *action) {
        if (loadingPanels_) return;
        if (auto *doc = activeDocument()) doc->viewer->setLut(action->data().toString());
        updateDisplayControls();
    });
    QSettings settings;
    recentFiles_ = settings.value("recentFiles").toStringList();
    settings.beginGroup("windowWorkflow/v1");
    if (!settings.value("launcherGeometry").toByteArray().isEmpty())
        restoreGeometry(settings.value("launcherGeometry").toByteArray());
    resize(std::clamp(width(), 560, 900), minimumSizeHint().height());
    restorePanelLayout(settings.value("panels").toByteArray());
    updateRecentFiles();
    updateWindowMenu();
    updatePanels();
    qApp->installEventFilter(this);
}

MainWindow::~MainWindow()
{
    qApp->removeEventFilter(this);
    jobs_.cancelAll();
    clearDocuments();
    for (auto *window : panels_) delete window;
    panels_.clear();
}

DocumentWidget *MainWindow::activeDocument() const
{
    return active_;
}

QList<DocumentWidget *> MainWindow::documents() const
{
    QList<DocumentWidget *> list;
    for (const auto &doc : documents_) if (doc) list.append(doc);
    return list;
}

QWidget *MainWindow::panel(const QString &id) const
{
    return panels_.value(id);
}

void MainWindow::updatePixelStatus()
{
    const auto text = pixelStatusText_ + (toolDimensions_.isEmpty() ? QString{} : "   " + toolDimensions_);
    pixelStatus_->setText(text);
    pixelStatus_->setToolTip(text);
}

void MainWindow::setActiveDocument(DocumentWidget *document)
{
    const bool hadOverlay = !activeOverlay_.isNull();
    activeOverlay_.clear();
    if (active_ == document && !hadOverlay) return;
    active_ = document;
    if (document) {
        document->viewer->setCrosshairVisible(crosshair_->isChecked());
        document->viewer->setScaleBarVisible(scaleBar_->isChecked());
        if (auto *action = tools_->checkedAction())
            document->viewer->setTool(static_cast<ImageViewer::Tool>(action->data().toInt()));
        pixelStatusText_ = document->image.metadata().name;
        toolDimensions_ = document->viewer->toolDimensions();
    } else {
        pixelStatusText_ = tr("Ready");
        toolDimensions_.clear();
    }
    updatePixelStatus();
    updatePanels();
}

OverlayWindow *MainWindow::activeOverlay() const
{
    return activeOverlay_ && activeOverlay_->isVisible() && activeOverlay_->document() ? activeOverlay_.data() : nullptr;
}

void MainWindow::setActiveOverlay(OverlayWindow *overlay)
{
    if (!overlay || !overlay->document()) return;
    const bool same = activeOverlay_ == overlay && active_ == overlay->document();
    activeOverlay_ = overlay;
    active_ = overlay->document();
    overlay->viewer->setCrosshairVisible(crosshair_->isChecked());
    overlay->viewer->setScaleBarVisible(scaleBar_->isChecked());
    if (!same) {
        pixelStatusText_ = overlay->document()->image.metadata().name;
        toolDimensions_.clear();
        updatePixelStatus();
    }
    updatePanels();
}

void MainWindow::clearDocuments()
{
    activeOverlay_.clear();
    for (auto *overlay : findChildren<OverlayWindow *>(QString{}, Qt::FindDirectChildrenOnly)) delete overlay;
    // Include windows waiting for WA_DeleteOnClose's deferred destruction.
    const auto windows = findChildren<DocumentWidget *>(QString{}, Qt::FindDirectChildrenOnly);
    documents_.clear();
    active_.clear();
    for (auto *document : windows) {
        disconnect(document, nullptr, this, nullptr);
        disconnect(document->viewer, nullptr, this, nullptr);
        delete document;
    }
}

struct MainWindow::PendingActivation {
    QPointer<MainWindow> window;
    QString token;
    PendingActivation(MainWindow *target, QString value) : window(target), token(std::move(value)) {}
    PendingActivation(const PendingActivation &) = delete;
    PendingActivation &operator=(const PendingActivation &) = delete;
    ~PendingActivation()
    {
        if (window && !token.isEmpty()) activateApplicationWindow(window, token);
    }
};

DocumentWidget *MainWindow::addDocument(Image image, const QString &lut, std::optional<bool> inverted)
{
    return addDocument(std::move(image), lut, inverted, {});
}

DocumentWidget *MainWindow::addDocument(Image image, const QString &lut, std::optional<bool> inverted,
                                        std::shared_ptr<PendingActivation> activation)
{
    auto *document = new DocumentWidget(std::move(image), this);
    if (!lut.isEmpty()) document->viewer->setLut(lut);
    if (inverted.has_value()) document->viewer->setInverted(*inverted);
    connect(document, &DocumentWidget::pathsDropped, this, [this, document](const QStringList &paths) {
        if (document->isVisible()) activateApplicationWindow(document);
        openPaths(paths);
    });
    document->setAttribute(Qt::WA_DeleteOnClose);
    documents_.append(document);
    document->setScreen(screen());
    document->resizeToImage();
    const int offset = int((documents_.size() - 1) % 8) * 28;
    const QPoint position = frameGeometry().bottomLeft() + QPoint(offset, 12 + offset);
    document->move(position);
    connect(document, &DocumentWidget::activated, this, [this, document] { setActiveDocument(document); });
    connect(document->viewer, &ImageViewer::roiContextMenuRequested, this, [this, document](const QPoint &position) {
        setActiveDocument(document);
        const auto roi = document->selection;
        if (roi) document->viewer->setSlice(document->viewer->slice(), roi->channel, document->viewer->time());
        QMenu menu(document->viewer);
        menu.setObjectName("roiContextMenu");
        for (const auto &id : {"analyze.measure", "selection.add", "analyze.histogram"})
            menu.addAction(findChild<QAction *>(id));
        if (roi && (roi->type == RoiType::Line || roi->type == RoiType::Rectangle))
            menu.addAction(findChild<QAction *>("analyze.profile"));
        menu.addSeparator();
        menu.addAction(findChild<QAction *>("selection.properties"));
        menu.addAction(findChild<QAction *>("selection.none"));
        menu.exec(position);
    });
    connect(document, &DocumentWidget::closing, this, [this, document] {
        documents_.removeAll(QPointer<DocumentWidget>(document));
        if (active_ == document) {
            const auto remaining = documents();
            setActiveDocument(remaining.isEmpty() ? nullptr : remaining.last());
        }
        updatePanels();
    });
    connect(document, &DocumentWidget::changed, this, [this, document] {
        document->setWindowTitle(document->title());
        if (activeDocument() == document) updatePanels();
    });
    connect(document, &DocumentWidget::toolDimensionsChanged, this, [this, document](const QString &text) {
        if (activeDocument() != document) return;
        toolDimensions_ = text;
        updatePixelStatus();
    });
    connect(document, &DocumentWidget::pixelHovered, this, [this, document](std::size_t x, std::size_t y, std::size_t z, double value) {
        if (activeDocument() != document) return;
        const auto &metadata = document->image.metadata();
        const double intensity = value * metadata.intensityScale + metadata.intensityOffset;
        pixelStatusText_ = tr("X %1   Y %2   Z %3   V %4").arg(x).arg(y).arg(z).arg(intensity, 0, 'g', 12);
        if (!metadata.intensityUnit.isEmpty()) pixelStatusText_ += " " + metadata.intensityUnit;
        if (intensity != value && !std::isnan(value)) pixelStatusText_ += tr(" (%1)").arg(value, 0, 'g', 12);
        updatePixelStatus();
    });
    connect(document, &DocumentWidget::viewError, this, [this](const QString &error) { statusBar()->showMessage(error, 15000); });
    connect(document, &DocumentWidget::displayChanged, this, [this, document] {
        if (activeDocument() == document) updateDisplayControls();
    });
    connect(document->viewer, &ImageViewer::sliceReady, this, [this, document] {
        if (activeDocument() == document) updateDisplayControls();
    });
    const auto showWhenReady = connect(document->viewer, &ImageViewer::sliceReady, document,
        [this, document, activation = std::move(activation)]() mutable {
        auto guard = std::move(activation);
        auto *active = activeDocument();
        const bool shouldActivate = active == document;
        const QString token = shouldActivate && guard ? std::exchange(guard->token, {}) : QString{};
        if (!token.isEmpty()) qputenv("XDG_ACTIVATION_TOKEN", token.toUtf8());
        document->setAttribute(Qt::WA_ShowWithoutActivating, !shouldActivate);
        document->show();
        document->setAttribute(Qt::WA_ShowWithoutActivating, false);
        setActiveDocument(active);
        if (active && active->isVisible()) {
            active->raise();
            active->activateWindow();
            if (shouldActivate) active->viewer->setFocus();
        }
        if (!token.isEmpty()) qunsetenv("XDG_ACTIVATION_TOKEN");
    }, Qt::SingleShotConnection);
    connect(document->viewer, &ImageViewer::loadFailed, this, [this, document, showWhenReady](const QString &error) {
        if (disconnect(showWhenReady)) document->close();
        statusBar()->showMessage(error, 15000);
    });
    connect(document->viewer, &ImageViewer::zoomChanged, this, [this, document](double scale) {
        if (activeDocument() == document)
            statusBar()->showMessage(tr("Zoom %1%").arg(scale * 100, 0, 'f', 1), 2000);
    });
    setActiveDocument(document);
    for (auto *overlay : findChildren<OverlayWindow *>(QString{}, Qt::FindDirectChildrenOnly)) overlay->refreshDocuments(documents());
    return document;
}

void MainWindow::pasteImage(bool systemImage, bool newImage)
{
    auto clipboard = readImageFromClipboard(systemImage);
    if (clipboard.empty()) return;
    auto *doc = activeDocument();
    if (newImage || !doc) {
        clipboard.metadata().name = tr("Clipboard");
        clipboard.metadata().sourcePath.clear();
        addDocument(std::move(clipboard))->modified = true;
        updatePanels();
        return;
    }
    if (doc->image.shape().size() > 5) { showError(tr("Pixel editing supports up to five dimensions.")); return; }
    if (clipboard.pixelType() != doc->image.pixelType()) {
        showError(tr("Clipboard and image pixel types must match. Use Paste special to paste into a new image."));
        return;
    }
    const Image input = doc->image;
    const QPointer<DocumentWidget> target = doc;
    const auto revision = doc->revision;
    const auto z = doc->viewer->slice(), c = doc->viewer->channel(), t = doc->viewer->time();
    auto output = std::make_shared<Image>();
    const auto roi = selectedRoi();
    const auto origin = roi ? roi->bounds().topLeft().toPoint() : QPoint(0, 0);
    jobs_.submit(tr("Paste"), [input, clipboard, output, origin, z, c, t](auto stop, auto progress) {
        *output = input.clone();
        const auto pixelBytes = bytesPerPixel(input.pixelType());
        auto *destination = output->mutableData() + (z + input.depth() * (c + input.channels() * t))
            * input.width() * input.height() * pixelBytes;
        const auto left = std::max<qint64>(0, origin.x()), top = std::max<qint64>(0, origin.y());
        const auto right = std::min<qint64>(input.width(), qint64(origin.x()) + qint64(clipboard.width()));
        const auto bottom = std::min<qint64>(input.height(), qint64(origin.y()) + qint64(clipboard.height()));
        for (auto y = top; y < bottom && right > left; ++y) {
            if (stop.stop_requested()) throw std::runtime_error(QCoreApplication::translate("MainWindow", "Paste cancelled").toStdString());
            std::memcpy(destination + (y * input.width() + left) * pixelBytes,
                clipboard.data() + ((y - origin.y()) * clipboard.width() + left - origin.x()) * pixelBytes,
                (right - left) * pixelBytes);
            progress(double(y - top + 1) / (bottom - top));
        }
    }, [this, target, revision, output](auto error) {
        if (showJobError(error)) return;
        if (target && target->revision == revision) target->replaceImage(*output, tr("Paste"));
        else {
            output->metadata().sourcePath.clear();
            addDocument(*output)->modified = true;
        }
        updatePanels();
    });
}

void MainWindow::importRaw(const QString &path, std::shared_ptr<PendingActivation> activation)
{
    if (activation && !activation->token.isEmpty())
        activateApplicationWindow(this, std::exchange(activation->token, {}));
    const auto options = showRawImportDialog(this, path);
    if (!options) return;
    auto output = std::make_shared<Image>();
    jobs_.submit(tr("Importing raw data"),
        [path, options = *options, output](auto stop, auto progress) { *output = readImage(path, options, stop, progress); },
        [this, path, output, activation = std::move(activation)](auto error) mutable {
            auto guard = std::move(activation);
            if (error) { guard.reset(); showJobError(error); return; }
            addDocument(*output, {}, std::nullopt, std::move(guard));
            recentFiles_.removeAll(path);
            recentFiles_.prepend(path);
            while (recentFiles_.size() > 12) recentFiles_.removeLast();
            updateRecentFiles();
        });
}

void MainWindow::openPaths(const QStringList &paths, const QString &activationToken)
{
    auto activation = activationToken.isEmpty()
        ? std::shared_ptr<PendingActivation>{}
        : std::make_shared<PendingActivation>(this, activationToken);
    QSet<QString> metaImagePayloads;
    for (const auto &path : paths) {
        const QFileInfo info(path);
        if (info.suffix().compare("mhd", Qt::CaseInsensitive) != 0) continue;
        QFile header(path);
        if (!header.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        while (!header.atEnd()) {
            const auto line = QString::fromUtf8(header.readLine());
            const auto separator = line.indexOf('=');
            if (separator < 0 || line.left(separator).trimmed() != "ElementDataFile") continue;
            const auto payload = QFileInfo(info.dir().filePath(line.mid(separator + 1).trimmed())).canonicalFilePath();
            if (!payload.isEmpty()) metaImagePayloads.insert(payload);
            break;
        }
    }
    QStringList pending;
    QSet<QString> explicitRaw;
    for (const auto &path : paths) {
        if (path.isEmpty()) continue;
        const QFileInfo info(path);
        if (metaImagePayloads.contains(info.canonicalFilePath())) continue;
        const auto suffix = info.suffix().toLower();
        // Keep explicit raw layout dialogs immediate, including cancellation
        // followed by other files in the same selection.
        if (!info.isDir() && (suffix == "raw" || suffix == "bin")) {
            const auto key = info.exists() ? info.canonicalFilePath() : info.absoluteFilePath();
            if (!explicitRaw.contains(key)) { explicitRaw.insert(key); openFiles({path}, activation); }
        } else pending.append(path);
    }
    if (pending.isEmpty()) return;
    auto plan = std::make_shared<ImageImportPlan>();
    jobs_.submit(tr("Scanning images"), [pending, plan](auto stop, auto progress) {
        *plan = discoverImageImports(pending, stop, progress);
        try { checkDicomImportMemory(plan->images, detail::availableMemoryBytes()); }
        catch (const std::exception &error) {
            plan->errors.append(QString::fromUtf8(error.what()));
            plan->images.removeIf([](const auto &image) { return image.dicomBytes || image.paths.size() > 1; });
        }
    }, [this, plan, explicitRaw, activation = std::move(activation)](auto error) mutable {
        auto guard = std::move(activation);
        if (error) { guard.reset(); showJobError(error); return; }
        for (const auto &image : plan->images) {
            if (image.paths.size() == 1) {
                if (explicitRaw.isEmpty() || !explicitRaw.contains(QFileInfo(image.paths.front()).canonicalFilePath()))
                    openFiles(image.paths, guard);
                continue;
            }
            auto output = std::make_shared<Image>();
            const auto paths = image.paths;
            jobs_.submit(tr("Reading DICOM %1").arg(QFileInfo(paths.front()).fileName()),
                [paths, output](auto stop, auto progress) {
                    *output = readDicomSeries(paths, stop, progress);
                }, [this, paths, output, activation = guard](auto error) mutable {
                    auto dicomGuard = std::move(activation);
                    if (error) {
                        dicomGuard.reset();
                        try { std::rethrow_exception(error); }
                        catch (const std::exception &exception) {
                            showError(tr("Could not import DICOM %1:\n%2").arg(paths.front(), QString::fromUtf8(exception.what())));
                        }
                        catch (...) { showJobError(error); }
                        return;
                    }
                    addDocument(*output, {}, std::nullopt, std::move(dicomGuard));
                });
        }
        guard.reset();
        if (!plan->errors.isEmpty()) showError(plan->errors.join('\n'));
    });
}

void MainWindow::openFiles(const QStringList &paths, std::shared_ptr<PendingActivation> activation)
{
    for (const auto &path : paths) {
        if (path.isEmpty()) continue;
        if ((QFileInfo(path).suffix().compare("raw", Qt::CaseInsensitive) == 0 ||
             QFileInfo(path).suffix().compare("bin", Qt::CaseInsensitive) == 0) &&
            !QFileInfo::exists(path + ".json")) {
            importRaw(path, activation);
        } else {
            auto output = std::make_shared<Image>();
            jobs_.submit(tr("Opening %1").arg(QFileInfo(path).fileName()),
                [path, output](auto stop, auto progress) { *output = readImage(path, {}, stop, progress); },
                [this, path, output, activation](auto error) mutable {
                    auto guard = std::move(activation);
                    if (error) { guard.reset(); showJobError(error); return; }
                    addDocument(*output, {}, std::nullopt, std::move(guard));
                    recentFiles_.removeAll(path);
                    recentFiles_.prepend(path);
                    while (recentFiles_.size() > 12) recentFiles_.removeLast();
                    updateRecentFiles();
                });
        }
    }
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Show) {
        auto *window = qobject_cast<QWidget *>(watched);
        if (window && (window == this || panels_.values().contains(window))) {
            keepWindowOnScreen(window);
            QTimer::singleShot(0, window, [window] { keepWindowOnScreen(window); });
        }
    }
    const bool measurementWindow = watched == panel("measurements") || watched == panel("multiMeasurements")
        || watched == panel("multiMeasurementsPerSlice");
    if (event->type() == QEvent::Close && measurementWindow && !confirmDiscardMeasurements(qobject_cast<QWidget *>(watched))) {
        event->ignore();
        return true;
    }
    if (event->type() != QEvent::ShortcutOverride && event->type() != QEvent::KeyPress)
        return QMainWindow::eventFilter(watched, event);
    auto *widget = qobject_cast<QWidget *>(watched);
    if (!widget) return false;
    auto *key = static_cast<QKeyEvent *>(event);
    if (metadata_ && (widget == metadata_ || metadata_->isAncestorOf(widget)) && key->matches(QKeySequence::Copy)) {
        if (event->type() == QEvent::KeyPress && !metadata_->selectedItems().isEmpty())
            QApplication::clipboard()->setText(propertyClipboardText(metadata_, -1));
        key->accept();
        return true;
    }
    auto *top = widget->window();
    const auto docs = documents();
    DocumentWidget *imageDocument = nullptr;
    for (auto *document : docs)
        for (auto *view : document->views())
            if (view->window() == top) imageDocument = document;
    OverlayWindow *overlayWindow = nullptr;
    for (auto *overlay : findChildren<OverlayWindow *>(QString{}, Qt::FindDirectChildrenOnly))
        for (auto *view : overlay->views())
            if (view->window() == top) overlayWindow = overlay;
    if (top != this && !isAncestorOf(top) && !imageDocument && !overlayWindow) return false;
    if (overlayWindow && event->type() == QEvent::KeyPress) setActiveOverlay(overlayWindow);
    else if (imageDocument && event->type() == QEvent::KeyPress) setActiveDocument(imageDocument);
    if ((top == panel("measurements") || top == panel("multiMeasurements") || top == panel("multiMeasurementsPerSlice"))
        && key->key() == Qt::Key_Escape && !QApplication::activeModalWidget()) {
        if (event->type() == QEvent::KeyPress) top->close();
        key->accept();
        return true;
    }
    const QKeySequence sequence(key->keyCombination());
    auto *action = shortcutActions_.value(sequence);
    bool editing = QApplication::activeModalWidget() != nullptr;
    for (auto *parent = widget; parent && !editing; parent = parent->parentWidget()) {
        editing = qobject_cast<QLineEdit *>(parent) || qobject_cast<QTextEdit *>(parent)
            || qobject_cast<QPlainTextEdit *>(parent) || qobject_cast<QAbstractSpinBox *>(parent)
            || qobject_cast<QComboBox *>(parent);
    }
    const bool canvas = qobject_cast<ImageViewer *>(widget) != nullptr;
    const bool selectionCanvas = canvas && !overlayWindow && activeDocument()
        && (widget == activeDocument()->viewer || !activeDocument()->orthogonalViews());
    // Keep Tab/Enter and arrow navigation local to controls and ROI drafts.
    if ((key->key() == Qt::Key_Tab || key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
        && !canvas && top != this) return false;
    if (editing) {
        if (action && event->type() == QEvent::ShortcutOverride) { key->accept(); return true; }
        return false;
    }
    if (canvas && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
        && activeDocument() && activeDocument()->viewer->tool() == ImageViewer::Tool::Polygon) return false;
    if (selectionCanvas && key->key() >= Qt::Key_Left && key->key() <= Qt::Key_Down
        && !(key->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))) {
        if (auto roi = selectedRoi()) {
            if (event->type() == QEvent::ShortcutOverride) { key->accept(); return true; }
            const QPointF delta(key->key() == Qt::Key_Left ? -1 : key->key() == Qt::Key_Right ? 1 : 0,
                                key->key() == Qt::Key_Up ? -1 : key->key() == Qt::Key_Down ? 1 : 0);
            if (key->modifiers().testFlag(Qt::AltModifier) && (roi->type == RoiType::Rectangle || roi->type == RoiType::Ellipse)) {
                const auto end = roi->points[1] + delta;
                if (end.x() > roi->points[0].x() && end.y() > roi->points[0].y()) roi->points[1] = end;
            } else {
                for (auto &point : roi->points) point += delta;
                if (roi->type == RoiType::Composite) roi->region.translate(delta);
            }
            activeDocument()->editRoi(*roi);
            return true;
        }
    }
    if (action) {
        if (event->type() == QEvent::ShortcutOverride) { key->accept(); return true; }
        if (action->isEnabled() && !key->isAutoRepeat()) action->trigger();
        return true;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::executeCommand(const QString &id)
{
    try {
        const auto &command = commands_.command(id);
        if (command.needsImage && !activeDocument()) return;
        const auto parameters = command.parameters.empty() ? std::optional<QVariantMap>(QVariantMap{}) : parameterForm(command);
        if (parameters) {
            commands_.execute(id, *parameters);
            if ((id.startsWith("process.") && id != "process.repeat" && id != "process.batch") || id == "edit.smooth" || id == "edit.invert") {
                repeatCommand_ = id;
                repeatParameters_ = *parameters;
            }
        }
    } catch (const std::exception &error) { showError(QString::fromUtf8(error.what())); }
}

std::optional<QVariantMap> MainWindow::parameterForm(const Command &command, bool batch)
{
    QDialog dialog(this);
    dialog.setWindowTitle(command.label);
    dialog.setMinimumWidth(380);
    auto *layout = new QFormLayout(&dialog);
    QMap<QString, QWidget *> editors;
    for (const auto &parameter : command.parameters) {
        if (batch && parameter.id == "scope") continue;
        QWidget *editor = nullptr;
        if (parameter.kind == ParameterKind::Real) {
            auto *spin = new QDoubleSpinBox(&dialog);
            spin->setDecimals(6);
            spin->setRange(parameter.minimum, parameter.maximum);
            spin->setValue(parameter.defaultValue.toDouble());
            if (const auto *doc = activeDocument()) {
                if (parameter.id == "lower") spin->setValue(doc->viewer->windowMinimum());
                if (parameter.id == "upper") spin->setValue(doc->viewer->windowMaximum());
                if (parameter.id.startsWith("spacing")) {
                    const int axis = parameter.id.endsWith('X') ? 0 : parameter.id.endsWith('Y') ? 1 : 2;
                    if (axis < int(doc->image.metadata().axes.size())) spin->setValue(doc->image.metadata().axes[axis].spacing);
                }
            }
            editor = spin;
        } else if (parameter.kind == ParameterKind::Integer) {
            auto *spin = new QSpinBox(&dialog);
            spin->setRange(int(parameter.minimum), int(parameter.maximum));
            spin->setValue(parameter.defaultValue.toInt());
            editor = spin;
        } else if (parameter.kind == ParameterKind::Boolean) {
            auto *check = new QCheckBox(&dialog);
            check->setChecked(parameter.defaultValue.toBool());
            editor = check;
        } else {
            auto *combo = new QComboBox(&dialog);
            for (const auto &choice : parameter.choices)
                combo->addItem(QCoreApplication::translate("CommandChoices", choice.toUtf8().constData()), choice);
            combo->setCurrentIndex(combo->findData(parameter.defaultValue));
            editor = combo;
        }
        if (command.id == "gel.options") if (const auto *doc = activeDocument()) {
            if (auto *combo = qobject_cast<QComboBox *>(editor)) combo->setCurrentIndex(combo->findData(doc->gelHorizontal ? "Horizontal" : "Vertical"));
            if (auto *check = qobject_cast<QCheckBox *>(editor)) check->setChecked(doc->gelDarkBands);
        }
        editor->setObjectName(parameter.id);
        editors.insert(parameter.id, editor);
        layout->addRow(parameter.label, editor);
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return {};
    QVariantMap result;
    for (const auto &parameter : command.parameters) {
        auto *editor = editors.value(parameter.id);
        if (!editor) result.insert(parameter.id, parameter.defaultValue);
        else if (auto *spin = qobject_cast<QDoubleSpinBox *>(editor)) result.insert(parameter.id, spin->value());
        else if (auto *spin = qobject_cast<QSpinBox *>(editor)) result.insert(parameter.id, spin->value());
        else if (auto *check = qobject_cast<QCheckBox *>(editor)) result.insert(parameter.id, check->isChecked());
        else if (auto *combo = qobject_cast<QComboBox *>(editor)) result.insert(parameter.id, combo->currentData());
    }
    return result;
}

std::optional<Roi> MainWindow::selectedRoi() const
{
    const auto *document = activeDocument();
    const auto index = rois_ && panel("rois")->isVisible() ? rois_->currentRow()
        : document && !document->rois.empty() ? int(document->rois.size() - 1) : -1;
    if (!document || document->selectionCleared) return {};
    auto roi = document->selection;
    if (!roi) {
        if (index < 0 || index >= int(document->rois.size())) return {};
        roi = document->rois[std::size_t(index)];
    }
    if (roi->channel != document->viewer->channel() || roi->time != document->viewer->time()) return {};
    // Use the same geometry on the current slice while retaining its original saved position.
    roi->z = document->viewer->slice();
    return roi;
}

bool MainWindow::showJobError(std::exception_ptr error)
{
    if (!error) return false;
    try { std::rethrow_exception(error); }
    catch (const std::exception &exception) { showError(QString::fromUtf8(exception.what())); }
    catch (...) { showError(tr("The background job failed with an unknown error.")); }
    return true;
}

void MainWindow::showError(const QString &message)
{
    QMessageBox::warning(this, tr("ImageCXX"), message);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    QStringList unsavedImages;
    for (const auto *doc : documents()) if (doc->modified)
        unsavedImages.append(doc->image.metadata().name.isEmpty() ? tr("Untitled") : doc->image.metadata().name);
    if (!unsavedImages.isEmpty()) {
        QMessageBox prompt(QMessageBox::Question, tr("Close ImageCXX"),
            tr("These images have unsaved pixel changes:\n\n%1\n\nClose without saving them?").arg(unsavedImages.join('\n')),
            QMessageBox::Discard | QMessageBox::Cancel, this);
        prompt.setTextFormat(Qt::PlainText);
        prompt.setDefaultButton(QMessageBox::Cancel);
        if (prompt.exec() != QMessageBox::Discard) { event->ignore(); return; }
    }
    if (!confirmDiscardMeasurements()) { event->ignore(); return; }
    if (jobs_.busy() && QMessageBox::question(this, tr("Background jobs running"), tr("Cancel running jobs and close ImageCXX?"),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) { event->ignore(); return; }
    jobs_.cancelAll();
    QSettings settings;
    settings.setValue("recentFiles", recentFiles_);
    settings.beginGroup("windowWorkflow/v1");
    settings.setValue("launcherGeometry", saveGeometry());
    settings.setValue("panels", panelLayout());
    clearDocuments();
    for (auto *window : panels_) window->hide();
    event->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    for (const auto &url : event->mimeData()->urls())
        if (url.isLocalFile()) { event->acceptProposedAction(); return; }
}

void MainWindow::dropEvent(QDropEvent *event)
{
    QStringList paths;
    for (const auto &url : event->mimeData()->urls()) if (url.isLocalFile()) paths.append(url.toLocalFile());
    if (!paths.isEmpty()) {
        if (isVisible()) activateApplicationWindow(this);
        openPaths(paths);
        event->acceptProposedAction();
    }
}
