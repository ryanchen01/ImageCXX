#pragma once
#include <QByteArray>
#include <QString>
#include <QUrl>
#include <optional>

namespace imagecxx {
struct ReleaseUpdate {
    QString version;
    QUrl url;
    QByteArray sha256;
    qint64 size = 0;
};

std::optional<ReleaseUpdate> releaseUpdate(const QByteArray &json, const QString &currentVersion,
    const QString &assetSuffix);
bool verifyUpdate(const QString &path, const ReleaseUpdate &release);
}
