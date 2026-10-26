#include "app/update_checker.h"
#include "app/update_release.h"
#include "mainwindow.h"
#include <QApplication>
#include <QAction>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QNetworkReply>
#include <QProgressDialog>
#include <QScopeGuard>
#include <QSettings>
#include <QTimer>
#include <chrono>

namespace imagecxx {
UpdateChecker::UpdateChecker(MainWindow &window) : QObject(&window)
{
#ifdef Q_OS_MACOS
    const auto check = [this, &window](bool manual) { checkWithSparkle(window, manual); };
#else
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    const auto check = [this, &window, timer](bool manual) {
        if (checking_ || progress_ || !pendingProgram.isEmpty()) {
            if (!manual) timer->start(std::chrono::minutes(1));
            return;
        }
        constexpr qint64 interval = 24LL * 60 * 60 * 1000;
        QSettings settings;
        const auto now = QDateTime::currentDateTimeUtc();
        const auto lastCheck = settings.value("updates/lastCheck").toDateTime();
        const auto elapsed = lastCheck.msecsTo(now);
        if (!manual && lastCheck.isValid() && elapsed >= 0 && elapsed <= interval) {
            timer->start(std::chrono::milliseconds(interval - elapsed + 1));
            return;
        }
        settings.setValue("updates/lastCheck", now);
        timer->start(std::chrono::milliseconds(interval + 1));
        checking_ = true;
        QNetworkRequest request(QUrl("https://api.github.com/repos/ryanchen01/ImageCXX/releases/latest"));
        request.setRawHeader("Accept", "application/vnd.github+json");
        request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
        request.setHeader(QNetworkRequest::UserAgentHeader, "ImageCXX/" + QCoreApplication::applicationVersion());
        request.setTransferTimeout(15000);
        auto *reply = network_.get(request);
        connect(reply, &QNetworkReply::readyRead, this, [reply] {
            if (reply->bytesAvailable() > 4 * 1024 * 1024) reply->abort();
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply, &window, manual] {
            const auto resetChecking = qScopeGuard([this] { checking_ = false; });
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                if (manual) QMessageBox::warning(&window, tr("Check for Updates"),
                    tr("Cannot check for updates. Please try again later."));
                return;
            }
#if defined(Q_OS_WIN)
            const QString suffix = "-windows-x86_64.exe";
#else
            const QString suffix = "-linux-x86_64.AppImage";
#endif
            const auto release = releaseUpdate(reply->readAll(), QCoreApplication::applicationVersion(), suffix);
            if (!window.isVisible()) return;
            if (!release) {
                if (manual) QMessageBox::information(&window, tr("Check for Updates"), tr("No updates are available."));
                return;
            }
#ifdef Q_OS_LINUX
            const auto appImage = qEnvironmentVariable("APPIMAGE");
            if (appImage.isEmpty()) {
                if (manual) QMessageBox::information(&window, tr("Check for Updates"),
                    tr("ImageCXX %1 is available. Install the latest AppImage from the GitHub releases page to update.").arg(release->version));
                return;
            }
#endif
            if (QMessageBox::question(&window, tr("Update ImageCXX"),
                tr("ImageCXX %1 is available. Download and install it?").arg(release->version),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
            auto *progress = new QProgressDialog(tr("Downloading update..."), tr("Cancel"), 0, 0, &window);
            progress_ = progress;
            progress->setWindowModality(Qt::WindowModal);
            progress->setAutoClose(false);
            progress->show();
            const auto complete = [this, &window, progress, release](const QString &path, const QString &error) {
                const bool canceled = progress->wasCanceled();
                progress->close(); progress->deleteLater();
                if (canceled) { directory_.reset(); return; }
                if (!error.isEmpty() || !verifyUpdate(path, *release)) {
                    QMessageBox::warning(&window, tr("Update failed"), error.isEmpty()
                        ? tr("The downloaded update failed verification. Your installation has not changed.") : error);
                    directory_.reset(); return;
                }
#ifdef Q_OS_WIN
                pendingProgram = path;
                pendingArguments = {"ImageCXXInstallDir=" + QDir::cleanPath(QCoreApplication::applicationDirPath() + "/..")};
                if (!window.close()) pendingProgram.clear();
                else directory_->setAutoRemove(false);
#else
                // Renames stay on the same filesystem; keep the old AppImage for rollback.
                const auto original = qEnvironmentVariable("APPIMAGE");
                const auto backup = original + ".zs-old";
                if (!QFile::setPermissions(path, QFile::permissions(original))) {
                    QMessageBox::warning(&window, tr("Update failed"), tr("Cannot set the updated AppImage's permissions."));
                    directory_.reset(); return;
                }
                const bool quitOnClose = QApplication::quitOnLastWindowClosed();
                QApplication::setQuitOnLastWindowClosed(false);
                const bool closed = window.close();
                QApplication::setQuitOnLastWindowClosed(quitOnClose);
                if (!closed) { directory_.reset(); return; }
                if ((QFile::exists(backup) && !QFile::remove(backup)) || !QFile::rename(original, backup)) {
                    window.show();
                    QMessageBox::warning(&window, tr("Update failed"), tr("Cannot back up the AppImage. Check the directory permissions."));
                    directory_.reset(); return;
                }
                if (!QFile::rename(path, original)) {
                    const bool restored = QFile::rename(backup, original);
                    window.show();
                    QMessageBox::warning(&window, tr("Update failed"), restored ? tr("Cannot replace the AppImage. The original was restored.")
                        : tr("Cannot restore the AppImage. Your previous version is at %1.").arg(backup));
                    directory_.reset(); return;
                }
                pendingProgram = original;
                QCoreApplication::quit();
#endif
            };
#ifdef Q_OS_WIN
            directory_ = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/ImageCXX-update-XXXXXX");
            const auto path = directory_->filePath("ImageCXX-Setup.exe");
            auto file = std::make_shared<QFile>(path);
            if (!directory_->isValid() || !file->open(QIODevice::WriteOnly)) {
                complete(path, tr("Cannot create the update download file.")); return;
            }
            QNetworkRequest download(release->url);
            download.setTransferTimeout(60000);
            auto *transfer = network_.get(download);
            connect(progress, &QProgressDialog::canceled, transfer, &QNetworkReply::abort);
            connect(transfer, &QNetworkReply::readyRead, this, [transfer, file, release] {
                const auto data = transfer->readAll();
                if (file->size() + data.size() > release->size || file->write(data) != data.size()) transfer->abort();
            });
            connect(transfer, &QNetworkReply::downloadProgress, progress, [progress](qint64 received, qint64 total) {
                if (total > 0) { progress->setRange(0, 100); progress->setValue(int(100.0 * received / total)); }
            });
            connect(transfer, &QNetworkReply::finished, this, [transfer, file, path, complete] {
                const bool flushed = file->flush(); file->close();
                complete(path, transfer->error() == QNetworkReply::NoError && flushed ? QString{} : tr("The update download failed. Please try again later."));
                transfer->deleteLater();
            });
#else
            directory_ = std::make_unique<QTemporaryDir>(QFileInfo(appImage).absolutePath() + "/.imagecxx-update-XXXXXX");
            const auto path = directory_->filePath("ImageCXX.AppImage");
            if (!directory_->isValid() || !QFile::copy(appImage, path)) {
                complete(path, tr("Cannot stage the update next to the AppImage. Check free space and directory permissions.")); return;
            }
            auto environment = QProcessEnvironment::systemEnvironment();
            environment.insert("APPIMAGE_EXTRACT_AND_RUN", "1");
            for (const auto &key : {"LD_LIBRARY_PATH", "QT_PLUGIN_PATH", "QML2_IMPORT_PATH", "QML_IMPORT_PATH"}) environment.remove(key);
            process_.setProcessEnvironment(environment);
            process_.setProgram(QCoreApplication::applicationDirPath() + "/../libexec/appimageupdatetool.AppImage");
            process_.setArguments({"--overwrite", "--update-info", "zsync|" + release->url.toString() + ".zsync", path});
            process_.disconnect(this);
            connect(progress, &QProgressDialog::canceled, &process_, &QProcess::kill);
            connect(&process_, &QProcess::readyReadStandardOutput, this, [this] { process_.readAllStandardOutput(); });
            connect(&process_, &QProcess::readyReadStandardError, this, [this] { process_.readAllStandardError(); });
            connect(&process_, &QProcess::errorOccurred, this, [complete, path](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart) complete(path, tr("Cannot start the bundled AppImageUpdate tool."));
            });
            connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [complete, path](int code, QProcess::ExitStatus status) {
                    complete(path, code == 0 && status == QProcess::NormalExit ? QString{} : tr("AppImageUpdate could not download the update."));
                });
            process_.start();
#endif
        });
    };
    connect(timer, &QTimer::timeout, this, [check] { check(false); });
#endif
    auto *action = window.findChild<QAction *>("help.checkUpdates");
    action->setEnabled(true);
    connect(action, &QAction::triggered, this, [check] { check(true); });
    QTimer::singleShot(0, this, [check] { check(false); });
}
}
