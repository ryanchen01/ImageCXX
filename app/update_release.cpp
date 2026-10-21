#include "app/update_release.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QVersionNumber>

namespace imagecxx {
std::optional<ReleaseUpdate> releaseUpdate(const QByteArray &json, const QString &currentVersion,
    const QString &assetSuffix)
{
    const auto document = QJsonDocument::fromJson(json);
    if (!document.isObject()) return {};
    const auto release = document.object();
    if (release.value("draft").toBool(true) || release.value("prerelease").toBool(true)) return {};
    const auto tag = release.value("tag_name").toString();
    static const QRegularExpression versionPattern("^v?((?:0|[1-9][0-9]*)\\.(?:0|[1-9][0-9]*)\\.(?:0|[1-9][0-9]*))$");
    const auto match = versionPattern.match(tag);
    if (!match.hasMatch()) return {};
    const auto version = match.captured(1);
    const auto current = QVersionNumber::fromString(currentVersion);
    if (current.isNull() || QVersionNumber::fromString(version) <= current) return {};
    const auto name = "ImageCXX-" + version + assetSuffix;
    for (const auto &value : release.value("assets").toArray()) {
        const auto asset = value.toObject();
        if (asset.value("name").toString() != name || asset.value("state").toString() != "uploaded") continue;
        const QUrl url(asset.value("browser_download_url").toString());
        if (url.scheme() != "https" || url.host() != "github.com" || !url.userInfo().isEmpty()
            || url.port() != -1 || url.hasQuery() || url.hasFragment()
            || url.path() != "/ryanchen01/ImageCXX/releases/download/" + tag + "/" + name) return {};
        const auto digest = asset.value("digest").toString();
        static const QRegularExpression digestPattern("^sha256:[a-fA-F0-9]{64}$");
        const auto size = asset.value("size").toInteger();
        if (!digestPattern.match(digest).hasMatch() || size <= 0) return {};
        return ReleaseUpdate{version, url, QByteArray::fromHex(digest.mid(7).toLatin1()), size};
    }
    return {};
}

bool verifyUpdate(const QString &path, const ReleaseUpdate &release)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() != release.size) return false;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&file) && hash.result() == release.sha256;
}
}
