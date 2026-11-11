#pragma once
#include <QString>
#include <tree_sitter/api.h>
namespace HighlightQueries { QByteArray compatibleSource(const TSLanguage* language,const QString& id); }
