#include "mainwindow.h"
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
#include <cstdio>
#include <memory>

int main(int argc, char *argv[])
{
    bool batch = false;
    for (int i = 1; i < argc; ++i) if (QByteArray(argv[i]) == "--batch") batch = true;
    std::unique_ptr<QCoreApplication> application;
    if (batch) application = std::make_unique<QCoreApplication>(argc, argv);
    else application = std::make_unique<QApplication>(argc, argv);
    QCoreApplication::setApplicationName("ImageCXX");
    QCoreApplication::setOrganizationName("ImageCXX");
    QCoreApplication::setApplicationVersion(IMAGECXX_VERSION);
    QCommandLineParser parser;
    parser.setApplicationDescription("ImageCXX scientific image analysis");
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOption({"batch", "Process files without a desktop window"});
    parser.addOption({"operation", "Batch operation: gaussian, median, threshold, dilate, erode, resample", "operation", "gaussian"});
    parser.addOption({"output", "Batch output directory. Existing files are never overwritten.", "directory"});
    parser.addOption({"parameters", "Command parameter object as JSON", "json", "{}"});
    parser.addPositionalArgument("images", "Image files", "[images...]");
    parser.process(*application);
    if (batch) {
        if (parser.value("output").isEmpty() || parser.positionalArguments().isEmpty()) parser.showHelp(2);
        try {
            QJsonParseError error;
            const auto json = QJsonDocument::fromJson(parser.value("parameters").toUtf8(), &error);
            if (error.error != QJsonParseError::NoError || !json.isObject()) throw std::invalid_argument("Parameters must be a JSON object");
            const auto result = imagecxx::runBatch(parser.positionalArguments(), parser.value("output"), "process." + parser.value("operation"), json.toVariant().toMap());
            for (const auto &path : result.outputs) std::fprintf(stdout, "%s\n", qPrintable(path));
            for (const auto &message : result.errors) std::fprintf(stderr, "%s\n", qPrintable(message));
            return result.errors.empty() ? 0 : 1;
        } catch (const std::exception &error) { std::fprintf(stderr, "%s\n", error.what()); return 2; }
    }
    imagecxx::applyTheme();
    QApplication::setWindowIcon(QIcon(":/icons/imagecxx.svg"));
    MainWindow window;
    window.show();
    window.openPaths(parser.positionalArguments());
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
