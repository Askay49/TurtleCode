#include "Document.hpp"

#include <utility>

Document::Document() {
    buffer_.editObserver = [this](std::size_t offset, std::size_t removed, std::size_t inserted) {
        if (hasSharedViews()) edits_.push_back({offset, removed, inserted});
        if (projectAnchors_) projectAnchors_(*this, offset, removed, inserted);
    };
    buffer_.lineEditObserver = [this](std::size_t row, std::size_t column, std::size_t removed, std::size_t inserted) {
        emit linesEdited(row, column, removed, inserted);
    };
}

void Document::appendEdits(std::vector<Edit> edits) {
    edits_ = std::move(edits);
}

void Document::publishChange(QObject* source, bool modified, bool replaced) {
    const auto edits = std::exchange(edits_, {});
    emit changed(source, modified, replaced, edits);
}

void Document::resetHistory() {
    history_.clear();
    emit historyReset();
}

