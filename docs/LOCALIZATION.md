# Interface languages

Open **Settings → Interface language...** (Russian: **Настройки → Язык интерфейса...**) and choose **English** or **Русский**. Save the choice, then restart Turtle Code when convenient. Changing this preference does not close the application or modify open documents. The initial default follows the system language: Russian on a Russian locale, English otherwise.

The saved language code is stored in QSettings under `interface/language`. The selected code and currently active language are separate so choosing a language does not partially retranslate an existing window.

Application text uses Qt's translation mechanism through `I18n::text()` and existing `tr()` calls. The English source text is the fallback. The Russian catalog is `translations/ru.json`, embedded as a Qt resource in the executable; installed applications do not need a separate translation file. `I18n::initialize()` runs before UI construction and before static command/theme registries are created. Protocol fields, settings keys, command IDs, file paths and exported JSON formats remain language independent. User-defined names and original theme/language brands are preserved.

Qt's own Russian translation is loaded when available. Common standard button labels are also included in the bundled catalog. Native operating-system file dialogs and external messages from clangd, compilers or the filesystem can follow the language of those external components.

To add a new translation, create a catalog with the same English keys, register its code in I18n and its endonym in the language selector, and include it in the resource list. Keep positional placeholders such as `%1` and `%2` intact. Existing saved preferences and theme/keybinding files should not be translated.

Checks include catalog loading, placeholder preservation, English fallback, persistence with deferred activation, and a separate Russian UI process that checks menus, command labels, standard buttons and language-server settings.


On the first launch after the rename to Turtle Code, previous settings, custom
themes, grammars and recovery journals are migrated automatically. Close all
previous-version editor windows before starting the renamed application.
