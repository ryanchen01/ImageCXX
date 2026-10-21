#pragma once

#include "core/image.h"
#include "processing/processing.h"
#include <itkCommand.h>
#include <itkProcessObject.h>
#include <algorithm>

namespace imagecxx::detail {
class ProgressObserver final : public itk::Command {
public:
    using Self = ProgressObserver;
    using Pointer = itk::SmartPointer<Self>;
    itkNewMacro(Self);
    std::stop_token stop;
    Progress progress;
    void Execute(itk::Object *caller, const itk::EventObject &event) override {
        auto *process = dynamic_cast<itk::ProcessObject *>(caller);
        if (!process || !itk::ProgressEvent().CheckEvent(&event)) return;
        if (stop.stop_requested()) process->AbortGenerateDataOn();
        if (progress) progress(std::clamp(static_cast<double>(process->GetProgress()), 0.0, 1.0));
    }
    void Execute(const itk::Object *caller, const itk::EventObject &event) override {
        Execute(const_cast<itk::Object *>(caller), event);
    }
};

inline void resetMaskMetadata(ImageMetadata &metadata)
{
    metadata.intensityScale = 1;
    metadata.intensityOffset = 0;
    metadata.intensityUnit.clear();
    metadata.attributes.remove(QStringLiteral("DICOM.0028,0004"));
}
} // namespace imagecxx::detail
