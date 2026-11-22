#include "app/file_open_service.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QTimer>
#include <stdexcept>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace imagecxx {

#ifdef Q_OS_MACOS
void yieldActivationToRunningInstance();
#endif

FileOpenService::FileOpenService(QObject *parent)
    : QObject(parent),
      serverName_("ImageCXX-" + QString::fromLatin1(QCryptographicHash::hash(
          (QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
              + QCoreApplication::applicationName()).toUtf8(),
          QCryptographicHash::Sha256).toHex().left(24))),
      lock_(QDir::temp().filePath(serverName_ + ".lock"))
{
    QCoreApplication::instance()->installEventFilter(this);
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&server_, &QLocalServer::newConnection, this, [this] {
        while (auto *socket = server_.nextPendingConnection()) {
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
                if (!socket->canReadLine()) return;
                const auto document = QJsonDocument::fromJson(socket->readLine());
                QJsonArray fileArray;
                QString activationToken;
                QString startupId;
                if (document.isObject()) {
                    const auto object = document.object();
                    fileArray = object.value("files").toArray();
                    activationToken = object.value("activationToken").toString();
                    startupId = object.value("startupId").toString();
                } else if (document.isArray()) {
                    fileArray = document.array();
                } else {
                    socket->disconnectFromServer();
                    return;
                }
                QStringList paths;
                for (const auto &value : fileArray) paths.append(value.toString());
                requestOpen(paths, std::move(activationToken), std::move(startupId));
                socket->write("OK\n");
                // Let the client close after reading the reply. Disconnecting a Windows
                // named pipe here can discard the acknowledgement before it is read.
            });
        }
    });
}

bool FileOpenService::start(const QStringList &paths, const QString &activationToken, const QString &startupId)
{
    if (!lock_.tryLock(5000))
        throw std::runtime_error(tr("Cannot initialize file opening while another ImageCXX instance is starting.").toStdString());
    const QString token = !activationToken.isEmpty() ? activationToken : qEnvironmentVariable("XDG_ACTIVATION_TOKEN");
    const QString startup = !startupId.isEmpty() ? startupId : qEnvironmentVariable("DESKTOP_STARTUP_ID");
    QJsonArray files;
    for (const auto &path : paths) files.append(QFileInfo(path).absoluteFilePath());
    const QByteArray legacyPayload = QJsonDocument(files).toJson(QJsonDocument::Compact) + '\n';
    QByteArray payload = legacyPayload;
    if (!token.isEmpty() || !startup.isEmpty()) {
        QJsonObject object{{"files", files}};
        if (!token.isEmpty()) object.insert("activationToken", token);
        if (!startup.isEmpty()) object.insert("startupId", startup);
        payload = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    }
    enum class SendResult { NotConnected, Accepted, RejectedByDisconnect, Failed };
    const auto sendRequest = [&](const QByteArray &data) -> SendResult {
        QLocalSocket socket;
        socket.connectToServer(serverName_);
        if (!socket.waitForConnected(1000)) return SendResult::NotConnected;
#ifdef Q_OS_WIN
        ULONG serverPid = 0;
        if (GetNamedPipeServerProcessId(reinterpret_cast<HANDLE>(socket.socketDescriptor()), &serverPid) && serverPid != 0)
            AllowSetForegroundWindow(serverPid);
#elif defined(Q_OS_MACOS)
        yieldActivationToRunningInstance();
#endif
        socket.write(data);
        if (!socket.waitForBytesWritten(5000)) return SendResult::Failed;
        const bool ready = socket.canReadLine() || socket.waitForReadyRead(5000);
        if (ready && socket.readLine() == "OK\n") return SendResult::Accepted;
        if (!ready && socket.bytesAvailable() == 0
            && (socket.error() == QLocalSocket::PeerClosedError || socket.state() == QLocalSocket::UnconnectedState))
            return SendResult::RejectedByDisconnect;
        return SendResult::Failed;
    };
    SendResult result = sendRequest(payload);
    if (result != SendResult::NotConnected) {
        if (result == SendResult::RejectedByDisconnect && payload != legacyPayload)
            result = sendRequest(legacyPayload);
        if (result != SendResult::Accepted)
            throw std::runtime_error(tr("The running ImageCXX instance did not accept the files.").toStdString());
        return false;
    }
    QLocalServer::removeServer(serverName_); // Remove an endpoint left by a crashed instance.
    if (!server_.listen(serverName_))
        throw std::runtime_error(tr("Cannot listen for file-open requests: %1").arg(server_.errorString()).toStdString());
    lock_.unlock();
    requestOpen(paths, paths.isEmpty() ? QString{} : token, paths.isEmpty() ? QString{} : startup);
    return true;
}

bool FileOpenService::eventFilter(QObject *object, QEvent *event)
{
    if (event->type() == QEvent::FileOpen) {
        const auto *open = static_cast<QFileOpenEvent *>(event);
        if (!open->file().isEmpty()) requestOpen({open->file()});
        else if (open->url().isLocalFile()) requestOpen({open->url().toLocalFile()});
        event->accept();
        return true;
    }
    return QObject::eventFilter(object, event);
}

void FileOpenService::requestOpen(QStringList paths, QString activationToken, QString startupId)
{
    for (auto &path : paths) path = QFileInfo(path).absoluteFilePath();
    // Deliver once the window is connected and outside native/IPC event handling.
    QTimer::singleShot(0, this, [this, paths = std::move(paths),
                                 activationToken = std::move(activationToken),
                                 startupId = std::move(startupId)] {
        emit openRequested(paths, activationToken, startupId);
    });
}

}
