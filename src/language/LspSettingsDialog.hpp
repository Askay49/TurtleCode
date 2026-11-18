#pragma once
#include "LspManager.hpp"
#include <QDialog>
#include <QWidget>
#include <functional>

class QSettings;

class LspSettingsPage final : public QWidget {
public:
    explicit LspSettingsPage(LspManager* manager, QWidget* parent, bool showCompletionControls = false);
    ~LspSettingsPage() override;
    bool prepareSave() { return prepareSave_(); }
    void writeSettings(QSettings& settings) const { writeSettings_(settings); }
    bool serverChanged{false};

private:
    std::function<bool()> prepareSave_;
    std::function<void(QSettings&)> writeSettings_;
    std::function<void()> stopProbe_;
};

class LspSettingsDialog final : public QDialog {
public:
    explicit LspSettingsDialog(LspManager* manager, QWidget* parent = nullptr);
    bool serverChanged{false};
};
