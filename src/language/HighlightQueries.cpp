#include "HighlightQueries.hpp"
#include <QFile>
#include <QVector>
#include <QDebug>
static void initializeQueryResources() { Q_INIT_RESOURCE(turtle_code_queries); }
namespace {
QVector<QByteArray> patterns(const QByteArray& source) {
    QVector<QByteArray> output; int depth=0,begin=-1; bool quoted=false,escaped=false,comment=false;
    for(int i=0;i<source.size();++i) {
        const char c=source[i];
        if(comment) { if(c=='\n') comment=false; else continue; }
        if(quoted) { if(escaped) escaped=false; else if(c=='\\') escaped=true; else if(c=='"') quoted=false; continue; }
        if(c==';') { comment=true; continue; }
        if(!depth) {
            if(c=='(' || c=='[') { begin=i; depth=1; }
            else if(c=='"') { begin=i; quoted=true; }
            else if((c=='\n' || c=='\r') && begin>=0) { output.append(source.mid(begin,i-begin)); begin=-1; }
        } else if(c=='"') quoted=true;
        else if(c=='(' || c=='[') ++depth;
        else if(c==')' || c==']') {
            if(--depth==0) {
                int end=i+1;
                // Include adjacent capture names, e.g. (comment) @comment @spell.
                while(end<source.size() && source[end]!='\n' && source[end]!='\r') ++end;
                output.append(source.mid(begin,end-begin)); begin=-1; i=end-1;
            }
        }
    }
    if(begin>=0 && depth==0) output.append(source.mid(begin));
    return output;
}
}
QByteArray HighlightQueries::compatibleSource(const TSLanguage* language,const QString& id) {
    initializeQueryResources(); QByteArray source;
    const auto append=[&](const QString& name) { QFile file(":/highlighting/queries/"+name+"/highlights.scm"); if(file.open(QIODevice::ReadOnly)) source+=file.readAll()+"\n"; };
    append("c"); if(id=="cpp") append("cpp");
    // Generic declarator predicate covers arbitrarily nested pointer/array names,
    // without treating size expressions or default arguments as parameter names.
    source+=R"SCM(
((identifier) @variable.parameter
 (#has-declarator-ancestor? @variable.parameter parameter_declaration optional_parameter_declaration)
 (#set! priority 101))
)SCM";
    // Modern compatible grammars can compile the entire upstream query at once.
    // Rebuilding Tree-sitter's symbol tables once per pattern made opening even
    // tiny files unnecessarily slow. Try a whole-query compile first, adapting
    // missing anonymous alternatives before falling back to pattern isolation.
    auto candidate=source;
    for(int attempt=0;attempt<64;++attempt) {
        uint32_t offset=0; TSQueryError error{};
        TSQuery* query=ts_query_new(language,candidate.constData(),candidate.size(),&offset,&error);
        if(query) { ts_query_delete(query); return candidate; }
        if(error!=TSQueryErrorNodeType || offset>=static_cast<uint32_t>(candidate.size())) break;
        int quote=offset;
        if(candidate[quote]!='"' && quote>0 && candidate[quote-1]=='"') --quote;
        if(candidate[quote]!='"') break;
        int brackets=0; bool inString=false,escape=false,comment=false;
        for(int i=0;i<quote;++i) {
            const char c=candidate[i];
            if(comment) { if(c=='\n') comment=false; else continue; }
            if(inString) { if(escape) escape=false; else if(c=='\\') escape=true; else if(c=='"') inString=false; }
            else if(c==';') comment=true;
            else if(c=='"') inString=true;
            else if(c=='[') ++brackets;
            else if(c==']') --brackets;
        }
        if(!brackets) break;
        int end=quote+1; for(;end<candidate.size();++end) if(candidate[end]=='"' && candidate[end-1]!='\\') break;
        if(end>=candidate.size()) break;
        candidate.remove(quote,end-quote+1);
    }
    QByteArray compatible; int rejected=0;
    for(auto pattern:patterns(source)) {
        bool accepted=false;
        for(int attempt=0;attempt<64;++attempt) {
            uint32_t offset=0; TSQueryError error{};
            TSQuery* query=ts_query_new(language,pattern.constData(),pattern.size(),&offset,&error);
            if(query) { ts_query_delete(query); accepted=true; break; }
            // Missing anonymous keywords inside a list do not invalidate the
            // remaining alternatives on older compatible grammar releases.
            if(error!=TSQueryErrorNodeType || offset>=static_cast<uint32_t>(pattern.size())) break;
            int quote=offset;
            if(pattern[quote]!='"' && quote>0 && pattern[quote-1]=='"') --quote;
            if(pattern[quote]!='"') break;
            int brackets=0; bool inString=false,escape=false;
            for(int i=0;i<quote;++i) { char c=pattern[i]; if(inString) { if(escape) escape=false; else if(c=='\\') escape=true; else if(c=='"') inString=false; } else if(c=='"') inString=true; else if(c=='[') ++brackets; else if(c==']') --brackets; }
            if(!brackets) break;
            int end=quote+1; for(;end<pattern.size();++end) if(pattern[end]=='"' && pattern[end-1]!='\\') break;
            if(end>=pattern.size()) break; pattern.remove(quote,end-quote+1);
        }
        if(accepted) compatible+=pattern+"\n"; else ++rejected;
    }
    if(rejected) qInfo()<<id<<"highlight queries: omitted"<<rejected<<"patterns unsupported by this grammar";
    return compatible;
}
