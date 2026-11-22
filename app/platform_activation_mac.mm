#import <AppKit/AppKit.h>

namespace imagecxx {

void activateMacApplication()
{
    if (!NSApp) return;
    if (@available(macOS 14.0, *)) {
        [NSApp activate];
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
    }
}

void yieldActivationToRunningInstance()
{
    if (!NSApp) return;
    if (@available(macOS 14.0, *)) {
        if (NSString *bundleId = [[NSBundle mainBundle] bundleIdentifier])
            [NSApp yieldActivationToApplicationWithBundleIdentifier:bundleId];
    }
}

} // namespace imagecxx
