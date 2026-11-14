#pragma once

#include "core/text/EditHistory.hpp"
#include <QString>
#include <functional>
#include <optional>

namespace EditorConfig {
struct Settings {
    std::optional<bool> tabs;
    std::optional<int> indentSize, tabWidth;
    bool indentSizeIsTab{false};
    std::optional<std::string> eol;
    std::optional<bool> finalNewline, trimWhitespace, bom;
    QString warning;
    int indentation() const { return indentSize.value_or(indentSizeIsTab || tabs.value_or(false) ? tabSize() : 4); }
    int tabSize() const { return tabWidth.value_or(indentSize.value_or(4)); }
    bool normalizes() const { return eol || finalNewline || trimWhitespace.value_or(false) || bom; }
};
Settings resolve(const QString& filePath);
// Operates on a staged buffer/history. Reads and history payloads are chunked;
// cancellation discards the stage in the caller without changing the live text.
HistoryResult normalize(PieceTreeBuffer& buffer, EditHistory& history,
                        const std::vector<std::size_t>& cursors, const Settings& settings,
                        const std::function<void(int)>& checkpoint = {});
}
