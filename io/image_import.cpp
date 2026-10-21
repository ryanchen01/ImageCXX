#include "io/image_io.h"
#include "io/io_detail.h"
#include "core/memory.h"

#include <dcmtk/dcmdata/dctk.h>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QMap>
#include <QSet>
#include <algorithm>
#include <limits>

namespace imagecxx {

ImageImportPlan discoverImageImports(const QStringList &paths, std::stop_token stop, IoProgress progress)
{
    ImageImportPlan plan;
    struct Candidate { QString path; bool explicitFile; };
    QList<Candidate> candidates;
    QSet<QString> seen;
    for (const auto &path : paths) {
        detail::checkIo(stop, progress, 0);
        if (path.isEmpty()) continue;
        const QFileInfo info(path);
        if (!info.exists() || !info.isReadable()) {
            plan.errors.append(QStringLiteral("Cannot read %1").arg(path));
        } else if (info.isDir()) {
            const auto previousCount = candidates.size();
            QDirIterator files(path, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
            while (files.hasNext()) {
                detail::checkIo(stop, progress, 0);
                candidates.append({files.next(), false});
            }
            if (candidates.size() == previousCount)
                plan.errors.append(QStringLiteral("No image files found in %1").arg(path));
        } else {
            candidates.append({path, true});
        }
    }
    // Explicit selections take precedence over files also found inside a folder.
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) {
        return a.explicitFile > b.explicitFile;
    });
    QSet<QString> imageSuffixes{"tif", "tiff", "mhd", "mha", "nii", "gz", "nrrd", "nhdr", "raw", "bin"};
    for (const auto &format : QImageReader::supportedImageFormats())
        imageSuffixes.insert(QString::fromLatin1(format).toLower());
    QMap<QPair<QString, QString>, qsizetype> series;
    for (qsizetype i = 0; i < candidates.size(); ++i) {
        detail::checkIo(stop, progress, double(i) / candidates.size());
        const auto &candidate = candidates[i];
        const QFileInfo info(candidate.path);
        if (!info.isReadable()) {
            plan.errors.append(QStringLiteral("Cannot read %1").arg(candidate.path));
            continue;
        }
        const auto canonical = info.canonicalFilePath();
        if (seen.contains(canonical)) continue;
        seen.insert(canonical);
        const auto path = info.absoluteFilePath();
        const auto suffix = info.suffix().toLower();
        if (!candidate.explicitFile && (suffix == "raw" || suffix == "bin") && !QFileInfo::exists(path + ".json"))
            continue;
        if (imageSuffixes.contains(suffix)) {
            plan.images.append({{path}});
            continue;
        }

        DcmFileFormat file;
        const auto status = file.loadFileUntilTag(QFile::encodeName(path).constData(), EXS_Unknown,
            EGL_noChange, DCM_MaxReadLength, ERM_autoDetect, DCM_PixelData);
        auto *dataset = file.getDataset();
        Uint16 rows = 0, columns = 0, samples = 1;
        const bool dicom = status.good() && dataset->findAndGetUint16(DCM_Rows, rows).good()
            && dataset->findAndGetUint16(DCM_Columns, columns).good() && rows && columns;
        if (!dicom) {
            if (candidate.explicitFile || suffix == "dcm" || suffix == "dicom")
                plan.images.append({{path}});
            continue;
        }
        OFString studyUid, seriesUid;
        dataset->findAndGetOFString(DCM_StudyInstanceUID, studyUid);
        dataset->findAndGetOFString(DCM_SeriesInstanceUID, seriesUid);
        dataset->findAndGetUint16(DCM_SamplesPerPixel, samples);
        Sint32 frames = 1;
        if (dataset->tagExists(DCM_NumberOfFrames)) dataset->findAndGetSint32(DCM_NumberOfFrames, frames);
        const auto bytes = detail::dicomImageBytes(*dataset);
        // Multiframe and color objects remain standalone images. Geometry and
        // encoding compatibility of grouped slices are checked by the series reader.
        if (seriesUid.empty() || samples != 1 || frames != 1) {
            plan.images.append({{path}, bytes});
            continue;
        }
        const auto key = qMakePair(QString::fromLatin1(studyUid.c_str()), QString::fromLatin1(seriesUid.c_str()));
        auto group = series.find(key);
        if (group == series.end()) {
            series.insert(key, plan.images.size());
            plan.images.append({{path}, bytes});
        } else {
            if (bytes > std::numeric_limits<std::size_t>::max() - plan.images[*group].dicomBytes)
                throw std::length_error("DICOM series exceeds the addressable size");
            plan.images[*group].paths.append(path);
            plan.images[*group].dicomBytes += bytes;
        }
    }
    if (plan.images.isEmpty() && plan.errors.isEmpty() && !paths.isEmpty())
        plan.errors.append(QStringLiteral("No supported image files found in the selected paths"));
    detail::checkIo(stop, progress, 1);
    return plan;
}

void checkDicomImportMemory(const QList<ImageImport> &images, std::optional<std::size_t> available)
{
    std::size_t bytes = 0, working = 0;
    for (const auto &image : images) {
        if (image.dicomBytes > std::numeric_limits<std::size_t>::max() - bytes)
            throw std::length_error("DICOM import exceeds the addressable size");
        bytes += image.dicomBytes;
        working = std::max(working, image.dicomBytes);
    }
    if (working > std::numeric_limits<std::size_t>::max() - bytes)
        throw std::length_error("DICOM import exceeds the addressable size");
    if (bytes) detail::checkMemoryBudget(bytes + working, available);
}

} // namespace imagecxx
