#include "app/batch.h"
#include "app/command_registry.h"
#include "app/processing_commands.h"
#include "io/image_io.h"
#include <gtest/gtest.h>
#include <QTemporaryDir>

using namespace imagecxx;
TEST(Commands, ValidatesDefaultsRangesChoicesAndUnknownParameters)
{
    CommandRegistry registry;
    int executions = 0;
    QString executed;
    QVariantMap received;
    registerProcessingCommands(registry, [&](const QString &id, const QVariantMap &parameters) {
        ++executions; executed = id; received = parameters;
    });
    registry.execute("process.gaussian", {{"sigma", 2.5}});
    EXPECT_EQ(executed, "process.gaussian");
    EXPECT_DOUBLE_EQ(received.value("sigma").toDouble(), 2.5);
    EXPECT_EQ(received.value("scope"), "Full image");
    EXPECT_THROW(registry.execute("process.gaussian", {{"sigma", -1}}), std::invalid_argument);
    EXPECT_THROW(registry.execute("process.median", {{"radius", 1.5}}), std::invalid_argument);
    EXPECT_THROW(registry.execute("process.gaussian", {{"scope", "Other"}}), std::invalid_argument);
    EXPECT_THROW(registry.execute("process.gaussian", {{"mystery", 1}}), std::invalid_argument);
    EXPECT_THROW(registry.execute("missing"), std::invalid_argument);
    EXPECT_EQ(executions, 1);
}

TEST(Commands, NumericStringsKeepTheirValidatedValueDuringExecution)
{
    CommandRegistry registry;
    ProcessingOptions options;
    registerProcessingCommands(registry, [&](const QString &id, const QVariantMap &parameters) {
        options = processingOptions(id, parameters);
    });
    for (const auto *radius : {"1e1", "10.0", "10"}) {
        registry.execute("process.median", {{"radius", radius}});
        EXPECT_EQ(options.radius, 10u);
    }
    registry.execute("process.gaussian", {{"sigma", "2.5e0"}});
    EXPECT_DOUBLE_EQ(options.sigma, 2.5);
    EXPECT_THROW(registry.execute("process.median", {{"radius", "1.5"}}), std::invalid_argument);
    EXPECT_THROW(registry.execute("process.median", {{"radius", "1e2"}}), std::invalid_argument);
}

TEST(Batch, ProducesTiffReportsFailuresAndNeverOverwrites)
{
    QTemporaryDir directory;
    auto image = Image::allocate(PixelType::UInt8, {8, 8});
    image.mutableData()[0] = std::byte{200};
    const auto source = directory.filePath("input.tif"); writeImage(image, source);
    const auto targetDirectory = directory.filePath("outputs");
    const auto result = runBatch({source, directory.filePath("missing.tif")}, targetDirectory,
        "process.threshold", {{"lower", 100}, {"upper", 255}});
    ASSERT_EQ(result.outputs.size(), 1); EXPECT_EQ(result.errors.size(), 1);
    const auto output = readImage(result.outputs.front());
    EXPECT_DOUBLE_EQ(output.sample(0, 0), 255); EXPECT_DOUBLE_EQ(output.sample(1, 0), 0);
    const auto duplicate = runBatch({source}, targetDirectory, "process.threshold", {{"lower", 0}, {"upper", 100}});
    EXPECT_TRUE(duplicate.outputs.empty()); EXPECT_EQ(duplicate.errors.size(), 1);
    const auto retained = readImage(result.outputs.front());
    EXPECT_DOUBLE_EQ(retained.sample(0, 0), 255); EXPECT_DOUBLE_EQ(retained.sample(1, 0), 0);
    EXPECT_DOUBLE_EQ(readImage(source).sample(0, 0), 200);
}
