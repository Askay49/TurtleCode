#pragma once

#include <QFont>
#include <QFontDatabase>
#include <QSettings>
#include <algorithm>

namespace FontSettings {
enum class Area { Tree, Editor, Terminal };

inline QString key(Area area) {
    switch(area) {
    case Area::Tree: return QStringLiteral("fonts/tree/");
    case Area::Editor: return QStringLiteral("fonts/editor/");
    case Area::Terminal: return QStringLiteral("fonts/terminal/");
    }
    return {};
}

inline QFont font(Area area) {
    auto result=QFontDatabase::systemFont(area==Area::Tree ? QFontDatabase::GeneralFont : QFontDatabase::FixedFont);
    QSettings settings;
    const auto prefix=key(area);
    const auto family=settings.value(prefix+"family").toString();
    if(!family.isEmpty()) result.setFamily(family);
    const int defaultSize=result.pointSize()>0 ? result.pointSize() : 11;
    result.setPointSize(std::clamp(settings.value(prefix+"size",defaultSize).toInt(),6,96));
    return result;
}
}
