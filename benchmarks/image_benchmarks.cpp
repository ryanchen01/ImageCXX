#include "core/image.h"
#include "processing/processing.h"
#include <benchmark/benchmark.h>
#include <cmath>
#include <cstdint>

using namespace imagecxx;

namespace {

Image unsignedRamp(std::size_t edge, std::size_t depth = 1) {
    auto image = Image::allocate(PixelType::UInt16, {edge, edge, depth});
    auto *pixels = reinterpret_cast<std::uint16_t *>(image.mutableData());
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel)
        pixels[pixel] = static_cast<std::uint16_t>((pixel * 17 + pixel / edge) % 65536);
    return image;
}

void SharedImageSnapshot(benchmark::State &state) {
    const auto image = unsignedRamp(static_cast<std::size_t>(state.range(0)));
    for (auto _ : state) {
        const Image snapshot = image;
        benchmark::DoNotOptimize(snapshot.data());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(SharedImageSnapshot)->Arg(512)->Arg(2048);

void ContiguousSliceView(benchmark::State &state) {
    const auto image = unsignedRamp(static_cast<std::size_t>(state.range(0)), 8);
    std::size_t z = 0;
    for (auto _ : state) {
        const auto slice = image.slice(z++ % image.depth());
        benchmark::DoNotOptimize(slice.data());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(ContiguousSliceView)->Arg(512)->Arg(2048);

void DetachedImageCopy(benchmark::State &state) {
    const auto image = unsignedRamp(static_cast<std::size_t>(state.range(0)));
    for (auto _ : state) {
        const auto copy = image.clone();
        benchmark::DoNotOptimize(copy.data());
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(image.byteCount()));
}
BENCHMARK(DetachedImageCopy)->Arg(512)->Arg(2048);

void HistogramUInt16(benchmark::State &state) {
    const auto image = unsignedRamp(static_cast<std::size_t>(state.range(0)));
    for (auto _ : state) {
        const auto result = histogram(image, 256);
        benchmark::DoNotOptimize(result.counts.data());
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(image.pixelCount()));
}
BENCHMARK(HistogramUInt16)->Arg(512)->Arg(2048)->Unit(benchmark::kMillisecond);

void Project3D(benchmark::State &state) {
    const auto image = unsignedRamp(128, 128);
    ProjectionOptions options;
    options.method = static_cast<ProjectionMethod>(state.range(0));
    options.interpolate = state.range(1);
    options.angleIncrement = 30;
    options.upper = 65535;
    for (auto _ : state) {
        const auto result = project3D(image, options);
        benchmark::DoNotOptimize(result.data());
    }
}
BENCHMARK(Project3D)->ArgsProduct({{0, 1, 2}, {0, 1}})->Unit(benchmark::kMillisecond)->UseRealTime();

void ItkGaussianFloat32(benchmark::State &state) {
    const auto edge = static_cast<std::size_t>(state.range(0));
    auto image = Image::allocate(PixelType::Float32, {edge, edge});
    auto *pixels = reinterpret_cast<float *>(image.mutableData());
    for (std::size_t y = 0; y < edge; ++y)
        for (std::size_t x = 0; x < edge; ++x)
            pixels[y * edge + x] = static_cast<float>(std::sin(x / 13.0) + std::cos(y / 19.0));
    ProcessingOptions options;
    options.operation = Operation::Gaussian;
    options.sigma = 1.5;
    for (auto _ : state) {
        const auto result = process(image, options);
        benchmark::DoNotOptimize(result.data());
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(image.pixelCount()));
}
BENCHMARK(ItkGaussianFloat32)->Arg(256)->Arg(512)->Unit(benchmark::kMillisecond)->UseRealTime();

} // namespace
