#include "processing/processing.h"
#include <QCoreApplication>
#include "core/memory.h"
#include <itkMultiThreaderBase.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <numbers>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace imagecxx {
namespace {

struct ProjectedPixel {
    double nearestDepthPos = 0, surface = 0;
    double brightest = -std::numeric_limits<double>::infinity(), brightestDepthPos = 0, sum = 0;
    std::size_t count = 0;
};

std::size_t projectionSize(double value) {
    if (!std::isfinite(value) || value < 1 || value > std::numeric_limits<int>::max())
        throw std::overflow_error(QCoreApplication::translate("Projection", "3D projection dimensions exceed the supported size").toStdString());
    return static_cast<std::size_t>(std::ceil(value));
}

template <typename T>
inline double readTyped(const std::byte *at) {
    if constexpr (std::is_same_v<T, std::uint8_t>)
        return static_cast<double>(std::to_integer<std::uint8_t>(*at));
    else {
        T value;
        std::memcpy(&value, at, sizeof(T));
        return static_cast<double>(value);
    }
}

// Exact std::round for non-negative finite coordinates without libm calls.
inline std::size_t roundNonNegative(double value) {
    const auto base = static_cast<std::size_t>(value);
    return base + static_cast<std::size_t>((value - double(base)) >= 0.5);
}

template <typename T>
inline void writeTyped(std::byte *at, double value) {
    if constexpr (std::is_integral_v<T>)
        value = std::isnan(value) ? 0.0 : std::round(std::clamp(value,
            double(std::numeric_limits<T>::lowest()), double(std::numeric_limits<T>::max())));
    const T number = static_cast<T>(value);
    std::memcpy(at, &number, sizeof(T));
}

inline bool clipRay2D(double o0, double d0, double h0, double o1, double d1, double h1,
                      double &entry, double &exit) {
    entry = -std::numeric_limits<double>::infinity();
    exit = std::numeric_limits<double>::infinity();
    if (d0 == 0.0) {
        if (o0 < -h0 || o0 >= h0) return false;
    } else {
        const double a = (-h0 - o0) / d0, b = (h0 - o0) / d0;
        entry = std::max(entry, std::min(a, b));
        exit = std::min(exit, std::max(a, b));
    }
    if (d1 == 0.0) {
        if (o1 < -h1 || o1 >= h1) return false;
    } else {
        const double a = (-h1 - o1) / d1, b = (h1 - o1) / d1;
        entry = std::max(entry, std::min(a, b));
        exit = std::min(exit, std::max(a, b));
    }
    return exit > entry;
}

template <typename T, ProjectionMethod Method>
inline void accumulateSample(ProjectedPixel &pixel, double value, double depthPos, double lower, double upper) {
    if constexpr (!std::is_integral_v<T>) {
        if (!std::isfinite(value)) return;
    }
    if (value < lower || value > upper) return;
    if (!pixel.count) {
        pixel.nearestDepthPos = depthPos;
        pixel.surface = value;
    }
    if constexpr (Method == ProjectionMethod::BrightestPoint) {
        if (value > pixel.brightest) {
            pixel.brightest = value;
            pixel.brightestDepthPos = depthPos;
        }
    } else if constexpr (Method == ProjectionMethod::MeanValue) {
        if constexpr (std::is_same_v<T, double>)
            pixel.sum = std::lerp(pixel.sum, value, 1.0 / double(pixel.count + 1));
        else
            pixel.sum += value;
    }
    ++pixel.count;
}

template <typename T, ProjectionMethod Method>
inline void finalizePixel(std::byte *at, const ProjectedPixel &pixel, std::size_t k,
                          double radius, const ProjectionOptions &options) {
    if (!pixel.count) return;
    const double nearestDepth = std::clamp(0.5 + pixel.nearestDepthPos / radius, 0.0, 1.0);
    const double surface = pixel.surface * (1.0 - (k == 3 ? 0 : options.surfaceDepthCue) / 100.0 * nearestDepth);
    double value = surface;
    if constexpr (Method != ProjectionMethod::NearestPoint) {
        if constexpr (Method == ProjectionMethod::MeanValue) {
            if constexpr (std::is_same_v<T, double>) value = pixel.sum;
            else value = pixel.sum / double(pixel.count);
        } else {
            const double brightestDepth = std::clamp(0.5 + pixel.brightestDepthPos / radius, 0.0, 1.0);
            value = pixel.brightest * (1.0 - (k == 3 ? 0 : options.interiorDepthCue) / 100.0 * brightestDepth);
        }
        value = std::lerp(value, surface, options.opacity / 100.0);
    }
    writeTyped<T>(at, value);
}

template <typename T, ProjectionMethod Method, bool Interpolate>
void projectTyped(const Image &image, Image &output, const ProjectionOptions &options,
                  std::stop_token stop, const Progress &progress) {
    const auto *buffer = image.data();
    const auto &strides = image.strides();
    const auto components = componentCount(image.pixelType());
    const auto pixelBytes = bytesPerPixel(image.pixelType());
    const auto inW = image.width(), inH = image.height(), inD = image.depth();
    const auto outW = output.width(), outH = output.height(), outD = output.depth();
    const auto channels = image.channels(), frames = image.frames();
    const auto lastX = inW - 1, lastY = inH - 1, lastZ = inD - 1;
    const double maxX = double(lastX), maxY = double(lastY), maxZ = double(lastZ);
    const std::size_t strideX = strides[0], strideY = strides[1], strideZ = strides[2];
    const bool unitSliceSpacing = options.sliceSpacing == 1.0;
    const double zExtent = double(lastZ) * options.sliceSpacing;
    const double radius = options.rotationAxis == 2 ? zExtent
        : std::hypot(double(image.dimension(options.rotationAxis == 0 ? 1 : 0) - 1), zExtent);
    const std::array<double, 3> center{double(lastX) / 2.0, double(lastY) / 2.0, double(lastZ) / 2.0};
    const std::array<double, 3> halfSize{double(inW) / 2.0, double(inH) / 2.0, double(inD) * options.sliceSpacing / 2.0};
    const double outputCenterX = center[0] + std::ceil(double(outW - inW) / 2.0);
    const double outputCenterY = center[1] + std::ceil(double(outH - inH) / 2.0);
    const double lower = options.lower, upper = options.upper;
    auto *destination = output.mutableData();

    struct ZSample {
        double depthPos;
        std::size_t low, high;
        double wz1;
    };
    std::vector<ZSample> zSamples;
    if (options.rotationAxis == 2) {
        const double entry = -halfSize[2], exit = halfSize[2];
        const double length = exit - entry;
        const auto samples = projectionSize(std::max(1.0, length / options.sliceSpacing));
        zSamples.resize(samples);
        for (std::size_t s = 0; s < samples; ++s) {
            const double depthPos = std::lerp(entry, exit, (double(s) + 0.5) / double(samples));
            const double cz = std::clamp(depthPos / options.sliceSpacing + center[2], 0.0, maxZ);
            if constexpr (!Interpolate) {
                const auto iz = roundNonNegative(cz);
                zSamples[s] = {depthPos, iz, iz, 0.0};
            } else {
                const auto lz = static_cast<std::size_t>(cz);
                const auto hz = lz < lastZ ? lz + 1 : lastZ;
                zSamples[s] = {depthPos, lz, hz, cz - double(lz)};
            }
        }
    }

    std::vector<double> cosines(outD), sines(outD);
    for (std::size_t angle = 0; angle < outD; ++angle) {
        const auto degrees = options.initialAngle + angle * options.angleIncrement;
        const double radians = degrees * std::numbers::pi / 180.0;
        double c = std::cos(radians), s = std::sin(radians);
        // Avoid floating-point drift across pixel boundaries in cardinal views.
        if (degrees % 90 == 0) { c = std::round(c); s = std::round(s); }
        cosines[angle] = c;
        sines[angle] = s;
    }

    // For Y-axis rotation, each output row y reads only one XZ slice y across all angles.
    // Grouping all angles per y keeps that W*D plane hot in L1/L2 cache when enough rows exist.
    const bool groupAnglesByY = options.rotationAxis == 1 && buffer && (frames * channels * outH >= 32);
    const std::size_t totalWork = groupAnglesByY ? (frames * channels * outH) : (frames * channels * outD * outH);
    const std::size_t progressStep = std::max<std::size_t>(1, totalWork / 100);

    std::atomic<std::size_t> completed{0};
    std::mutex progressMutex;
    double reported = 0.0;
    std::exception_ptr error;
    std::atomic<bool> failed{false};
    const auto report = [&](double fraction) {
        if (!progress) return;
        const std::lock_guard lock(progressMutex);
        if (failed.load() || stop.stop_requested()) return;
        reported = std::max(reported, std::clamp(fraction, 0.0, 1.0));
        progress(reported);
    };
    report(0.0);

    auto threader = itk::MultiThreaderBase::New();
    // Storage providers need not support concurrent reads without a shared buffer.
    if (!buffer) {
        threader->SetMaximumNumberOfThreads(1);
        threader->SetNumberOfWorkUnits(1);
    }

    const auto readBilinear2D = [&](const std::byte *p00, const std::byte *p10,
                                    const std::byte *p01, const std::byte *p11,
                                    double w00, double w10, double w01, double w11) -> double {
        if constexpr (std::is_integral_v<T>) {
            return w00 * readTyped<T>(p00) + w10 * readTyped<T>(p10)
                 + w01 * readTyped<T>(p01) + w11 * readTyped<T>(p11);
        } else {
            double v = 0.0;
            if (w00 > 0.0) v += w00 * readTyped<T>(p00);
            if (w10 > 0.0) v += w10 * readTyped<T>(p10);
            if (w01 > 0.0) v += w01 * readTyped<T>(p01);
            if (w11 > 0.0) v += w11 * readTyped<T>(p11);
            return v;
        }
    };

    const auto sampleLazyBilinear = [&](std::size_t c, std::size_t t, std::size_t k,
                                        std::size_t x0, std::size_t x1, double wx0, double wx1,
                                        std::size_t y0, std::size_t y1, double wy0, double wy1,
                                        std::size_t z0, std::size_t z1, double wz0, double wz1) -> double {
        double v = 0.0;
        const std::size_t xs[2]{x0, x1}, ys[2]{y0, y1}, zs[2]{z0, z1};
        const double wxs[2]{wx0, wx1}, wys[2]{wy0, wy1}, wzs[2]{wz0, wz1};
        for (int dz = 0; dz < 2; ++dz) {
            if (wzs[dz] <= 0.0) continue;
            for (int dy = 0; dy < 2; ++dy) {
                if (wys[dy] <= 0.0) continue;
                for (int dx = 0; dx < 2; ++dx) {
                    const double w = (wxs[dx] * wys[dy]) * wzs[dz];
                    if (w > 0.0) v += w * image.sample(xs[dx], ys[dy], zs[dz], c, t, k);
                }
            }
        }
        return v;
    };

    const auto processRow = [&](std::size_t y, std::size_t angle, std::size_t c, std::size_t t,
                                std::vector<ProjectedPixel> &scratch) {
        const double cosine = cosines[angle], sine = sines[angle];
        const std::size_t volumeOffset = (strides.size() > 3 ? c * strides[3] : 0)
            + (strides.size() > 4 ? t * strides[4] : 0);
        const std::byte *volBase = buffer ? buffer + volumeOffset : nullptr;
        auto *dstRow = destination + (((t * channels + c) * outD + angle) * outH + y) * outW * pixelBytes;
        const double py = double(y) - outputCenterY;

        if (options.rotationAxis == 0) {
            // X-axis rotation: (y, z) trajectory is identical for every x in this row.
            const double oy = cosine * py, oz = -sine * py;
            const double dy = sine, dz = cosine;
            double entry = 0, exit = 0;
            if (!clipRay2D(oy, dy, halfSize[1], oz, dz, halfSize[2], entry, exit)) return;
            const double length = exit - entry;
            const auto samples = projectionSize(std::max({1.0, length * std::abs(dy), length * std::abs(dz) / options.sliceSpacing}));

            const std::size_t totalPixels = outW * components;
            scratch.assign(totalPixels, ProjectedPixel{});
            std::size_t remaining = totalPixels;

            for (std::size_t sample = 0; sample < samples; ++sample) {
                if ((sample & 4095) == 4095 && (stop.stop_requested() || failed.load()))
                    throw std::runtime_error(QCoreApplication::translate("Projection", "3D projection cancelled").toStdString());
                const double depthPos = std::lerp(entry, exit, (double(sample) + 0.5) / double(samples));
                const double cy = std::clamp(oy + depthPos * dy + center[1], 0.0, maxY);
                const double rawZ = oz + depthPos * dz;
                const double cz = std::clamp((unitSliceSpacing ? rawZ : rawZ / options.sliceSpacing) + center[2], 0.0, maxZ);

                if constexpr (!Interpolate) {
                    const auto iy = roundNonNegative(cy);
                    const auto iz = roundNonNegative(cz);
                    if (volBase) {
                        const auto *srcRow = volBase + iy * strideY + iz * strideZ;
                        if (components == 1) {
                            for (std::size_t x = 0; x < outW; ++x) {
                                auto &px = scratch[x];
                                if constexpr (Method == ProjectionMethod::NearestPoint) {
                                    if (px.count) continue;
                                }
                                accumulateSample<T, Method>(px, readTyped<T>(srcRow + x * strideX), depthPos, lower, upper);
                                if constexpr (Method == ProjectionMethod::NearestPoint) {
                                    if (px.count && --remaining == 0) break;
                                }
                            }
                        } else {
                            for (std::size_t x = 0; x < outW; ++x) {
                                const auto *srcPx = srcRow + x * strideX;
                                for (std::size_t k = 0; k < components; ++k) {
                                    auto &px = scratch[x * components + k];
                                    if constexpr (Method == ProjectionMethod::NearestPoint) {
                                        if (px.count) continue;
                                    }
                                    accumulateSample<T, Method>(px, readTyped<T>(srcPx + k), depthPos, lower, upper);
                                    if constexpr (Method == ProjectionMethod::NearestPoint) {
                                        if (px.count) --remaining;
                                    }
                                }
                            }
                        }
                    } else {
                        for (std::size_t x = 0; x < outW; ++x) {
                            for (std::size_t k = 0; k < components; ++k) {
                                auto &px = scratch[x * components + k];
                                if constexpr (Method == ProjectionMethod::NearestPoint) {
                                    if (px.count) continue;
                                }
                                accumulateSample<T, Method>(px, image.sample(x, iy, iz, c, t, k), depthPos, lower, upper);
                                if constexpr (Method == ProjectionMethod::NearestPoint) {
                                    if (px.count) --remaining;
                                }
                            }
                        }
                    }
                } else {
                    const auto ly = static_cast<std::size_t>(cy), lz = static_cast<std::size_t>(cz);
                    const auto hy = ly < lastY ? ly + 1 : lastY, hz = lz < lastZ ? lz + 1 : lastZ;
                    const double wy1 = cy - double(ly), wy0 = 1.0 - wy1;
                    const double wz1 = cz - double(lz), wz0 = 1.0 - wz1;
                    const double w00 = wy0 * wz0, w10 = wy1 * wz0, w01 = wy0 * wz1, w11 = wy1 * wz1;
                    if (volBase) {
                        const auto *r00 = volBase + ly * strideY + lz * strideZ;
                        const auto *r10 = volBase + hy * strideY + lz * strideZ;
                        const auto *r01 = volBase + ly * strideY + hz * strideZ;
                        const auto *r11 = volBase + hy * strideY + hz * strideZ;
                        if (components == 1) {
                            for (std::size_t x = 0; x < outW; ++x) {
                                auto &px = scratch[x];
                                if constexpr (Method == ProjectionMethod::NearestPoint) {
                                    if (px.count) continue;
                                }
                                const std::size_t off = x * strideX;
                                const double val = readBilinear2D(r00 + off, r10 + off, r01 + off, r11 + off,
                                                                  w00, w10, w01, w11);
                                accumulateSample<T, Method>(px, val, depthPos, lower, upper);
                                if constexpr (Method == ProjectionMethod::NearestPoint) {
                                    if (px.count && --remaining == 0) break;
                                }
                            }
                        } else {
                            for (std::size_t x = 0; x < outW; ++x) {
                                const std::size_t off = x * strideX;
                                for (std::size_t k = 0; k < components; ++k) {
                                    auto &px = scratch[x * components + k];
                                    if constexpr (Method == ProjectionMethod::NearestPoint) {
                                        if (px.count) continue;
                                    }
                                    const double val = readBilinear2D(r00 + off + k, r10 + off + k, r01 + off + k, r11 + off + k,
                                                                      w00, w10, w01, w11);
                                    accumulateSample<T, Method>(px, val, depthPos, lower, upper);
                                    if constexpr (Method == ProjectionMethod::NearestPoint) {
                                        if (px.count) --remaining;
                                    }
                                }
                            }
                        }
                    } else {
                        for (std::size_t x = 0; x < outW; ++x) {
                            for (std::size_t k = 0; k < components; ++k) {
                                auto &px = scratch[x * components + k];
                                if constexpr (Method == ProjectionMethod::NearestPoint) {
                                    if (px.count) continue;
                                }
                                const double val = sampleLazyBilinear(c, t, k, x, x, 1.0, 0.0,
                                                                      ly, hy, wy0, wy1, lz, hz, wz0, wz1);
                                accumulateSample<T, Method>(px, val, depthPos, lower, upper);
                                if constexpr (Method == ProjectionMethod::NearestPoint) {
                                    if (px.count) --remaining;
                                }
                            }
                        }
                    }
                }
                if constexpr (Method == ProjectionMethod::NearestPoint) {
                    if (remaining == 0) break;
                }
            }
            for (std::size_t x = 0; x < outW; ++x) {
                for (std::size_t k = 0; k < components; ++k)
                    finalizePixel<T, Method>(dstRow + x * pixelBytes + (components > 1 ? k : 0),
                                             scratch[x * components + k], k, radius, options);
            }
            return;
        }

        if (options.rotationAxis == 1) {
            // Y-axis rotation: y is constant and exact integer along every ray in this row.
            const std::byte *sliceBase = volBase ? volBase + y * strideY : nullptr;
            const double dx = -sine, dz = cosine;
            for (std::size_t x = 0; x < outW; ++x) {
                const double px = double(x) - outputCenterX;
                const double ox = cosine * px, oz = sine * px;
                double entry = 0, exit = 0;
                if (!clipRay2D(ox, dx, halfSize[0], oz, dz, halfSize[2], entry, exit)) continue;
                const double length = exit - entry;
                const auto samples = projectionSize(std::max({1.0, length * std::abs(dx), length * std::abs(dz) / options.sliceSpacing}));

                std::array<ProjectedPixel, 4> pixels{};
                std::size_t remaining = components;
                for (std::size_t sample = 0; sample < samples; ++sample) {
                    if ((sample & 4095) == 4095 && (stop.stop_requested() || failed.load()))
                        throw std::runtime_error(QCoreApplication::translate("Projection", "3D projection cancelled").toStdString());
                    const double depthPos = std::lerp(entry, exit, (double(sample) + 0.5) / double(samples));
                    const double cx = std::clamp(ox + depthPos * dx + center[0], 0.0, maxX);
                    const double rawZ = oz + depthPos * dz;
                    const double cz = std::clamp((unitSliceSpacing ? rawZ : rawZ / options.sliceSpacing) + center[2], 0.0, maxZ);

                    if constexpr (!Interpolate) {
                        const auto ix = roundNonNegative(cx);
                        const auto iz = roundNonNegative(cz);
                        if (sliceBase) {
                            const auto *at = sliceBase + ix * strideX + iz * strideZ;
                            if (components == 1) {
                                accumulateSample<T, Method>(pixels[0], readTyped<T>(at), depthPos, lower, upper);
                                if constexpr (Method == ProjectionMethod::NearestPoint) {
                                    if (pixels[0].count) break;
                                }
                            } else {
                                for (std::size_t k = 0; k < components; ++k) {
                                    if constexpr (Method == ProjectionMethod::NearestPoint) {
                                        if (pixels[k].count) continue;
                                    }
                                    accumulateSample<T, Method>(pixels[k], readTyped<T>(at + k), depthPos, lower, upper);
                                    if constexpr (Method == ProjectionMethod::NearestPoint) {
                                        if (pixels[k].count) --remaining;
                                    }
                                }
                                if constexpr (Method == ProjectionMethod::NearestPoint) {
                                    if (remaining == 0) break;
                                }
                            }
                        } else {
                            for (std::size_t k = 0; k < components; ++k) {
                                if constexpr (Method == ProjectionMethod::NearestPoint) {
                                    if (pixels[k].count) continue;
                                }
                                accumulateSample<T, Method>(pixels[k], image.sample(ix, y, iz, c, t, k), depthPos, lower, upper);
                                if constexpr (Method == ProjectionMethod::NearestPoint) {
                                    if (pixels[k].count) --remaining;
                                }
                            }
                            if constexpr (Method == ProjectionMethod::NearestPoint) {
                                if (remaining == 0) break;
                            }
                        }
                    } else {
                        const auto lx = static_cast<std::size_t>(cx), lz = static_cast<std::size_t>(cz);
                        const auto hx = lx < lastX ? lx + 1 : lastX, hz = lz < lastZ ? lz + 1 : lastZ;
                        const double wx1 = cx - double(lx), wx0 = 1.0 - wx1;
                        const double wz1 = cz - double(lz), wz0 = 1.0 - wz1;
                        const double w00 = wx0 * wz0, w10 = wx1 * wz0, w01 = wx0 * wz1, w11 = wx1 * wz1;
                        if (sliceBase) {
                            const auto *zL = sliceBase + lz * strideZ;
                            const auto *zH = sliceBase + hz * strideZ;
                            const std::size_t oxL = lx * strideX, oxH = hx * strideX;
                            if (components == 1) {
                                const double val = readBilinear2D(zL + oxL, zL + oxH, zH + oxL, zH + oxH,
                                                                  w00, w10, w01, w11);
                                accumulateSample<T, Method>(pixels[0], val, depthPos, lower, upper);
                                if constexpr (Method == ProjectionMethod::NearestPoint) {
                                    if (pixels[0].count) break;
                                }
                            } else {
                                for (std::size_t k = 0; k < components; ++k) {
                                    if constexpr (Method == ProjectionMethod::NearestPoint) {
                                        if (pixels[k].count) continue;
                                    }
                                    const double val = readBilinear2D(zL + oxL + k, zL + oxH + k, zH + oxL + k, zH + oxH + k,
                                                                      w00, w10, w01, w11);
                                    accumulateSample<T, Method>(pixels[k], val, depthPos, lower, upper);
                                    if constexpr (Method == ProjectionMethod::NearestPoint) {
                                        if (pixels[k].count) --remaining;
                                    }
                                }
                                if constexpr (Method == ProjectionMethod::NearestPoint) {
                                    if (remaining == 0) break;
                                }
                            }
                        } else {
                            for (std::size_t k = 0; k < components; ++k) {
                                if constexpr (Method == ProjectionMethod::NearestPoint) {
                                    if (pixels[k].count) continue;
                                }
                                const double val = sampleLazyBilinear(c, t, k, lx, hx, wx0, wx1,
                                                                      y, y, 1.0, 0.0, lz, hz, wz0, wz1);
                                accumulateSample<T, Method>(pixels[k], val, depthPos, lower, upper);
                                if constexpr (Method == ProjectionMethod::NearestPoint) {
                                    if (pixels[k].count) --remaining;
                                }
                            }
                            if constexpr (Method == ProjectionMethod::NearestPoint) {
                                if (remaining == 0) break;
                            }
                        }
                    }
                }
                for (std::size_t k = 0; k < components; ++k)
                    finalizePixel<T, Method>(dstRow + x * pixelBytes + (components > 1 ? k : 0),
                                             pixels[k], k, radius, options);
            }
            return;
        }

        // Z-axis rotation: (x, y) and bilinear weights are constant along the entire Z ray.
        const auto samples = zSamples.size();
        for (std::size_t x = 0; x < outW; ++x) {
            const double px = double(x) - outputCenterX;
            const double ox = cosine * px + sine * py;
            const double oy = -sine * px + cosine * py;
            if (ox < -halfSize[0] || ox >= halfSize[0] || oy < -halfSize[1] || oy >= halfSize[1]) continue;
            const double cx = std::clamp(ox + center[0], 0.0, maxX);
            const double cy = std::clamp(oy + center[1], 0.0, maxY);

            std::array<ProjectedPixel, 4> pixels{};
            std::size_t remaining = components;
            if constexpr (!Interpolate) {
                const auto ix = roundNonNegative(cx);
                const auto iy = roundNonNegative(cy);
                const std::byte *xyBase = volBase ? volBase + ix * strideX + iy * strideY : nullptr;
                for (std::size_t s = 0; s < samples; ++s) {
                    if ((s & 4095) == 4095 && (stop.stop_requested() || failed.load()))
                        throw std::runtime_error(QCoreApplication::translate("Projection", "3D projection cancelled").toStdString());
                    const auto &zs = zSamples[s];
                    if (xyBase) {
                        const auto *at = xyBase + zs.low * strideZ;
                        if (components == 1) {
                            accumulateSample<T, Method>(pixels[0], readTyped<T>(at), zs.depthPos, lower, upper);
                            if constexpr (Method == ProjectionMethod::NearestPoint) {
                                if (pixels[0].count) break;
                            }
                        } else {
                            for (std::size_t k = 0; k < components; ++k) {
                                if constexpr (Method == ProjectionMethod::NearestPoint) {
                                    if (pixels[k].count) continue;
                                }
                                accumulateSample<T, Method>(pixels[k], readTyped<T>(at + k), zs.depthPos, lower, upper);
                                if constexpr (Method == ProjectionMethod::NearestPoint) {
                                    if (pixels[k].count) --remaining;
                                }
                            }
                            if constexpr (Method == ProjectionMethod::NearestPoint) {
                                if (remaining == 0) break;
                            }
                        }
                    } else {
                        for (std::size_t k = 0; k < components; ++k) {
                            if constexpr (Method == ProjectionMethod::NearestPoint) {
                                if (pixels[k].count) continue;
                            }
                            accumulateSample<T, Method>(pixels[k], image.sample(ix, iy, zs.low, c, t, k), zs.depthPos, lower, upper);
                            if constexpr (Method == ProjectionMethod::NearestPoint) {
                                if (pixels[k].count) --remaining;
                            }
                        }
                        if constexpr (Method == ProjectionMethod::NearestPoint) {
                            if (remaining == 0) break;
                        }
                    }
                }
            } else {
                const auto lx = static_cast<std::size_t>(cx), ly = static_cast<std::size_t>(cy);
                const auto hx = lx < lastX ? lx + 1 : lastX, hy = ly < lastY ? ly + 1 : lastY;
                const double wx1 = cx - double(lx), wx0 = 1.0 - wx1;
                const double wy1 = cy - double(ly), wy0 = 1.0 - wy1;
                const double w00 = wx0 * wy0, w10 = wx1 * wy0, w01 = wx0 * wy1, w11 = wx1 * wy1;
                const auto *b00 = volBase ? volBase + lx * strideX + ly * strideY : nullptr;
                const auto *b10 = volBase ? volBase + hx * strideX + ly * strideY : nullptr;
                const auto *b01 = volBase ? volBase + lx * strideX + hy * strideY : nullptr;
                const auto *b11 = volBase ? volBase + hx * strideX + hy * strideY : nullptr;
                for (std::size_t s = 0; s < samples; ++s) {
                    if ((s & 4095) == 4095 && (stop.stop_requested() || failed.load()))
                        throw std::runtime_error(QCoreApplication::translate("Projection", "3D projection cancelled").toStdString());
                    const auto &zs = zSamples[s];
                    const double wz1 = zs.wz1, wz0 = 1.0 - wz1;
                    if (volBase) {
                        const std::size_t ozL = zs.low * strideZ;
                        const std::size_t ozH = zs.high * strideZ;
                        const auto readZSample = [&](std::size_t kOff) -> double {
                            if (wz1 == 0.0)
                                return readBilinear2D(b00 + ozL + kOff, b10 + ozL + kOff, b01 + ozL + kOff, b11 + ozL + kOff,
                                                      w00, w10, w01, w11);
                            const std::array<double, 8> weights{
                                w00 * wz0, w10 * wz0, w01 * wz0, w11 * wz0,
                                w00 * wz1, w10 * wz1, w01 * wz1, w11 * wz1};
                            const std::array<const std::byte *, 8> ptrs{
                                b00 + ozL + kOff, b10 + ozL + kOff, b01 + ozL + kOff, b11 + ozL + kOff,
                                b00 + ozH + kOff, b10 + ozH + kOff, b01 + ozH + kOff, b11 + ozH + kOff};
                            double v = 0.0;
                            for (std::size_t i = 0; i < 8; ++i)
                                if (weights[i] > 0.0) v += weights[i] * readTyped<T>(ptrs[i]);
                            return v;
                        };
                        if (components == 1) {
                            accumulateSample<T, Method>(pixels[0], readZSample(0), zs.depthPos, lower, upper);
                            if constexpr (Method == ProjectionMethod::NearestPoint) {
                                if (pixels[0].count) break;
                            }
                        } else {
                            for (std::size_t k = 0; k < components; ++k) {
                                if constexpr (Method == ProjectionMethod::NearestPoint) {
                                    if (pixels[k].count) continue;
                                }
                                accumulateSample<T, Method>(pixels[k], readZSample(k), zs.depthPos, lower, upper);
                                if constexpr (Method == ProjectionMethod::NearestPoint) {
                                    if (pixels[k].count) --remaining;
                                }
                            }
                            if constexpr (Method == ProjectionMethod::NearestPoint) {
                                if (remaining == 0) break;
                            }
                        }
                    } else {
                        for (std::size_t k = 0; k < components; ++k) {
                            if constexpr (Method == ProjectionMethod::NearestPoint) {
                                if (pixels[k].count) continue;
                            }
                            const double val = sampleLazyBilinear(c, t, k, lx, hx, wx0, wx1,
                                                                  ly, hy, wy0, wy1, zs.low, zs.high, wz0, wz1);
                            accumulateSample<T, Method>(pixels[k], val, zs.depthPos, lower, upper);
                            if constexpr (Method == ProjectionMethod::NearestPoint) {
                                if (pixels[k].count) --remaining;
                            }
                        }
                        if constexpr (Method == ProjectionMethod::NearestPoint) {
                            if (remaining == 0) break;
                        }
                    }
                }
            }
            for (std::size_t k = 0; k < components; ++k)
                finalizePixel<T, Method>(dstRow + x * pixelBytes + (components > 1 ? k : 0),
                                         pixels[k], k, radius, options);
        }
    };

    threader->ParallelizeArray(0, totalWork, [&](std::size_t work) {
        try {
            if (failed.load()) return;
            if (stop.stop_requested())
                throw std::runtime_error(QCoreApplication::translate("Projection", "3D projection cancelled").toStdString());
            std::vector<ProjectedPixel> scratch;
            if (groupAnglesByY) {
                auto remainder = work;
                const auto y = remainder % outH; remainder /= outH;
                const auto c = remainder % channels;
                const auto t = remainder / channels;
                for (std::size_t angle = 0; angle < outD; ++angle) {
                    if (failed.load()) return;
                    if (stop.stop_requested())
                        throw std::runtime_error(QCoreApplication::translate("Projection", "3D projection cancelled").toStdString());
                    processRow(y, angle, c, t, scratch);
                }
            } else {
                auto remainder = work;
                const auto y = remainder % outH; remainder /= outH;
                const auto angle = remainder % outD; remainder /= outD;
                const auto c = remainder % channels;
                const auto t = remainder / channels;
                processRow(y, angle, c, t, scratch);
            }
            const auto done = completed.fetch_add(1, std::memory_order_relaxed) + 1;
            if (progress && (done % progressStep == 0 || done == totalWork))
                report(double(done) / double(totalWork));
        } catch (...) {
            const std::lock_guard guard(progressMutex);
            if (!error) error = std::current_exception();
            failed.store(true);
        }
    }, nullptr);

    if (error) std::rethrow_exception(error);
    if (stop.stop_requested())
        throw std::runtime_error(QCoreApplication::translate("Projection", "3D projection cancelled").toStdString());
    report(1.0);
}

template <typename T>
void dispatchMethod(const Image &image, Image &output, const ProjectionOptions &options,
                    std::stop_token stop, const Progress &progress) {
    const auto dispatchInterp = [&]<ProjectionMethod M>() {
        if (options.interpolate) projectTyped<T, M, true>(image, output, options, stop, progress);
        else projectTyped<T, M, false>(image, output, options, stop, progress);
    };
    switch (options.method) {
    case ProjectionMethod::NearestPoint: dispatchInterp.template operator()<ProjectionMethod::NearestPoint>(); break;
    case ProjectionMethod::BrightestPoint: dispatchInterp.template operator()<ProjectionMethod::BrightestPoint>(); break;
    case ProjectionMethod::MeanValue: dispatchInterp.template operator()<ProjectionMethod::MeanValue>(); break;
    }
}

} // namespace

