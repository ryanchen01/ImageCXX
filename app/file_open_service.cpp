#include "app/file_open_service.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QTimer>
#include <stdexcept>

namespace imagecxx {

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
                if (!document.isArray()) { socket->disconnectFromServer(); return; }
                QStringList paths;
                for (const auto &value : document.array()) paths.append(value.toString());
                requestOpen(paths);
                socket->write("OK\n");
                socket->disconnectFromServer();
            });
        }
    });
}

bool FileOpenService::start(const QStringList &paths)
{
    if (!lock_.tryLock(5000))
        throw std::runtime_error(tr("Cannot initialize file opening while another ImageCXX instance is starting.").toStdString());
    QLocalSocket socket;
    socket.connectToServer(serverName_);
    if (socket.waitForConnected(1000)) {
        QJsonArray files;
        for (const auto &path : paths) files.append(QFileInfo(path).absoluteFilePath());
        socket.write(QJsonDocument(files).toJson(QJsonDocument::Compact) + '\n');
        if (!socket.waitForBytesWritten(5000) ||
            (!socket.canReadLine() && !socket.waitForReadyRead(5000)) || socket.readLine() != "OK\n")
            throw std::runtime_error(tr("The running ImageCXX instance did not accept the files.").toStdString());
        return false;
    }
    QLocalServer::removeServer(serverName_); // Remove an endpoint left by a crashed instance.
    if (!server_.listen(serverName_))
        throw std::runtime_error(tr("Cannot listen for file-open requests: %1").arg(server_.errorString()).toStdString());
    lock_.unlock();
    requestOpen(paths);
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

void FileOpenService::requestOpen(QStringList paths)
{
    for (auto &path : paths) path = QFileInfo(path).absoluteFilePath();
    // Deliver once the window is connected and outside native/IPC event handling.
    QTimer::singleShot(0, this, [this, paths] { emit openRequested(paths); });
}

}
