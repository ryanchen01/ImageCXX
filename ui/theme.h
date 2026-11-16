#pragma once

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

namespace imagecxx {

inline void applyTheme()
{
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    QPalette palette;
    const QColor window("#F7F4EE");
    const QColor base("#FDFBF7");
    const QColor alternate("#F1ECE3");
    const QColor button("#EFEAE0");
    const QColor foreground("#18191B");
    const QColor highlight("#DDD5C8");
    const QColor accent("#2E69B8");
    const QColor disabled("#8A857E");
    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, foreground);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, alternate);
    palette.setColor(QPalette::Text, foreground);
    palette.setColor(QPalette::PlaceholderText, disabled);
    palette.setColor(QPalette::Button, button);
    palette.setColor(QPalette::ButtonText, foreground);
    palette.setColor(QPalette::ToolTipBase, base);
    palette.setColor(QPalette::ToolTipText, foreground);
    palette.setColor(QPalette::Highlight, highlight);
    palette.setColor(QPalette::HighlightedText, foreground);
    palette.setColor(QPalette::Link, accent);
    palette.setColor(QPalette::Accent, accent);
    palette.setColor(QPalette::Mid, QColor("#D5CEC2"));
    palette.setColor(QPalette::Dark, QColor("#B9B0A2"));
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, role, disabled);
    QApplication::setPalette(palette);
}

} // namespace imagecxx