ProjectionGeometry projectionGeometry(const Image &image, const ProjectionOptions &options) {
    if (image.empty() || image.depth() < 2 || image.shape().size() > 5)
        throw std::invalid_argument(QCoreApplication::translate("Projection", "3D Project requires a Z stack with at least two slices and at most five axes").toStdString());
    if (options.method != ProjectionMethod::NearestPoint && options.method != ProjectionMethod::BrightestPoint
        && options.method != ProjectionMethod::MeanValue)
        throw std::invalid_argument(QCoreApplication::translate("Projection", "Invalid projection method").toStdString());
    if (options.rotationAxis < 0 || options.rotationAxis > 2 || !std::isfinite(options.sliceSpacing) || options.sliceSpacing <= 0
        || options.initialAngle < 0 || options.initialAngle > 359 || options.totalRotation < 0 || options.totalRotation > 360
        || options.angleIncrement < 1 || options.angleIncrement > 360)
        throw std::invalid_argument(QCoreApplication::translate("Projection", "Invalid rotation axis, spacing or angles").toStdString());
    if (!std::isfinite(options.lower) || !std::isfinite(options.upper) || options.lower > options.upper
        || options.opacity < 0 || options.opacity > 100 || options.surfaceDepthCue < 0 || options.surfaceDepthCue > 100
        || options.interiorDepthCue < 0 || options.interiorDepthCue > 100)
        throw std::invalid_argument(QCoreApplication::translate("Projection", "Invalid transparency bounds, opacity or depth cueing").toStdString());
    const double depth = double(image.depth()) * options.sliceSpacing;
    projectionSize(std::max(depth, 1.));
    projectionSize(double(image.width())); projectionSize(double(image.height()));
    ProjectionGeometry geometry;
    geometry.shape = image.shape();
    geometry.shape[0] = options.rotationAxis == 0 ? image.width() : projectionSize(std::hypot(double(image.width()),
        options.rotationAxis == 1 ? depth : double(image.height())));
    geometry.shape[1] = options.rotationAxis == 1 ? image.height() : projectionSize(std::hypot(double(image.height()),
        options.rotationAxis == 0 ? depth : double(image.width())));
    geometry.shape[2] = options.totalRotation == 360 ? (359 / options.angleIncrement + 1)
        : (options.totalRotation / options.angleIncrement + 1);
    geometry.byteCount = bytesPerPixel(image.pixelType());
    for (auto length : geometry.shape) {
        if (geometry.byteCount > std::numeric_limits<std::size_t>::max() / length)
            throw std::overflow_error(QCoreApplication::translate("Projection", "3D projection buffer exceeds the addressable size").toStdString());
        geometry.byteCount *= length;
    }
    geometry.requiredMemoryBytes = geometry.byteCount;
    return geometry;
}

