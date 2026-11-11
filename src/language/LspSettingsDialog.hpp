#pragma once
#include "LspManager.hpp"
#include <QDialog>

class LspSettingsDialog final : public QDialog {
public:
    explicit LspSettingsDialog(LspManager* manager, QWidget* parent = nullptr);
    bool serverChanged{false};
};
