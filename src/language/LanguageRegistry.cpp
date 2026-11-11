#include "app/I18n.hpp"
#include "LanguageRegistry.hpp"

#include <QFileInfo>
#include <QSettings>

const QVector<LanguageDefinition>& LanguageRegistry::languages() {
    // Repositories are public Tree-sitter grammar projects. GrammarManager builds
    // each parser locally and stores it per user; no grammar is a build dependency.
    static const QVector<LanguageDefinition> definitions{
        {"c", "C", {"c"}, "tree-sitter-c", I18n::text("Systems"), "tree-sitter/tree-sitter-c", "tree_sitter_c", "", ""},
        {"cpp", "C++", {"h", "cc", "hh", "cpp", "hpp", "cxx", "hxx", "ino"}, "tree-sitter-cpp", I18n::text("Systems"), "tree-sitter/tree-sitter-cpp", "tree_sitter_cpp", "", ""},
        {"rust", "Rust", {"rs"}, "tree-sitter-rust", I18n::text("Systems"), "tree-sitter/tree-sitter-rust", "tree_sitter_rust", "", ""},
        {"go", "Go", {"go"}, "tree-sitter-go", I18n::text("Systems"), "tree-sitter/tree-sitter-go", "tree_sitter_go", "", ""},
        {"zig", "Zig", {"zig"}, "tree-sitter-zig", I18n::text("Systems"), "tree-sitter-grammars/tree-sitter-zig", "tree_sitter_zig", "", ""},
        {"assembly", I18n::text("Assembly"), {"s", "asm"}, "tree-sitter-asm", I18n::text("Systems"), "RubixDev/tree-sitter-asm", "tree_sitter_asm", "", ""},

        {"python", "Python", {"py", "pyw", "pyi"}, "tree-sitter-python", I18n::text("General purpose"), "tree-sitter/tree-sitter-python", "tree_sitter_python", "", ""},
        {"javascript", "JavaScript", {"js", "mjs", "cjs", "jsx"}, "tree-sitter-javascript", I18n::text("Web"), "tree-sitter/tree-sitter-javascript", "tree_sitter_javascript", "", ""},
        {"typescript", "TypeScript", {"ts", "tsx"}, "tree-sitter-typescript", I18n::text("Web"), "tree-sitter/tree-sitter-typescript", "tree_sitter_typescript", "typescript", ""},
        {"html", "HTML", {"html", "htm"}, "tree-sitter-html", I18n::text("Web"), "tree-sitter/tree-sitter-html", "tree_sitter_html", "", ""},
        {"css", "CSS", {"css"}, "tree-sitter-css", I18n::text("Web"), "tree-sitter/tree-sitter-css", "tree_sitter_css", "", ""},
        {"java", "Java", {"java"}, "tree-sitter-java", I18n::text("General purpose"), "tree-sitter/tree-sitter-java", "tree_sitter_java", "", ""},
        {"csharp", "C#", {"cs"}, "tree-sitter-c-sharp", I18n::text("General purpose"), "tree-sitter/tree-sitter-c-sharp", "tree_sitter_c_sharp", "", ""},
        {"php", "PHP", {"php", "phtml"}, "tree-sitter-php", I18n::text("Web"), "tree-sitter/tree-sitter-php", "tree_sitter_php", "php", ""},
        {"ruby", "Ruby", {"rb", "rake", "gemspec"}, "tree-sitter-ruby", I18n::text("General purpose"), "tree-sitter/tree-sitter-ruby", "tree_sitter_ruby", "", ""},
        {"swift", "Swift", {"swift"}, "tree-sitter-swift", I18n::text("General purpose"), "alex-pinkus/tree-sitter-swift", "tree_sitter_swift", "", ""},
        {"kotlin", "Kotlin", {"kt", "kts"}, "tree-sitter-kotlin", I18n::text("General purpose"), "tree-sitter-grammars/tree-sitter-kotlin", "tree_sitter_kotlin", "", ""},
        {"dart", "Dart", {"dart"}, "tree-sitter-dart", I18n::text("General purpose"), "UserNobody14/tree-sitter-dart", "tree_sitter_dart", "", ""},
        {"lua", "Lua", {"lua"}, "tree-sitter-lua", I18n::text("Scripting"), "tree-sitter-grammars/tree-sitter-lua", "tree_sitter_lua", "", ""},
        {"perl", "Perl", {"pl", "pm", "t"}, "tree-sitter-perl", I18n::text("Scripting"), "tree-sitter-perl/tree-sitter-perl", "tree_sitter_perl", "", ""},

        {"bash", "Bash / Shell", {"sh", "bash"}, "tree-sitter-bash", I18n::text("Linux & DevOps"), "tree-sitter/tree-sitter-bash", "tree_sitter_bash", "", ""},
        {"cmake", "CMake", {"cmake"}, "tree-sitter-cmake", I18n::text("Linux & DevOps"), "uyha/tree-sitter-cmake", "tree_sitter_cmake", "", ""},
        {"make", "Makefile", {"mk"}, "tree-sitter-make", I18n::text("Linux & DevOps"), "tree-sitter-grammars/tree-sitter-make", "tree_sitter_make", "", ""},
        {"meson", "Meson", {"meson"}, "tree-sitter-meson", I18n::text("Linux & DevOps"), "Decodetalkers/tree-sitter-meson", "tree_sitter_meson", "", ""},
        {"nix", "Nix", {"nix"}, "tree-sitter-nix", I18n::text("Linux & DevOps"), "nix-community/tree-sitter-nix", "tree_sitter_nix", "", ""},
        {"dockerfile", "Dockerfile", {}, "tree-sitter-dockerfile", I18n::text("Linux & DevOps"), "camdencheek/tree-sitter-dockerfile", "tree_sitter_dockerfile", "", ""},
        {"nginx", "Nginx", {"nginx"}, "tree-sitter-nginx", I18n::text("Linux & DevOps"), "opa-oz/tree-sitter-nginx", "tree_sitter_nginx", "", ""},

        {"json", "JSON", {"json"}, "tree-sitter-json", I18n::text("Configuration & data"), "tree-sitter/tree-sitter-json", "tree_sitter_json", "", ""},
        {"yaml", "YAML", {"yaml", "yml"}, "tree-sitter-yaml", I18n::text("Configuration & data"), "tree-sitter-grammars/tree-sitter-yaml", "tree_sitter_yaml", "", ""},
        {"toml", "TOML", {"toml"}, "tree-sitter-toml", I18n::text("Configuration & data"), "tree-sitter-grammars/tree-sitter-toml", "tree_sitter_toml", "", ""},
        {"xml", "XML", {"xml", "xsd", "xsl", "svg"}, "tree-sitter-xml", I18n::text("Configuration & data"), "tree-sitter-grammars/tree-sitter-xml", "tree_sitter_xml", "xml", ""},
        {"ini", "INI / EditorConfig", {"ini", "editorconfig"}, "tree-sitter-ini", I18n::text("Configuration & data"), "justinmk/tree-sitter-ini", "tree_sitter_ini", "", ""},
        {"sql", "SQL", {"sql"}, "tree-sitter-sql", I18n::text("Configuration & data"), "DerekStride/tree-sitter-sql", "tree_sitter_sql", "", ""},
        {"markdown", "Markdown", {"md", "markdown"}, "tree-sitter-markdown", I18n::text("Documentation"), "tree-sitter-grammars/tree-sitter-markdown", "tree_sitter_markdown", "tree-sitter-markdown", "split_parser"},
    };
    return definitions;
}

