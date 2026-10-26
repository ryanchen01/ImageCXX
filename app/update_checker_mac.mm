#include "app/update_checker.h"
#include "mainwindow.h"
#include <QApplication>
#include <QPointer>
#include <QTimer>
#import <Sparkle/Sparkle.h>

@interface ImageCXXUpdaterDelegate : NSObject <SPUUpdaterDelegate>
@property(nonatomic, copy) void (^installHandler)(void);
@end

@implementation ImageCXXUpdaterDelegate {
    QPointer<MainWindow> window_;
}
- (instancetype)initWithWindow:(MainWindow *)window
{
    self = [super init];
    if (self) window_ = window;
    return self;
}
- (BOOL)updater:(SPUUpdater *)updater shouldPostponeRelaunchForUpdate:(SUAppcastItem *)item
    untilInvokingBlock:(void (^)(void))handler
{
    Q_UNUSED(updater);
    Q_UNUSED(item);
    self.installHandler = handler;
    const bool quitOnClose = QApplication::quitOnLastWindowClosed();
    QApplication::setQuitOnLastWindowClosed(false);
    const bool closed = window_ && window_->close();
    QApplication::setQuitOnLastWindowClosed(quitOnClose);
    if (closed) QTimer::singleShot(0, qApp, [] { QCoreApplication::quit(); });
    // A canceled close postpones installation until a later accepted quit.
    return YES;
}
@end

void imagecxx::UpdateChecker::checkWithSparkle(MainWindow &window, bool manual)
{
    static SPUStandardUpdaterController *controller = [&window] {
        ImageCXXUpdaterDelegate *delegate = [[ImageCXXUpdaterDelegate alloc] initWithWindow:&window];
        SPUStandardUpdaterController *updater = [[SPUStandardUpdaterController alloc]
            initWithStartingUpdater:NO updaterDelegate:delegate userDriverDelegate:nil];
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, qApp, [delegate] {
            if (delegate.installHandler) {
                const auto handler = delegate.installHandler;
                delegate.installHandler = nil;
                handler();
            }
        });
        [updater startUpdater];
        return updater;
    }();
    if (manual) [controller checkForUpdates:nil];
}
