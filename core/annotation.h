#pragma once
#include "core/roi.h"

namespace imagecxx {
// Display-only snapshot, independent of editable/saved scientific ROIs.
struct Annotation {
    Roi geometry;
    QString text;
};
}
