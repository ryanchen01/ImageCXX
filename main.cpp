#include "mainwindow.h"
#include "app/file_open_service.h"
#include "app/batch.h"
#include "app/processing_commands.h"
#include "ui/theme.h"
#ifdef IMAGECXX_ENABLE_UPDATES
#include "app/update_checker.h"
#include <QMessageBox>
#include <QProcess>
#endif
#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QIcon>
#include <QJsonDocument>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>
#include <cstdio>
#include <memory>

int main(int argc, char *argv[])
{
    // Capture launcher activation metadata before QApplication strips DESKTOP_STARTUP_ID on X11.
    const QString activationToken = qEnvironmentVariable("XDG_ACTIVATION_TOKEN");
    const QString startupId = qEnvironmentVariable("DESKTOP_STARTUP_ID");
    bool batch = false;
    for (int i = 1; i < argc; ++i) if (QByteArray(argv[i]) == "--batch") batch = true;
    std::unique_ptr<QCoreApplication> application;
    if (batch) application = std::make_unique<QCoreApplication>(argc, argv);
    else application = std::make_unique<QApplication>(argc, argv);
    QCoreApplication::setApplicationName("ImageCXX");
    QCoreApplication::setOrganizationName("ImageCXX");
    QCoreApplication::setApplicationVersion(IMAGECXX_VERSION);
    const QLocale locale;
    QTranslator qtTranslator;
    if (qtTranslator.load(locale, "qtbase", "_", QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        application->installTranslator(&qtTranslator);
    QTranslator translator;
    if (translator.load(locale, "ImageCXX", "_", ":/i18n"))
        application->installTranslator(&translator);
    QCommandLineParser parser;
    parser.setApplicationDescription(QCoreApplication::translate("Main", "ImageCXX scientific image analysis"));
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOption({"batch", QCoreApplication::translate("Main", "Process files without a desktop window")});
    parser.addOption({"operation", QCoreApplication::translate("Main", "Batch operation: gaussian, median, threshold, dilate, erode, resample"), QCoreApplication::translate("Main", "operation"), "gaussian"});
    parser.addOption({"output", QCoreApplication::translate("Main", "Batch output directory. Existing files are never overwritten."), QCoreApplication::translate("Main", "directory")});
    parser.addOption({"parameters", QCoreApplication::translate("Main", "Command parameter object as JSON"), QCoreApplication::translate("Main", "json"), "{}"});
    parser.addPositionalArgument(QCoreApplication::translate("Main", "images"), QCoreApplication::translate("Main", "Image files"), QCoreApplication::translate("Main", "[images...]"));
    parser.process(*application);
    if (batch) {
        if (parser.value("output").isEmpty() || parser.positionalArguments().isEmpty()) parser.showHelp(2);
        try {
            QJsonParseError error;
            const auto json = QJsonDocument::fromJson(parser.value("parameters").toUtf8(), &error);
            if (error.error != QJsonParseError::NoError || !json.isObject()) throw std::invalid_argument(QCoreApplication::translate("Main", "Parameters must be a JSON object").toStdString());
            const auto result = imagecxx::runBatch(parser.positionalArguments(), parser.value("output"), "process." + parser.value("operation"), json.toVariant().toMap());
            for (const auto &path : result.outputs) std::fprintf(stdout, "%s\n", qPrintable(path));
            for (const auto &message : result.errors) std::fprintf(stderr, "%s\n", qPrintable(message));
            return result.errors.empty() ? 0 : 1;
        } catch (const std::exception &error) { std::fprintf(stderr, "%s\n", error.what()); return 2; }
    }
    imagecxx::FileOpenService fileOpens;
    try {
        if (!fileOpens.start(parser.positionalArguments(), activationToken, startupId)) return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    imagecxx::applyTheme();
    QApplication::setWindowIcon(QIcon(":/icons/imagecxx.svg"));
    const bool wayland = QGuiApplication::platformName().startsWith(QStringLiteral("wayland"));
    const bool deferWaylandActivation = wayland && !parser.positionalArguments().isEmpty()
        && (!activationToken.isEmpty() || !startupId.isEmpty());
    if (deferWaylandActivation) qunsetenv("XDG_ACTIVATION_TOKEN");
    MainWindow window;
    if (deferWaylandActivation) window.setAttribute(Qt::WA_ShowWithoutActivating);
    window.show();
    if (deferWaylandActivation) window.setAttribute(Qt::WA_ShowWithoutActivating, false);
    QObject::connect(&fileOpens, &imagecxx::FileOpenService::openRequested, &window,
        [&window, wayland](const QStringList &paths, const QString &token, const QString &openStartupId) {
            const QString waylandToken = !token.isEmpty() ? token : openStartupId;
            if (wayland && !paths.isEmpty() && !waylandToken.isEmpty()) {
                if (window.isMinimized()) window.showNormal();
                else if (!window.isVisible()) window.show();
                window.openPaths(paths, waylandToken);
                return;
            }
            imagecxx::activateApplicationWindow(&window, token, openStartupId);
            window.openPaths(paths);
        });
#ifdef IMAGECXX_ENABLE_UPDATES
    imagecxx::UpdateChecker updates(window);
    const int result = application->exec();
    if (!updates.pendingProgram.isEmpty() && !QProcess::startDetached(updates.pendingProgram, updates.pendingArguments)) {
        QMessageBox::critical(nullptr, QObject::tr("Update failed"),
            QObject::tr("Cannot launch %1. Please run it manually.").arg(updates.pendingProgram));
        return 1;
    }
    return result;
#else
    return application->exec();
#endif
}