const LanguageDefinition* LanguageRegistry::language(const QString& id) {
    for (const auto& item : languages()) if (item.id == id) return &item;
    return nullptr;
}

const LanguageDefinition* LanguageRegistry::languageForPath(const QString& path) {
    const QFileInfo info(path);
    const QString suffix = info.suffix().toLower();
    const QString fileName = info.fileName().toLower();
    if (fileName == "cmakelists.txt") return language("cmake");
    if (fileName == "makefile" || fileName == "gnumakefile" || fileName == "bsdmakefile") return language("make");
    if (fileName == "meson.build" || fileName == "meson_options.txt") return language("meson");
    if (fileName == "dockerfile" || fileName.startsWith("dockerfile.")) return language("dockerfile");
    if (fileName == "nginx.conf") return language("nginx");
    if (fileName == ".editorconfig" || fileName == "gemfile" || fileName == "rakefile")
        return fileName == ".editorconfig" ? language("ini") : language("ruby");
    for (const auto& item : languages()) if (item.extensions.contains(suffix)) return &item;
    return nullptr;
}

bool LanguageRegistry::isEnabled(const QString& id) {
    QSettings settings;
    return settings.value("languages/" + id + "/enabled", true).toBool();
}

void LanguageRegistry::setEnabled(const QString& id, bool enabled) {
    QSettings settings;
    settings.setValue("languages/" + id + "/enabled", enabled);
    settings.sync();
}
