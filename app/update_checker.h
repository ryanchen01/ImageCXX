#pragma once
#include <QNetworkAccessManager>
#include <QProcess>
#include <QPointer>
#include <QTemporaryDir>
#include <memory>

class MainWindow;
class QProgressDialog;
namespace imagecxx {
class UpdateChecker : public QObject {
public:
    explicit UpdateChecker(MainWindow &window);
    QString pendingProgram;
    QStringList pendingArguments;
private:
    QNetworkAccessManager network_;
    std::unique_ptr<QTemporaryDir> directory_;
    QProcess process_;
    bool checking_ = false;
    QPointer<QProgressDialog> progress_;
#ifdef Q_OS_MACOS
    void checkWithSparkle(MainWindow &window, bool manual);
#endif
};
}
