#pragma once
#include <QColor>
#include <QObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>
class QWidget;
struct EditorTheme { QString id; QString name; QMap<QString, QColor> colors; QMap<QString,int> fontStyles; };
struct HighlightStyle { QColor color; int fontStyles{0}; }; // 1 bold, 2 italic, 4 underline, 8 strike-through
class ThemeManager : public QObject {
    Q_OBJECT
public:
    static ThemeManager& instance();
    QColor color(const QString& role) const;
    void initialize();
    void refreshAppearance();
    HighlightStyle highlightStyle(const QString& role) const;
    HighlightStyle captureStyle(const QString& capture,const QString& fallbackRole) const;
    HighlightStyle semanticStyle(const QString& type,const QStringList& modifiers) const;
    void showSettings(QWidget* parent);
signals:
    void themeChanged();
private:
    ThemeManager();
    void apply(const EditorTheme& theme);
    bool saveCustom(EditorTheme theme, QString* error);
    QVector<EditorTheme> themes_;
    EditorTheme current_;
};
