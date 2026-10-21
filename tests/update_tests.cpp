#include "app/update_release.h"
#include <gtest/gtest.h>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

namespace {
QJsonObject release(const QString &version = "0.3.0")
{
    const auto name = "ImageCXX-" + version + "-windows-x86_64.exe";
    return {{"tag_name", "v" + version}, {"draft", false}, {"prerelease", false},
        {"assets", QJsonArray{QJsonObject{{"name", name}, {"state", "uploaded"}, {"size", 3},
            {"digest", "sha256:" + QString(QCryptographicHash::hash("abc", QCryptographicHash::Sha256).toHex())},
            {"browser_download_url", "https://github.com/ryanchen01/ImageCXX/releases/download/v" + version + "/" + name}}}}};
}
std::optional<imagecxx::ReleaseUpdate> parse(const QJsonObject &object)
{
    return imagecxx::releaseUpdate(QJsonDocument(object).toJson(), "0.2.0", "-windows-x86_64.exe");
}
}

TEST(Updates, AcceptsOnlyNewerStableVersionsAndMatchingAssets)
{
    ASSERT_TRUE(parse(release()));
    EXPECT_EQ(parse(release())->version, "0.3.0");
    EXPECT_TRUE(parse(release("0.10.0")));
    EXPECT_FALSE(parse(release("0.2.0")));
    EXPECT_FALSE(parse(release("0.1.9")));
    for (const auto &version : {"0.3.0-beta.1", "0.3", "01.3.0", "junk"}) EXPECT_FALSE(parse(release(version)));
    auto object = release(); object["draft"] = true; EXPECT_FALSE(parse(object));
    object = release(); object["prerelease"] = true; EXPECT_FALSE(parse(object));
    object = release(); object.remove("prerelease"); EXPECT_FALSE(parse(object));
    EXPECT_FALSE(imagecxx::releaseUpdate("not json", "0.2.0", "-windows-x86_64.exe"));
    EXPECT_FALSE(imagecxx::releaseUpdate(QJsonDocument(release()).toJson(), "0.2.0", "-linux-x86_64.AppImage"));
}

TEST(Updates, RejectsUntrustedOrIncompleteAssets)
{
    for (const auto &url : {"http://github.com/ryanchen01/ImageCXX/releases/download/v0.3.0/ImageCXX-0.3.0-windows-x86_64.exe",
        "https://evil.example/ImageCXX-0.3.0-windows-x86_64.exe", "https://github.com/other/repo/releases/download/v0.3.0/setup.exe"}) {
        auto object = release(); auto asset = object["assets"].toArray()[0].toObject();
        asset["browser_download_url"] = url; object["assets"] = QJsonArray{asset}; EXPECT_FALSE(parse(object));
    }
    for (const auto &field : {"digest", "size", "state", "name"}) {
        auto object = release(); auto asset = object["assets"].toArray()[0].toObject();
        asset.remove(field); object["assets"] = QJsonArray{asset}; EXPECT_FALSE(parse(object));
    }
    auto object = release(); auto asset = object["assets"].toArray()[0].toObject();
    asset["digest"] = "sha256:not-a-hash"; object["assets"] = QJsonArray{asset}; EXPECT_FALSE(parse(object));
}

TEST(Updates,ChecksSizeAndDigestBeforeInstallation)
{
    QTemporaryDir directory;
    const auto path = directory.filePath("setup.exe");
    const auto update = parse(release()); ASSERT_TRUE(update);
    EXPECT_FALSE(imagecxx::verifyUpdate(path, *update));
    QFile file(path); ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write("abc"), 3); file.close();
    EXPECT_TRUE(imagecxx::verifyUpdate(path, *update));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly)); ASSERT_EQ(file.write("bad"), 3); file.close();
    EXPECT_FALSE(imagecxx::verifyUpdate(path, *update));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly)); ASSERT_EQ(file.write("abcd"), 4); file.close();
    EXPECT_FALSE(imagecxx::verifyUpdate(path, *update));
}
