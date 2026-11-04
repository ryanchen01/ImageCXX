#pragma once

#include <QObject>
#include <QLocalServer>
#include <QLockFile>
#include <QStringList>

namespace imagecxx {

class FileOpenService : public QObject {
    Q_OBJECT
public:
    explicit FileOpenService(QObject *parent = nullptr);
    // Returns false after forwarding to the running desktop instance.
    bool start(const QStringList &paths);

signals:
    void openRequested(const QStringList &paths);

protected:
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    void requestOpen(QStringList paths);
    QString serverName_;
    QLockFile lock_;
    QLocalServer server_;
};

}
