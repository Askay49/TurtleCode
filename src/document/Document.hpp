#pragma once

#include "core/text/EditHistory.hpp"
#include "core/text/PieceTreeBuffer.hpp"

#include <QString>
#include <QObject>
#include <memory>
#include <vector>

class Document final : public QObject {
    Q_OBJECT
public:
    struct Edit { std::size_t offset, removed, inserted; };
    Document();

    PieceTreeBuffer& buffer() noexcept { return buffer_; }
    const PieceTreeBuffer& buffer() const noexcept { return buffer_; }
    EditHistory& history() noexcept { return history_; }
    const EditHistory& history() const noexcept { return history_; }
    QByteArray diskState;
    bool keepVimMarks{true};
    QString& filePath() noexcept { return filePath_; }
    const QString& filePath() const noexcept { return filePath_; }
    bool& busy() noexcept { return busy_; }
    bool& vimFinalNewline() noexcept { return vimFinalNewline_; }
    void attachView() { ++viewCount_; }
    bool detachView() { return --viewCount_ == 0; }
    bool hasSharedViews() const { return viewCount_ > 1; }
    using AnchorProjector = void (*)(Document&, std::size_t, std::size_t, std::size_t);
    void setAnchorProjector(AnchorProjector projector) { projectAnchors_ = projector; }
    void appendEdits(std::vector<Edit> edits);
    void publishChange(QObject* source, bool modified, bool replaced);
    void resetHistory();

signals:
    void linesEdited(std::size_t row, std::size_t column, std::size_t removed, std::size_t inserted);
    void changed(QObject* source, bool modified, bool replaced, const std::vector<Edit>& edits);
    void busyChanged(bool busy);
    void historyReset();

private:
    PieceTreeBuffer buffer_;
    EditHistory history_;
    QString filePath_;
    std::vector<Edit> edits_;
    int viewCount_{0};
    bool busy_{false};
    bool vimFinalNewline_{false};
    AnchorProjector projectAnchors_{nullptr};
};

using DocumentPtr = std::shared_ptr<Document>;
