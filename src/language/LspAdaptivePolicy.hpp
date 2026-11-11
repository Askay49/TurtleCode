#pragma once
#include <algorithm>
#include <cstddef>
#include <QtGlobal>

// Per-document automatic workload policy. Manual commands use a separate
// transport-copy budget, independently of semantic highlighting pressure.
class LspAdaptivePolicy {
public:
    enum class Mode { None, Full, Range };
    static constexpr std::size_t MiB=1024u*1024u;
    static bool transportFits(std::size_t bytes) { return bytes<=32*MiB; }
    static bool automaticDocument(std::size_t bytes) { return bytes<=16*MiB; }
    int syncDelay(std::size_t bytes) const { return std::max(bytes>2*MiB?1000:200,std::min(2000,static_cast<int>(averageMs_*2))); }
    bool automaticFeatures(std::size_t bytes,qint64 now) const { return bytes<=2*MiB && now>=pausedUntil_ && averageMs_<600; }
    Mode mode(std::size_t bytes,bool range,bool full,qint64 now) const {
        if(!automaticDocument(bytes) || now<pausedUntil_) return Mode::None;
        if(bytes>2*MiB) return range ? Mode::Range : Mode::None;
        if(range && (bytes>512*1024 || averageMs_>150 || failures_)) return Mode::Range;
        return full ? Mode::Full : range ? Mode::Range : Mode::None;
    }
    int delay(std::size_t bytes) const { return std::clamp(std::max(bytes>2*MiB?1000:200,static_cast<int>(averageMs_*3)),200,4000); }
    qint64 remainingPause(qint64 now) const { return std::max<qint64>(0,pausedUntil_-now); }
    void observe(qint64 elapsedMs,std::size_t tokens,bool success,qint64 now) {
        // One successful cold-start transfer is not evidence of sustained overload.
        averageMs_=averageMs_==0 ? (success?std::min<qint64>(elapsedMs,1000):elapsedMs) : averageMs_*0.7+elapsedMs*0.3;
        failures_=success ? 0 : failures_+1;
        pressure_=(elapsedMs>600 || tokens>50000 || !success) ? pressure_+1 : std::max(0,pressure_-1);
        if((elapsedMs>2000 && !success) || tokens>100000 || failures_>=2 || pressure_>=3) {
            pausedUntil_=now+30000; pressure_=0; failures_=0;
        }
    }
    void reset() { *this=LspAdaptivePolicy{}; }
private:
    double averageMs_{0}; int failures_{0},pressure_{0}; qint64 pausedUntil_{0};
};
