#pragma once

#include "core/image.h"

namespace imagecxx::detail {
inline void resetMaskMetadata(ImageMetadata &metadata)
{
    metadata.intensityScale = 1;
    metadata.intensityOffset = 0;
    metadata.intensityUnit.clear();
    metadata.attributes.remove(QStringLiteral("DICOM.0028,0004"));
}
} // namespace imagecxx::detail
