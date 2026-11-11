#pragma once
#include <algorithm>
#include <cstddef>

struct HighlightLimits {
    std::size_t cacheRows{4096};
    std::size_t cacheBytes{8u*1024u*1024u}; // Per cache; the UI and worker each own one.
    std::size_t sourceWorkingBytes{128u*1024u*1024u};
    unsigned parseTimeMs{1500};
    unsigned queryTimeMs{50};
    std::size_t maxSpansPerLine{4096};
    std::size_t maxLineBytes{256u*1024u};
    std::size_t queryBytes{2u*1024u*1024u};
    bool operator==(const HighlightLimits&) const = default;
    HighlightLimits normalized() const {
        auto out=*this;
        out.cacheRows=std::clamp<std::size_t>(cacheRows,16,1'000'000);
        out.cacheBytes=std::clamp<std::size_t>(cacheBytes,4096,256u*1024u*1024u);
        out.sourceWorkingBytes=std::clamp<std::size_t>(sourceWorkingBytes,4096,1024u*1024u*1024u);
        out.parseTimeMs=std::clamp(parseTimeMs,1u,60'000u);
        out.queryTimeMs=std::clamp(queryTimeMs,1u,1000u);
        out.maxSpansPerLine=std::clamp<std::size_t>(maxSpansPerLine,1,65536);
        out.maxLineBytes=std::clamp<std::size_t>(maxLineBytes,128,8u*1024u*1024u);
        out.queryBytes=std::clamp<std::size_t>(queryBytes,4096,out.cacheBytes);
        return out;
    }
};
enum class HighlightLimit { None, ParseTime, QueryTime, SpanMemory, SourceMemory };
