#pragma once

#include "io/image_io.h"
#include "mainwindow.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QThread>
#include <functional>

namespace imagecxx::test {

inline void writeImageWithMetadata(const Image &image, const QString &path, std::stop_token stop = {},
                                   IoProgress progress = {}, bool includeFullMetadata = true)
{
    imagecxx::writeImage(image, path, stop, progress, includeFullMetadata);
}

inline void sendKey(QWidget *target, Qt::Key key, Qt::KeyboardModifiers modifiers = {})
{
    QKeyEvent overrideEvent(QEvent::ShortcutOverride, key, modifiers);
    QCoreApplication::sendEvent(target, &overrideEvent);
    QKeyEvent press(QEvent::KeyPress, key, modifiers);
    QCoreApplication::sendEvent(target, &press);
}

inline bool waitFor(const std::function<bool()> &condition)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    return condition();
}

inline bool finishJobs(MainWindow &window)
{
    QElapsedTimer timer;
    timer.start();
    while (window.jobs().busy() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    QCoreApplication::processEvents();
    return !window.jobs().busy();
}

} // namespace imagecxx::test
