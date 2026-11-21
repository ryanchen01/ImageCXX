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

inline QString mainWindowStyleSheet()
{
    return QStringLiteral(R"(
        QMainWindow, QDialog { background: #F7F4EE; color: #18191B; }
        QWidget { font-size: 12px; }
        QMenuBar, QToolBar, QStatusBar { background: #F7F4EE; color: #18191B; }
        QMenuBar { border-bottom: 1px solid #DFD9CE; padding: 1px 4px; }
        QMenuBar::item { padding: 4px 8px; border-radius: 4px; }
        QMenuBar::item:selected { background: #EAE4D9; color: #18191B; }
        QMenu { background: #FDFBF7; color: #18191B; border: 1px solid #D5CEC2; border-radius: 6px; padding: 4px; }
        QMenu::item { padding: 5px 24px 5px 10px; border-radius: 4px; }
        QMenu::item:selected { background: #E5DEC8; color: #18191B; }
        QMenu::item:disabled { color: #8A857E; }
        QMenu::separator { height: 1px; background: #E4DED4; margin: 4px 6px; }
        QToolTip { background: #FDFBF7; color: #18191B; border: 1px solid #D5CEC2; border-radius: 4px; padding: 4px 6px; }
        QToolBar { spacing: 3px; padding: 4px 6px; border-bottom: 1px solid #D8D1C5; }
        QToolBar::separator { width: 1px; background: #DFD8CC; margin: 4px 3px; }
        QToolButton { color: #18191B; padding: 5px; border-radius: 5px; border: 1px solid transparent; font-weight: 500; }
        QToolButton:checked { background: #DFD7C8; color: #18191B; border-color: #C7BFB0; }
        QToolBar#imageTools QToolButton:enabled:hover { background: #ECE6DC; border-color: #D6CEC0; }
        QToolBar#imageTools QToolButton:enabled:pressed,
        QToolBar#imageTools QToolButton:enabled:checked:hover { background: #D5CCB8; border-color: #BEB4A4; }
        QTreeView, QListView, QTableView, QPlainTextEdit { background: #FDFBF7; alternate-background-color: #F3EFE7;
            color: #18191B; border: 0; selection-background-color: #DDD5C8; selection-color: #18191B; outline: 0; }
        QTreeView::item, QListView::item { padding: 3px 4px; border-radius: 3px; }
        QTreeView::item:hover, QListView::item:hover { background: #F0EAE0; }
        QTreeView::item:selected, QListView::item:selected { background: #DDD5C8; color: #18191B; }
        QHeaderView::section { background: #EFEAE0; color: #3D3B37; font-weight: 600; padding: 6px 8px;
            border: 0; border-bottom: 1px solid #D5CEC2; border-right: 1px solid #E4DED4; }
        QTableCornerButton::section { background: #EFEAE0; border: 0; border-bottom: 1px solid #D5CEC2; border-right: 1px solid #E4DED4; }
        QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox { background: #FDFBF7; color: #18191B;
            border: 1px solid #CFC7BA; border-radius: 5px; padding: 4px 6px; min-height: 19px; selection-background-color: #D5E2F2; selection-color: #18191B; }
        QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover, QComboBox:hover { border-color: #B5AC9E; }
        QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus { border-color: #2E69B8; background: #FFFFFF; }
        QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled, QComboBox:disabled { background: #F1ECE3; color: #8A857E; border-color: #DFD9CE; }
        QPushButton { background: #EFEAE0; color: #18191B; border: 1px solid #CFC7BA;
            border-radius: 5px; padding: 6px 12px; font-weight: 500; }
        QPushButton:hover { background: #E5DFD3; border-color: #B8AFA0; color: #18191B; }
        QPushButton:pressed { background: #D9D1C3; border-color: #A89E8E; }
        QPushButton:focus { border-color: #2E69B8; }
        QLabel, QCheckBox { color: #18191B; }
        QCheckBox { spacing: 6px; }
        QToolButton:disabled, QPushButton:disabled { color: #8A857E; background: #F2EEE6; border-color: #E0DAD0; }
        QSlider::groove:horizontal { height: 4px; background: #DED8CD; border-radius: 2px; }
        QSlider::sub-page:horizontal { background: #2E69B8; border-radius: 2px; }
        QSlider::sub-page:horizontal:disabled { background: #B8B1A6; }
        QSlider::handle:horizontal { width: 14px; height: 14px; margin: -5px 0; border-radius: 7px;
            background: #FDFBF7; border: 1px solid #8A8275; }
        QSlider::handle:horizontal:hover { background: #FFFFFF; border-color: #2E69B8; }
        QSlider::handle:horizontal:disabled { background: #EFEAE0; border-color: #C5BEB2; }
        QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
        QScrollBar::handle:vertical { background: #CFC7BA; min-height: 24px; border-radius: 3px; }
        QScrollBar::handle:vertical:hover { background: #B5AC9E; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical, QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { height: 0; background: none; }
        QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
        QScrollBar::handle:horizontal { background: #CFC7BA; min-width: 24px; border-radius: 3px; }
        QScrollBar::handle:horizontal:hover { background: #B5AC9E; }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal, QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { width: 0; background: none; }
        QWidget#sliceNavBar { background: #F7F4EE; border-top: 1px solid #DFD9CE; }
        QLabel#sliceAxisLabel { font-weight: 600; color: #4A4640; padding: 1px 6px; background: #EAE4D9; border-radius: 4px; }
        QLabel#slicePosition { color: #545049; font-weight: 500; min-width: 36px; qproperty-alignment: 'AlignRight | AlignVCenter'; }
        QStatusBar { border-top: 1px solid #E4DED4; }
        QStatusBar::item { border: 0; }
        QStatusBar QProgressBar { border: 0; background: #DFD8CC; border-radius: 3px; height: 5px; }
        QStatusBar QProgressBar::chunk { background: #2E69B8; border-radius: 3px; }
    )");
}

} // namespace imagecxx
