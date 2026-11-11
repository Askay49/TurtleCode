# Отладка

Новые проекты используют формат конфигураций VS Code в собственной папке:

```text
.turtle/
├── launch.json
├── tasks.json
└── settings.json
```

`tasks.json` и `settings.json` необязательны, если не нужны задачи и настройки.
Редактор читает только `.turtle`, без неявного импорта `.vscode` или настроек
VS Code пользователя. Файлы допускают комментарии `//`, `/* … */` и завершающие
запятые. Ошибки JSON показывают файл, строку и столбец. При наличии `launch.json`
он имеет приоритет; испорченный файл не заменяется другим конфигом молча.

## Запуск и настройка

1. Откройте папку проекта и исходник, сохраните изменения.
2. Ctrl+Shift+D открывает «Запуск и отладка».
3. «Настроить» создаёт `.turtle/launch.json` с шаблоном Python (`debugpy`)
   или C/C++ (`lldb`). Кнопка не перезаписывает существующие настройки.
4. Выберите конфигурацию в списке панели, нажмите F5.

Шестерёнка открывает активный файл настройки. Выбор конфигурации запоминается
локально для проекта; `launch.json`, `tasks.json`, `settings.json` перечитываются
после изменений. F5 не создаёт конфиги автоматически. Несохранённые файлы
и незавершённые операции с ними блокируют запуск.

F9 — точка останова, F10 — шаг с обходом, F11 — шаг внутрь, Shift+F11 — выход,
Shift+F5 — остановка. Доступны стек, переменные, Watch и вычисление выражений.
Точки останова и Watch сохраняются локально отдельно от переносимых файлов.

## launch.json

```json
{
    "version": "0.2.0",
    "configurations": [
        {
            "name": "openocd",
            "type": "cortex-debug",
            "request": "launch",
            "cwd": "${workspaceFolder}",
            "servertype": "openocd",
            "executable": "build/cp302.elf",
            "configFiles": ["interface/stlink.cfg", "target/stm32f4x.cfg"],
            "preLaunchTask": "Build"
        }
    ]
}
```

Имена конфигураций обязательны и уникальны. Поддерживаются `linux`-переопределения.

- `cortex-debug`: OpenOCD или external GDB server, launch/attach. Установленный
  `marus25.cortex-debug` ищется в `.vscode/extensions`, `.vscode-oss/extensions`,
  `.vscode-insiders/extensions`, выбирается самая новая доступная версия.
  Node запускает `dist/debugadapter.js`; служебные defaults, support-пути и
  порт консоли GDB-сервера заполняет редактор.
- `debugpy` и прежнее имя `python`: запуск Python-файла. Интерпретатор — поле
  `python`, затем `python.defaultInterpreterPath` в settings.json, затем python3.
  На выбранном интерпретаторе нужен debugpy. Поддерживаются args, env, cwd и
  обычные параметры запуска debugpy. Attach и запуск через module/pythonArgs
  пока не реализованы и дают ошибку.
- `lldb`: запуск/подключение через lldb-dap из PATH. Это совместимость основных
  DAP-параметров, без специфических возможностей frontend CodeLLDB.
- `cppdbg`: установленный `ms-vscode.cpptools` и его OpenDebugAD7; параметры
  передаются адаптеру. Доступность конкретного MI-debugger зависит от установки.

Внешний адаптер, toolchain и сервер должны быть установлены; редактор не запускает
сам VS Code и не устанавливает расширения автоматически. Это поддержка форматов
и перечисленных адаптеров, а не исполнение произвольных расширений VS Code.
Составные debug-сессии, SWO/RTT, графики, отдельные окна памяти/SVD, debugServer,
pipeTransport, sourceFileMap, envFile и postDebugTask пока не поддерживаются.

## settings.json

Плоские ключи в том же формате, что у VS Code:

```json
{
    "cortex-debug.armToolchainPath": "/path/to/arm-toolchain/bin",
    "cortex-debug.openocdPath": "/usr/local/bin/openocd",
    "cortex-debug.gdbPath": "/path/to/arm-toolchain/bin/arm-none-eabi-gdb",
    "cortex-debug.variableUseNaturalFormat": true
}
```

