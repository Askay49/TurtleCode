# Сборка и тесты

Все команды выполняются из корня Turtle Code. Сборка редактора и тесты разделены;
отдельный каталог для тестов не нужен.

## Редактор

```sh
cmake -S . -B build
cmake --build build -j4
./build/turtle-code
```

Обычная сборка не компилирует тестовые программы и не запускает CTest.
`BUILD_TESTING=ON` по умолчанию оставляет доступными отдельные тестовые цели.
`-DBUILD_TESTING=OFF` полностью отключает их регистрацию.
Прежний параметр `TURTLE_CODE_RUN_TESTS_AFTER_BUILD` удалён; старое значение
в CMakeCache очищается при настройке.

## Тесты

Собрать тесты без запуска:

```sh
cmake --build build --target turtle-code-tests -j4
```

Запустить уже собранные тесты:

```sh
ctest --test-dir build --output-on-failure -LE memory
```

Либо одной отдельной командой собрать тесты и затем запустить их:

```sh
cmake --build build --target turtle-code-check -j4
```

Полный набор включает оконные тесты: их собственная тестовая программа
компилирует код окна, но не зависит от сборки исполняемого файла редактора.
Можно собирать и запускать отдельный тест, например:

```sh
cmake --build build --target turtle-code-debug-tests -j4
ctest --test-dir build -R '^debug$' --output-on-failure
```

Тесты памяти по-прежнему включаются явно через `TURTLE_CODE_MEMORY_TESTS=ON`;
их запуск описан в `MEMORY_TESTS.md`. Реальные адаптеры отладки требуют
переменных окружения, перечисленных в `DEBUGGING.md`.

## Пресеты Ninja

Для отдельных конфигураций Debug и Release сохранены прежние каталоги
`build/debug` и `build/release`. Не меняйте генератор поверх существующего
`build` с Makefiles.

```sh
cmake --preset debug
cmake --build --preset debug -j4
cmake --build --preset debug-tests -j4
ctest --preset debug
```

Для Release доступны аналогичные пресеты `release` и `release-tests`.

## AppImage и GitHub Actions

Workflow [AppImage](../.github/workflows/appimage.yml) автоматически собирает
`Turtle_Code-x86_64.AppImage` после push в `main`, push тега `v*` и для pull request.
Его также можно запустить вручную: GitHub → Actions → AppImage → Run workflow.
После успешной сборки откройте запуск и скачайте архив
`Turtle_Code-x86_64.AppImage` в разделе Artifacts. Внутри находятся AppImage и
файл SHA-256. Артефакт хранится 30 дней; публикация GitHub Release не выполняется.
После распаковки сделайте файл исполняемым:
`chmod +x Turtle_Code-x86_64.AppImage`.

Сборка выполняется на Ubuntu 22.04 x86_64 с GCC 12, Qt 6 и Tree-sitter.
Проверяются текстовое ядро и история; это не полный набор тестов интерфейса.
linuxdeploy с Qt-плагином упаковывает приложение и зависимости, включая
платформенные плагины X11/Wayland и библиотеки TLS. Версии упаковщиков закреплены
тегами выпусков и SHA-256 в `packaging/build-appimage.sh`.
После упаковки проверяется запуск AppImage с `--version` через Qt offscreen.
Это не проверка реальной графической сессии или всех дистрибутивов.

Для локальной упаковки на Ubuntu 22.04 установите зависимости из workflow,
соберите Release с префиксом `/usr`, затем выполните:

```sh
bash packaging/build-appimage.sh build/appimage dist
chmod +x dist/Turtle_Code-x86_64.AppImage
./dist/Turtle_Code-x86_64.AppImage
```

Для запуска AppImage может потребоваться FUSE 2. Без FUSE можно запустить
`./dist/Turtle_Code-x86_64.AppImage --appimage-extract-and-run`.
Языковые серверы, отладочные адаптеры, Git CLI и инструменты сборки грамматик
по-прежнему устанавливаются отдельно. Файл AppImage не содержит весь toolchain.

## Установка

После сборки:

```sh
cmake --install build --prefix "$HOME/.local"
```

Программа устанавливается в `bin`, ярлык — в `share/applications`,
иконки SVG и PNG — в `share/icons/hicolor` относительно выбранного префикса.
Для `~/.local` права администратора не нужны. Проверьте, что `~/.local/bin`
входит в `PATH` графической сессии. Подробности обновления меню приведены
в [README](../README.md#установка-в-linux).
