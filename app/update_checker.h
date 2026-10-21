#pragma once
#include <QNetworkAccessManager>
#include <QProcess>
#include <QTemporaryDir>
#include <memory>

class MainWindow;
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
#ifdef Q_OS_MACOS
    void checkWithSparkle(MainWindow &window);
#endif
};
}
