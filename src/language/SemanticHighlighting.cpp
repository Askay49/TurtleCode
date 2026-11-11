#include "SemanticHighlighting.hpp"
#include <cmath>
#include <limits>
namespace {
bool unsignedInteger(const QJsonValue& value,quint32& out) {
    if (!value.isDouble()) return false;
    const double number=value.toDouble();
    if (number<0 || number>std::numeric_limits<quint32>::max() || std::floor(number)!=number) return false;
    out=static_cast<quint32>(number); return true;
}
}
SemanticHighlightResult SemanticHighlighting::decode(const QJsonArray& data,const QJsonObject& legend,const QString& text,int originLine,std::size_t maxTokens) {
    SemanticHighlightResult result;
    auto fail=[&](const QString& error) { result.lines.clear(); result.error=error; return result; };
    if(static_cast<std::size_t>(data.size()/5)>maxTokens) return fail("Semantic token budget exceeded.");
    if (data.size()%5) return fail("Semantic token data is not a sequence of five integers.");
    const auto types=legend.value("tokenTypes").toArray(), modifiers=legend.value("tokenModifiers").toArray();
    if (types.isEmpty() || modifiers.size()>32) return fail("Invalid semantic token legend.");
    const auto lines=text.split('\n'); quint64 line=0,start=0;
    for (int i=0;i<data.size();i+=5) {
        quint32 dl,ds,length,type,flags;
        if (!unsignedInteger(data[i],dl) || !unsignedInteger(data[i+1],ds) || !unsignedInteger(data[i+2],length) ||
            !unsignedInteger(data[i+3],type) || !unsignedInteger(data[i+4],flags)) return fail("Invalid semantic token integer.");
        line+=dl; start=dl?ds:start+ds;
        if (!length || (line<static_cast<quint64>(originLine) || line-static_cast<quint64>(originLine)>=static_cast<quint64>(lines.size())) || type>=static_cast<quint32>(types.size()) || !types[type].isString()) return fail("Invalid semantic token position or type.");
        auto row=lines[static_cast<int>(line)-originLine]; if (row.endsWith('\r')) row.chop(1);
        const quint64 end=start+length;
        if (end>static_cast<quint64>(row.size()) || (start<static_cast<quint64>(row.size()) && row[static_cast<int>(start)].isLowSurrogate()) ||
            (end<static_cast<quint64>(row.size()) && row[static_cast<int>(end)].isLowSurrogate())) return fail("Semantic token splits a UTF-16 character or exceeds its line.");
        if (modifiers.size()<32 && (flags>>modifiers.size())) return fail("Unknown semantic token modifier index.");
        SemanticToken token{static_cast<int>(line),static_cast<int>(start),static_cast<int>(length),types[type].toString(),{}};
        for(int bit=0;bit<modifiers.size();++bit) if(flags&(quint32(1)<<bit)) {
            if (!modifiers[bit].isString()) return fail("Invalid semantic token modifier.");
            token.modifiers.append(modifiers[bit].toString());
        }
        auto& output=result.lines[token.line];
        if (!output.isEmpty() && output.last().start+output.last().length>token.start) return fail("Overlapping semantic tokens were not negotiated.");
        output.append(std::move(token));
    }
    result.valid=true; return result;
}
void SemanticHighlighting::applyEdit(QHash<int,QVector<SemanticToken>>& lines,int firstLine,int removedLines,int insertedLines) {
    QHash<int,QVector<SemanticToken>> adjusted;
    for(auto it=lines.begin();it!=lines.end();++it) {
        if(it.key()>=firstLine && it.key()<=firstLine+removedLines) continue;
        int row=it.key(); if(row>firstLine+removedLines) row+=insertedLines-removedLines;
        auto tokens=std::move(it.value()); for(auto& token:tokens) token.line=row;
        adjusted.insert(row,std::move(tokens));
    }
    lines=std::move(adjusted);
}
