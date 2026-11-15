#pragma once
#include "core/text/ChunkedFile.hpp"
#include <QMimeData>
#include <QStringDecoder>

class StoredClipboard final : public QMimeData {
public:
    std::shared_ptr<ChunkedFile> file;
    explicit StoredClipboard(std::shared_ptr<ChunkedFile> data):file(std::move(data)){}
    QStringList formats() const override {return {"text/plain"};}
    bool hasFormat(const QString& format) const override {return format=="text/plain";}
protected:
    QVariant retrieveData(const QString& format,QMetaType type) const override {
        if(format!="text/plain")return {};
        if(type==QMetaType::fromType<QByteArray>())return QByteArray::fromStdString(file->read(0,file->size()));
        QString result;QStringDecoder decoder(QStringDecoder::Utf8);
        for(std::size_t offset=0;offset<file->size();offset+=64*1024){const auto bytes=file->read(offset,std::min<std::size_t>(64*1024,file->size()-offset));result+=decoder(QByteArrayView(bytes.data(),bytes.size()));}
        return result;
    }
};
