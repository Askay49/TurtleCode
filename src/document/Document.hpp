#pragma once

#include "core/text/EditHistory.hpp"
#include "core/text/PieceTreeBuffer.hpp"

#include <QString>
#include <memory>

class Document final {
public:
    Document() = default;

    PieceTreeBuffer& buffer() noexcept { return buffer_; }
    const PieceTreeBuffer& buffer() const noexcept { return buffer_; }
    EditHistory& history() noexcept { return history_; }
    const EditHistory& history() const noexcept { return history_; }
    QByteArray diskState;
    QString& filePath() noexcept { return filePath_; }
    const QString& filePath() const noexcept { return filePath_; }

private:
    PieceTreeBuffer buffer_;
    EditHistory history_;
    QString filePath_;
};

using DocumentPtr = std::shared_ptr<Document>;
