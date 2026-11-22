#include "app/file_open_service.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QStandardPaths>
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
    QString receivedToken;
    QString receivedStartupId;
    QObject::connect(&service, &imagecxx::FileOpenService::openRequested,
        &service, [&](const QStringList &paths, const QString &token, const QString &startupId) {
            received = paths;
            receivedToken = token;
            receivedStartupId = startupId;
        });
    QTemporaryDir workingDirectory;
    ASSERT_TRUE(workingDirectory.isValid());
    // A child process reports its physical cwd, including macOS's /var -> /private/var alias.
    const QDir resolvedWorkingDirectory(QFileInfo(workingDirectory.path()).canonicalFilePath());
    QProcess client;
    client.setWorkingDirectory(resolvedWorkingDirectory.path());
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("XDG_ACTIVATION_TOKEN", "test-wayland-token");
    environment.insert("DESKTOP_STARTUP_ID", "launcher-123_TIME987654");
    client.setProcessEnvironment(environment);
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
    EXPECT_EQ(received, (QStringList{resolvedWorkingDirectory.filePath(paths[0]), resolvedWorkingDirectory.filePath(paths[1])}));
    EXPECT_EQ(receivedToken, "test-wayland-token");
    EXPECT_EQ(receivedStartupId, "launcher-123_TIME987654");
}

TEST(FileOpening, FallsBackToLegacyArrayForPreUpdateServer)
{
    const QString serverName = "ImageCXX-" + QString::fromLatin1(QCryptographicHash::hash(
        (QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
            + QCoreApplication::applicationName()).toUtf8(),
        QCryptographicHash::Sha256).toHex().left(24));
    QLocalServer::removeServer(serverName);
    QLocalServer legacyServer;
    legacyServer.setSocketOptions(QLocalServer::UserAccessOption);
    ASSERT_TRUE(legacyServer.listen(serverName));
    int rejectedNonArray = 0;
    QStringList received;
    QObject::connect(&legacyServer, &QLocalServer::newConnection, &legacyServer, [&] {
        while (auto *socket = legacyServer.nextPendingConnection()) {
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QLocalSocket::readyRead, socket, [&, socket] {
                if (!socket->canReadLine()) return;
                const auto doc = QJsonDocument::fromJson(socket->readLine());
                if (!doc.isArray()) {
                    ++rejectedNonArray;
                    socket->disconnectFromServer();
                    return;
                }
                for (const auto &value : doc.array()) received.append(value.toString());
                socket->write("OK\n");
            });
        }
    });
    QTemporaryDir workingDirectory;
    ASSERT_TRUE(workingDirectory.isValid());
    const QDir resolvedWorkingDirectory(QFileInfo(workingDirectory.path()).canonicalFilePath());
    QProcess client;
    client.setWorkingDirectory(resolvedWorkingDirectory.path());
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("XDG_ACTIVATION_TOKEN", "legacy-fallback-token");
    environment.insert("DESKTOP_STARTUP_ID", "legacy-fallback_TIME42");
    client.setProcessEnvironment(environment);
    QEventLoop loop;
    QObject::connect(&client, &QProcess::finished, &loop, &QEventLoop::quit);
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(10000);
    client.start(QCoreApplication::applicationFilePath(), QStringList{"--forward", "legacy.png"});
    loop.exec();
    ASSERT_EQ(client.state(), QProcess::NotRunning);
    EXPECT_EQ(client.exitCode(), 0) << client.readAllStandardError().constData();
    EXPECT_EQ(rejectedNonArray, 1);
    EXPECT_EQ(received, (QStringList{resolvedWorkingDirectory.filePath("legacy.png")}));
}

int main(int argc, char **argv)
{
    const QString activationToken = qEnvironmentVariable("XDG_ACTIVATION_TOKEN");
    const QString startupId = qEnvironmentVariable("DESKTOP_STARTUP_ID");
    QApplication application(argc, argv);
    if (application.arguments().value(1) == "--forward") {
        imagecxx::FileOpenService service;
        try { return service.start(application.arguments().mid(2), activationToken, startupId) ? 2 : 0; }
        catch (const std::exception &error) { qCritical("%s", error.what()); return 3; }
    }
    QTemporaryDir config;
    if (!config.isValid()) return 1;
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
