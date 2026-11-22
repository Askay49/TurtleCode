#include "DebugTemplates.hpp"

#include "DebugConfiguration.hpp"
#include "app/I18n.hpp"

#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>

namespace {
QString t(const char* text) { return I18n::text(text); }

QString uniqueName(const QString& base, const QStringList& names) {
    auto candidate = base;
    for (int number = 2; names.contains(candidate); ++number)
        candidate = QStringLiteral("%1 (%2)").arg(base).arg(number);
    return candidate;
}

class ConfigFile {
public:
    explicit ConfigFile(QString filePath) : path(std::move(filePath)), output(path) {}

    bool read(const char* key, const char* version, QString& error) {
        existed = QFileInfo::exists(path);
        if (!existed) {
            document = QJsonObject{{"version", version}, {key, QJsonArray{}}};
            return true;
        }
        if (!DebugJson::read(path, document, error, false, &source)) return false;
        if (document.value("version").toString() != version || !document.value(key).isArray()) {
            error = t("%1 requires version %2 and a %3 array.").arg(path, version, key);
            return false;
        }
        return true;
    }

    bool stage(const char* key, const QJsonArray& entries, QString& error) {
        if (existed) {
            if (!insert(key, entries)) {
                error = t("Cannot update the %1 array in %2.").arg(key, path);
                return false;
            }
        } else {
            document[key] = entries;
            updated = QJsonDocument(document).toJson(QJsonDocument::Indented);
        }
        if (updated.size() > 1024 * 1024) {
            error = t("Configuration exceeds 1 MiB: %1").arg(path);
            return false;
        }
        if (!output.open(QIODevice::WriteOnly) || output.write(updated) != updated.size()) {
            error = path + ": " + output.errorString();
            return false;
        }
        return true;
    }

    bool check(QString& error) const {
        if (matches(source.raw, existed)) return true;
        error = t("Configuration changed on disk: %1").arg(path);
        return false;
    }

    bool commit(QString& error) {
        if (output.commit()) return true;
        error = path + ": " + output.errorString();
        return false;
    }

    bool undoCommit() const {
        if (!matches(updated, true)) return false;
        if (!existed) return QFile::remove(path);
        QSaveFile restore(path);
        return restore.open(QIODevice::WriteOnly) && restore.write(source.raw) == source.raw.size() && restore.commit();
    }

    QJsonObject document;

private:
    bool matches(const QByteArray& bytes, bool present) const {
        QFile file(path);
        if (!present) return !file.exists();
        return file.open(QIODevice::ReadOnly) && file.size() == bytes.size() && file.readAll() == bytes;
    }