Учитываются также `cortex-debug.armToolchainPrefix`, `registerUseNaturalFormat`,
суффиксы `.linux` для путей и `python.defaultInterpreterPath`. Явные поля
конфигурации запуска имеют приоритет над настройками. Другие ключи доступны
через `${config:ключ}`; настройки интерфейса Turtle Code остаются в QSettings.

## tasks.json

```json
{
    "version": "2.0.0",
    "tasks": [
        {
            "label": "Configure",
            "type": "process",
            "command": "cmake",
            "args": ["-B", "build"],
            "problemMatcher": []
        },
        {
            "label": "Build",
            "type": "process",
            "command": "cmake",
            "args": ["--build", "build", "--parallel", "10"],
            "dependsOn": "Configure",
            "problemMatcher": []
        }
    ]
}
```

`preLaunchTask` ссылается на `label`. Поддерживаются process/shell, строковые
args, options.cwd/env/shell, глобальные options и linux-переопределения. Значения
env — строки или null (удалить переменную). Окружение задачи не меняет окружение
редактора или следующей задачи. Локальные env дополняют глобальные.

У process один элемент args — один аргумент. Shell без args исполняет строку
command как скрипт; при наличии args команда и аргументы заключаются в кавычки
для POSIX shell. По умолчанию используется `/bin/sh -c`, без профиля пользователя.
Явный options.shell позволяет выбрать другую оболочку и её аргументы.

Зависимости: одна метка dependsOn либо массив с dependsOrder: sequence.
Выполненная общая зависимость не повторяется; циклы и отсутствующие задачи
дают ошибку. Составная задача может содержать только dependsOn без command.
Параллельные зависимости, фоновые задачи, task-provider типы вроде cppbuild,
inputs и объекты аргументов с quoting пока не поддерживаются. ProblemMatcher
не разбирает диагностику: вывод виден в консоли, успех определяется кодом выхода.

Задачи выполняются асинхронно. Любая ошибка останавливает подготовку до запуска
адаптера. Программа может отсутствовать до сборки, но должна существовать
после неё. Stop и закрытие редактора отменяют подготовку; на Unix завершается
вся группа процессов сборки вместе с потомками.

## Переменные

Поддерживаются `${workspaceFolder}`, `${workspaceRoot}`, `${workspaceFolderBasename}`,
`${file}`, `${fileDirname}`, `${fileBasename}`, `${fileBasenameNoExtension}`,
`${fileExtname}`, `${fileWorkspaceFolder}`, `${relativeFile}`, `${relativeFileDirname}`,
`${userHome}`, `${cwd}`, `${pathSeparator}`, `${/}`, `${env:ИМЯ}` и `${config:ключ}`.
В текущей модели одного корня `${cwd}` — корень проекта. Для file-переменных
нужен выбранный исходник; открытие файлов настроек не меняет этот выбор.
Неизвестные переменные, отсутствующие настройки и рекурсия дают ошибку.
`${command:…}`, `${input:…}` и именованные корни multi-root не исполняются.

## Прежний debug.json

Если launch.json отсутствует, продолжает работать `.turtle/debug.json` версии 1
с типами python/cpp/dap. Сохраняются preLaunch (массивы команд без shell),
adapterArgs, launchArguments, externalSymbols и `${serverConsolePort}`.
Существующие файлы не переписываются; миграция ещё более старых настроек
из QSettings сохраняет исходный формат debug.json, чтобы не потерять параметры.

Для cp302 короткие файлы уже подготовлены. Исходная `.vscode` не изменена;
старый длинный конфиг сохранён как `.turtle/debug.pre-launch-format.json.bak`.

Форматы и обозначения сверены с документацией VS Code:
[launch.json](https://code.visualstudio.com/docs/debugtest/debugging-configuration),
[tasks.json](https://code.visualstudio.com/docs/debugtest/tasks),
[переменные](https://code.visualstudio.com/docs/reference/variables-reference).
