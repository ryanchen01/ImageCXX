#include "app/file_open_service.h"
#include <QApplication>
#include <QEventLoop>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <gtest/gtest.h>

TEST(FileOpening, StartupAndNativeEvents)
{
    imagecxx::FileOpenService service;
    QList<QStringList> requests;
    QObject::connect(&service, &imagecxx::FileOpenService::openRequested,
        &service, [&](const QStringList &paths) { requests.append(paths); });
    ASSERT_TRUE(service.start({"first image.png", "stack.nii.gz"}));
    QCoreApplication::processEvents();
    ASSERT_EQ(requests.size(), 1);
    EXPECT_EQ(requests[0], (QStringList{QFileInfo("first image.png").absoluteFilePath(),
        QFileInfo("stack.nii.gz").absoluteFilePath()}));
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto nativePath = directory.filePath(QString::fromUtf8("图像 with spaces.tif"));
    QFileOpenEvent event(nativePath);
    EXPECT_TRUE(QCoreApplication::sendEvent(qApp, &event));
    QCoreApplication::processEvents();
    ASSERT_EQ(requests.size(), 2);
    EXPECT_EQ(requests[1], (QStringList{nativePath}));
}

TEST(FileOpening, SecondProcessForwardsAndExits)
{
    imagecxx::FileOpenService service;
    ASSERT_TRUE(service.start({}));
    QCoreApplication::processEvents();
    QStringList received;
    QObject::connect(&service, &imagecxx::FileOpenService::openRequested,
        &service, [&](const QStringList &paths) { received = paths; });
    QTemporaryDir workingDirectory;
    ASSERT_TRUE(workingDirectory.isValid());
    QProcess client;
    client.setWorkingDirectory(workingDirectory.path());
    QEventLoop loop;
    QObject::connect(&client, &QProcess::finished, &loop, &QEventLoop::quit);
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(10000);
    const QStringList paths{QString::fromUtf8("relative 图像.png"), "-image.nii.gz"};
    client.start(QCoreApplication::applicationFilePath(), QStringList{"--forward"} + paths);
    loop.exec();
    ASSERT_EQ(client.state(), QProcess::NotRunning);
    EXPECT_EQ(client.exitCode(), 0) << client.readAllStandardError().constData();
    EXPECT_EQ(received, (QStringList{workingDirectory.filePath(paths[0]), workingDirectory.filePath(paths[1])}));
}

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    if (application.arguments().value(1) == "--forward") {
        imagecxx::FileOpenService service;
        try { return service.start(application.arguments().mid(2)) ? 2 : 0; }
        catch (const std::exception &error) { qCritical("%s", error.what()); return 3; }
    }
    QTemporaryDir config;
    if (!config.isValid()) return 1;
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