    bool insert(const char* key, const QJsonArray& entries) {
        // Scan the validated document with comments masked, keeping offsets into the original bytes.
        static const QRegularExpression tokens(R"json("(?:[^"\\]++|\\.)*+"|[\[\]{}:,])json");
        const auto clean = QString::fromLatin1(source.withoutComments);
        auto stream = tokens.globalMatch(clean);
        int depth = 0;
        qsizetype open = -1, close = -1;
        bool nextIsArray = false, inArray = false;
        const auto keyLiteral = QByteArray("\"") + key + '"';
        while (stream.hasNext()) {
            const auto token = stream.next();
            const auto start = token.capturedStart();
            const auto value = token.capturedView();
            if (value.startsWith(u'"') && depth == 1) {
                auto colon = token.capturedEnd();
                while (colon < clean.size() && clean.at(colon).isSpace()) ++colon;
                if (colon == clean.size() || clean.at(colon) != u':') continue;
                const auto encoded = source.withoutComments.mid(start, value.size());
                nextIsArray = encoded == keyLiteral ||
                    QJsonDocument::fromJson("[" + encoded + "]").array().at(0).toString() == QString::fromLatin1(key);
            }
            if (depth == 1 && nextIsArray && value == u"[") {
                inArray = true;
                nextIsArray = false;
                open = start;
            }
            if (value == u"{" || value == u"[") ++depth;
            if (value == u"}" || value == u"]") --depth;
            if (inArray && depth == 1 && value == u"]") {
                close = start;
                inArray = false;
            }
        }
        if (open < 0 || close <= open) return false;

        auto last = close - 1;
        while (last > open && clean.at(last).isSpace()) --last;
        const bool needsComma = last > open && clean.at(last) != u',';
        auto lineStart = source.raw.lastIndexOf('\n', close - 1) + 1;
        const auto onClosingLine = source.raw.mid(lineStart, close - lineStart);
        const bool ownLine = lineStart > open && onClosingLine.trimmed().isEmpty();
        const QByteArray newline = source.raw.contains("\r\n") ? "\r\n" : "\n";
        const auto indent = ownLine ? onClosingLine : QByteArray("    ");
        auto lines = QJsonDocument(entries).toJson(QJsonDocument::Indented).split('\n');
        QByteArray addition = ownLine ? QByteArray{} : newline;
        for (qsizetype index = 1; index + 2 < lines.size(); ++index)
            addition += indent + lines.at(index) + newline;
        if (!ownLine) addition += indent;

        updated = source.raw;
        updated.insert(ownLine ? lineStart : close, addition);
        if (needsComma) updated.insert(last + 1, ',');
        return true;
    }

    QString path;
    QSaveFile output;
    DebugJson::Source source;
    QByteArray updated;
    bool existed = false;
};

QStringList labels(const QJsonArray& items, const char* key, bool useLinuxOverrides = false) {
    QStringList result;
    for (const auto& item : items) {
        const auto object = item.toObject();
        const auto main = object.value(key);
        const auto overrides = object.value("linux").toObject();
        result.append(useLinuxOverrides && overrides.contains(key) ? overrides.value(key).toString() : main.toString());
    }
    return result;
}

bool uniqueLabels(const QStringList& labels) {
    return !labels.contains(QString()) && QSet<QString>(labels.begin(), labels.end()).size() == labels.size();
}
}

QStringList DebugTemplates::taskLabels(const QJsonArray& tasks) {
    return labels(tasks, "label", true);
}

bool DebugTemplates::append(const QString& root, const Draft& draft, QString& name, QString& error) {
    if (!QDir(root).mkpath(".turtle")) {
        error = t("Cannot create debug configuration") + ": " + QDir(root).filePath(".turtle");
        return false;
    }
    QLockFile lock(DebugProject::path(root, ".debug-config.lock"));
    if (!lock.tryLock(0)) {
        error = t("Cannot lock the project debug configuration.");
        return false;
    }

    const auto launchPath = DebugProject::path(root, "launch.json");
    ConfigFile launch(launchPath);
    if (!launch.read("configurations", "0.2.0", error)) return false;
    auto launchNames = labels(launch.document.value("configurations").toArray(), "name");
    if (!uniqueLabels(launchNames)) {
        error = t("Configuration names must be non-empty and unique.");
        return false;
    }

    QJsonArray newEntries;
    if (!QFileInfo::exists(launchPath) && QFileInfo(DebugConfiguration::legacyPath(root)).isFile()) {
        const auto legacyName = t("Existing debug.json configuration");
        newEntries.append(QJsonObject{{"name", legacyName}, {"type", "turtle-legacy"}, {"request", "launch"}});
        launchNames.append(legacyName);
    }

    auto entry = draft.launch;
    name = uniqueName(entry.value("name").toString(), launchNames);
    entry["name"] = name;
    ConfigFile tasks(DebugProject::path(root, "tasks.json"));
    const bool writeTasks = !draft.tasks.isEmpty();
    const auto prerequisite = entry.value("preLaunchTask").toString();
    if (writeTasks || !prerequisite.isEmpty()) {
        if (!tasks.read("tasks", "2.0.0", error)) return false;
        auto existing = taskLabels(tasks.document.value("tasks").toArray());
        if (!uniqueLabels(existing)) {
            error = t("Task labels must be non-empty and unique.");
            return false;
        }
        if (writeTasks) {
            QJsonArray additions;
            for (const auto& item : draft.tasks) {
                auto task = item.toObject();
                const auto label = uniqueName(task.value("label").toString().arg(name), existing);
                task["label"] = label;
                if (!additions.isEmpty()) task["dependsOn"] = additions.last().toObject().value("label");
                additions.append(task);
                existing.append(label);
                entry["preLaunchTask"] = label;
            }
            if (!tasks.stage("tasks", additions, error)) return false;
        } else if (!existing.contains(prerequisite)) {
            error = t("Task not found: %1").arg(prerequisite);
            return false;
        }
    }

    newEntries.append(entry);
    if (!launch.stage("configurations", newEntries, error) || !launch.check(error)) return false;
    if ((writeTasks || !prerequisite.isEmpty()) && !tasks.check(error)) return false;
    if (writeTasks && !tasks.commit(error)) return false;
    if (launch.check(error) && launch.commit(error)) return true;
    if (writeTasks && !tasks.undoCommit())
        error += '\n' + t("The added tasks could not be removed from tasks.json.");
    return false;
}