Image project3D(const Image &image, const ProjectionOptions &options, std::stop_token stop, Progress progress) {
    static std::mutex allocationMutex;
    const std::unique_lock lock(allocationMutex);
    if (stop.stop_requested()) throw std::runtime_error(QCoreApplication::translate("Projection", "3D projection cancelled").toStdString());
    const auto geometry = projectionGeometry(image, options);
    detail::checkMemoryBudget(geometry.requiredMemoryBytes, detail::availableMemoryBytes());
    try {
        auto metadata = image.metadata();
        metadata.name += QCoreApplication::translate("Projection", " · 3D Projection"); metadata.sourcePath.clear(); metadata.direction.clear();
        metadata.axes[0] = {"X", 1, 0, "px"}; metadata.axes[1] = {"Y", 1, 0, "px"};
        metadata.axes[2] = {"Angle", double(options.angleIncrement), double(options.initialAngle), "degrees"};
        metadata.attributes.insert("ImageCXX.LastOperation", "3D Project");
        metadata.attributes.remove("ImageCXX.EmbeddedPlaneOrigin"); metadata.attributes.remove("ImageCXX.EmbeddedPlaneZUnit");
        auto output = Image::allocate(image.pixelType(), geometry.shape, metadata);
        switch (image.pixelType()) {
        case PixelType::UInt8: case PixelType::RGB8: case PixelType::RGBA8:
            dispatchMethod<std::uint8_t>(image, output, options, stop, progress); break;
        case PixelType::UInt16:
            dispatchMethod<std::uint16_t>(image, output, options, stop, progress); break;
        case PixelType::Int16:
            dispatchMethod<std::int16_t>(image, output, options, stop, progress); break;
        case PixelType::Float32:
            dispatchMethod<float>(image, output, options, stop, progress); break;
        case PixelType::Float64:
            dispatchMethod<double>(image, output, options, stop, progress); break;
        }
        return output;
    } catch (const std::bad_alloc &) {
        throw std::runtime_error(QCoreApplication::translate("Projection", "Memory allocation failed during 3D projection. The source image is unchanged.").toStdString());
    }
}
} // namespace imagecxx
