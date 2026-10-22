#pragma once

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

namespace imagecxx {

inline void applyTheme()
{
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    QPalette palette;
    const QColor background("#FAF7F2");
    const QColor foreground("#1A1C1F");
    palette.setColor(QPalette::Window, background);
    palette.setColor(QPalette::WindowText, foreground);
    palette.setColor(QPalette::Base, background);
    palette.setColor(QPalette::AlternateBase, QColor("#F0ECE6"));
    palette.setColor(QPalette::Text, foreground);
    palette.setColor(QPalette::Button, background);
    palette.setColor(QPalette::ButtonText, foreground);
    palette.setColor(QPalette::ToolTipBase, background);
    palette.setColor(QPalette::ToolTipText, foreground);
    palette.setColor(QPalette::Highlight, QColor("#DED8CF"));
    palette.setColor(QPalette::HighlightedText, foreground);
    palette.setColor(QPalette::Link, foreground);
    palette.setColor(QPalette::Accent, foreground);
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, role, QColor("#8B8883"));
    QApplication::setPalette(palette);
}

} // namespace imagecxx
