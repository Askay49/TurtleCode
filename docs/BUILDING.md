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
