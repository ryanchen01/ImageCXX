#pragma once

#include <QString>

namespace imagecxx::detail {
inline QString spreadsheetCsvCell(QString value)
{
    qsizetype first = 0;
    while (first < value.size() && (value.at(first).unicode() <= 0x20 || value.at(first).isSpace())) ++first;
    bool number = false;
    value.toDouble(&number);
    // Keep a literal text prefix through spreadsheet save/reopen cycles.
    if (!number && first < value.size() && QStringLiteral("=+-@＝＋－＠").contains(value.at(first)))
        value.prepend(QStringLiteral("Text: "));
    value.replace('"', "\"\"");
    return QLatin1Char('"') + value + QLatin1Char('"');
}
} // namespace imagecxx::detail
