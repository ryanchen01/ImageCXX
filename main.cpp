#include "mainwindow.h"
#include "app/batch.h"
#include "app/processing_commands.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QPalette>
#include <QStyleFactory>
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
    QCoreApplication::setApplicationVersion("0.2.0");
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
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    QPalette palette;
    const QColor background("#FAF7F2");
    const QColor foreground("#1A1C1F");
    palette.setColor(QPalette::Window, background);
    palette.setColor(QPalette::WindowText, foreground);
    palette.setColor(QPalette::Base, background);
    palette.setColor(QPalette::AlternateBase, QColor("#F0ECE6"));
    palette.setColor(QPalette::Text, foreground);
    palette.setColor(QPalette::Button, background);
    palette.setColor(QPalette::ButtonText, foreground);
    palette.setColor(QPalette::ToolTipBase, background);
    palette.setColor(QPalette::ToolTipText, foreground);
    palette.setColor(QPalette::Highlight, QColor("#DED8CF"));
    palette.setColor(QPalette::HighlightedText, foreground);
    palette.setColor(QPalette::Link, foreground);
    palette.setColor(QPalette::Accent, foreground);
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, role, QColor("#8B8883"));
    QApplication::setPalette(palette);
    MainWindow window;
    window.show();
    window.openPaths(parser.positionalArguments());
    return application->exec();
}
