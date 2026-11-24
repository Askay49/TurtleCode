cmake_minimum_required(VERSION 3.25)
project(TurtleCode VERSION 0.11.1 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)
include(GNUInstallDirs)
option(TURTLE_CODE_MEMORY_TESTS "Register opt-in Linux memory regression tests" OFF)
option(TURTLE_CODE_BUILD_BENCHMARKS "Build text-core microbenchmarks" OFF)
option(TURTLE_CODE_BUILD_APP "Build the Qt desktop application" ON)
option(TURTLE_CODE_ENABLE_TREE_SITTER "Enable Tree-sitter syntax parsing (runtime required; grammars are installed by the IDE)" ON)
set(TURTLE_CODE_CPP_GRAMMAR_DIR "" CACHE PATH "Optional tree-sitter-cpp source checkout used only for syntax tests")

add_library(turtle-code-text-core STATIC
    src/core/text/PieceTreeBuffer.cpp
    src/core/text/PieceTreeBuffer.hpp
    src/core/text/EditHistory.cpp
    src/core/text/EditHistory.hpp
)
target_include_directories(turtle-code-text-core PUBLIC src)
target_compile_options(turtle-code-text-core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)

if(TURTLE_CODE_BUILD_APP)
    configure_file(src/app/Version.hpp.in generated/app/Version.hpp @ONLY)
    find_package(Qt6 6.2.4 REQUIRED COMPONENTS Core Gui Widgets Network Concurrent Test)
    add_library(turtle-code-vim STATIC src/editor/VimController.cpp src/editor/VimState.cpp src/editor/VimKeymaps.cpp src/editor/VimBlock.cpp src/editor/VimMotions.cpp src/editor/VimNormal.cpp src/editor/VimEx.cpp src/editor/VimSearch.cpp src/editor/UnicodeNavigation.cpp)
    target_include_directories(turtle-code-vim PUBLIC src)
    target_link_libraries(turtle-code-vim PUBLIC turtle-code-text-core Qt6::Core Qt6::Gui)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PCRE2_VIM REQUIRED IMPORTED_TARGET libpcre2-8>=10.34)
    target_link_libraries(turtle-code-vim PUBLIC PkgConfig::PCRE2_VIM)
    pkg_check_modules(LIBGIT2 REQUIRED IMPORTED_TARGET libgit2>=1.1)
    pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
    pkg_check_modules(PCRE2 REQUIRED IMPORTED_TARGET libpcre2-16)
    add_library(turtle-code-workspace STATIC src/project/WorkspaceFiles.cpp src/project/WorkspaceIndex.cpp src/project/DirectoryListing.cpp src/search/SearchMatches.cpp src/search/SearchScanner.cpp)
    set_target_properties(turtle-code-workspace PROPERTIES AUTOMOC ON)
    target_include_directories(turtle-code-workspace PUBLIC src)
    target_link_libraries(turtle-code-workspace PUBLIC Qt6::Core Qt6::Concurrent PkgConfig::SQLITE3 PkgConfig::PCRE2 PkgConfig::LIBGIT2)
    find_path(EDITORCONFIG_INCLUDE_DIR editorconfig/editorconfig.h REQUIRED)
    find_library(EDITORCONFIG_LIBRARY NAMES editorconfig REQUIRED)
    add_library(turtle-code-editorconfig STATIC src/editor/EditorConfig.cpp src/editor/EditorConfig.hpp)
    target_include_directories(turtle-code-editorconfig PUBLIC src PRIVATE "${EDITORCONFIG_INCLUDE_DIR}")
    target_link_libraries(turtle-code-editorconfig PUBLIC turtle-code-text-core Qt6::Core "${EDITORCONFIG_LIBRARY}")
    add_library(turtle-code-git-backend STATIC src/git/GitBackend.cpp src/git/GitBackend.hpp)
    target_include_directories(turtle-code-git-backend PUBLIC src)
    target_link_libraries(turtle-code-git-backend PUBLIC Qt6::Core PkgConfig::LIBGIT2)
    target_compile_options(turtle-code-git-backend PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>)
    # Qt 6.2 does not provide qt_standard_project_setup().
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTOUIC ON)

    add_library(turtle-code-i18n STATIC src/app/I18n.cpp src/app/I18n.hpp)
    target_include_directories(turtle-code-i18n PUBLIC src "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(turtle-code-i18n PUBLIC Qt6::Core)
    target_link_libraries(turtle-code-vim PRIVATE turtle-code-i18n)
    qt_add_resources(turtle-code-i18n turtle_code_translations
        PREFIX "/i18n" BASE "${CMAKE_CURRENT_SOURCE_DIR}/translations"
        FILES translations/ru.json)

    add_library(turtle-code-themes STATIC src/app/CommandPalette.cpp src/app/CommandPalette.hpp src/app/ThemeManager.cpp src/app/ThemeManager.hpp
        src/app/WorkspaceBackground.cpp src/app/WorkspaceBackground.hpp
        src/app/ShortcutManager.cpp src/app/ShortcutManager.hpp
        src/language/SemanticHighlighting.cpp src/language/SemanticHighlighting.hpp)
    target_include_directories(turtle-code-themes PUBLIC src)
    target_link_libraries(turtle-code-themes PUBLIC Qt6::Widgets turtle-code-workspace PRIVATE Qt6::Concurrent)
    add_library(turtle-code-icons STATIC src/app/Codicons.cpp src/app/Codicons.hpp
        src/app/FontIcon.cpp src/app/FontIcon.hpp
        src/app/FileIcons.cpp src/app/FileIcons.hpp)
    target_include_directories(turtle-code-icons PUBLIC src)
    target_link_libraries(turtle-code-icons PUBLIC Qt6::Widgets)
    target_link_libraries(turtle-code-themes PUBLIC turtle-code-icons)
    qt_add_resources(turtle-code-icons turtle_code_codicons
        PREFIX "/codicons" BASE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/codicons"
        FILES third_party/codicons/codicon.ttf third_party/codicons/mapping.json)
    qt_add_resources(turtle-code-icons turtle_code_seti
        PREFIX "/seti" BASE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/seti"
        FILES third_party/seti/seti.ttf third_party/seti/vs-seti-icon-theme.json
              third_party/seti/language-associations.json)
    qt_add_resources(turtle-code-themes turtle_code_highlighting
        PREFIX "/highlighting" BASE "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES themes/tokyo-night-vscode.json
              themes/catppuccin-latte.json themes/catppuccin-frappe.json
              themes/catppuccin-macchiato.json themes/catppuccin-mocha.json)

    if(BUILD_TESTING)
        add_executable(turtle-code-vim-search-tests EXCLUDE_FROM_ALL tests/VimSearchTests.cpp)
        target_link_libraries(turtle-code-vim-search-tests PRIVATE turtle-code-vim Qt6::Core)
        target_compile_options(turtle-code-vim-search-tests PRIVATE -UNDEBUG)
        add_test(NAME vim-search-core COMMAND turtle-code-vim-search-tests)
        set_tests_properties(vim-search-core PROPERTIES TIMEOUT 90)
        add_executable(turtle-code-workspace-scale-tests EXCLUDE_FROM_ALL tests/WorkspaceScaleTests.cpp)
        target_link_libraries(turtle-code-workspace-scale-tests PRIVATE turtle-code-workspace)
        target_compile_options(turtle-code-workspace-scale-tests PRIVATE -UNDEBUG)
        add_test(NAME workspace-scale COMMAND turtle-code-workspace-scale-tests)
        set_tests_properties(workspace-scale PROPERTIES TIMEOUT 90)
        add_executable(turtle-code-editorconfig-tests EXCLUDE_FROM_ALL tests/EditorConfigTests.cpp)
        target_link_libraries(turtle-code-editorconfig-tests PRIVATE turtle-code-editorconfig)
        target_compile_options(turtle-code-editorconfig-tests PRIVATE -UNDEBUG)
        add_test(NAME editorconfig COMMAND turtle-code-editorconfig-tests)
        set_tests_properties(editorconfig PROPERTIES TIMEOUT 30)
        add_executable(turtle-code-file-icon-tests EXCLUDE_FROM_ALL tests/FileIconsTests.cpp)
        target_link_libraries(turtle-code-file-icon-tests PRIVATE turtle-code-icons)
        target_compile_options(turtle-code-file-icon-tests PRIVATE -UNDEBUG)
        add_test(NAME file-icons COMMAND turtle-code-file-icon-tests)
        set_tests_properties(file-icons PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_executable(turtle-code-icon-tests EXCLUDE_FROM_ALL tests/CodiconsTests.cpp)
        target_link_libraries(turtle-code-icon-tests PRIVATE turtle-code-icons)
        target_compile_options(turtle-code-icon-tests PRIVATE -UNDEBUG)
        add_test(NAME codicons COMMAND turtle-code-icon-tests)
        set_tests_properties(codicons PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_executable(turtle-code-i18n-tests EXCLUDE_FROM_ALL tests/I18nTests.cpp)
        target_link_libraries(turtle-code-i18n-tests PRIVATE turtle-code-i18n)
        target_compile_options(turtle-code-i18n-tests PRIVATE -UNDEBUG)
        add_test(NAME translations COMMAND turtle-code-i18n-tests)
        add_executable(turtle-code-shortcut-tests EXCLUDE_FROM_ALL tests/ShortcutTests.cpp)
        target_link_libraries(turtle-code-shortcut-tests PRIVATE turtle-code-themes Qt6::Test)
        target_compile_options(turtle-code-shortcut-tests PRIVATE -UNDEBUG)
        add_test(NAME shortcuts COMMAND turtle-code-shortcut-tests)
        set_tests_properties(shortcuts PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_executable(turtle-code-theme-tests EXCLUDE_FROM_ALL tests/ThemeTests.cpp)
        target_link_libraries(turtle-code-theme-tests PRIVATE turtle-code-themes turtle-code-icons)
        target_compile_options(turtle-code-theme-tests PRIVATE -UNDEBUG)
        add_test(NAME themes COMMAND turtle-code-theme-tests)
        set_tests_properties(themes PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()

    # Vendored MIT libvterm: offline, reproducible builds, no higher Qt minimum.
    add_library(turtle-code-vterm STATIC
        third_party/libvterm/src/encoding.c third_party/libvterm/src/keyboard.c
        third_party/libvterm/src/mouse.c third_party/libvterm/src/parser.c
        third_party/libvterm/src/pen.c third_party/libvterm/src/screen.c
        third_party/libvterm/src/state.c third_party/libvterm/src/unicode.c
        third_party/libvterm/src/vterm.c)
    set_target_properties(turtle-code-vterm PROPERTIES AUTOMOC OFF AUTOUIC OFF)
    target_include_directories(turtle-code-vterm PUBLIC third_party/libvterm/include)
    add_library(turtle-code-terminal STATIC
        src/terminal/TerminalSession.cpp src/terminal/TerminalSession.hpp
        src/terminal/TerminalWidget.cpp src/terminal/TerminalWidget.hpp
        src/terminal/TerminalPanel.cpp src/terminal/TerminalPanel.hpp)
    target_include_directories(turtle-code-terminal PUBLIC src)
    target_link_libraries(turtle-code-terminal PUBLIC turtle-code-vterm turtle-code-themes turtle-code-icons turtle-code-i18n Qt6::Widgets)
    target_compile_options(turtle-code-terminal PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>)

    add_library(turtle-code-workspace-edits STATIC src/language/WorkspaceEdits.cpp src/language/WorkspaceEdits.hpp)
    target_include_directories(turtle-code-workspace-edits PUBLIC src)
    target_link_libraries(turtle-code-workspace-edits PUBLIC Qt6::Core)

    qt_add_executable(turtle-code
        MACOSX_BUNDLE
        src/main.cpp
        src/document/RecoveryStore.cpp
        src/document/RecoveryStore.hpp
        src/document/Document.cpp
        src/document/Document.hpp
        src/app/MainWindow.cpp
        src/app/MainWindowSettings.cpp
        src/app/MainWindowSymbols.cpp
        src/app/MainWindowVim.cpp
        src/app/MainWindowWorkspace.cpp
        src/app/SavedProjectsPage.cpp
        src/app/SavedProjectsPage.hpp
        src/editor/EditorPaneWorkspace.cpp
        src/editor/VimDiffSession.cpp
        src/app/MainWindow.hpp
        src/debug/DapClient.cpp
        src/debug/DapClient.hpp
        src/debug/DebugPanel.cpp
        src/debug/DebugPanel.hpp
        src/debug/DebugTemplateDialog.cpp
        src/debug/DebugTemplates.cpp
        src/debug/DebugTemplates.hpp
        src/debug/DebugViews.hpp
        src/debug/DebugConfiguration.hpp
        src/project/ProjectExplorer.cpp
        src/project/ProjectExplorer.hpp
        src/project/ProjectExplorerTransfer.cpp
        src/search/ProjectSearchService.cpp
        src/search/ProjectSearchService.hpp
        src/search/SearchPanel.cpp
        src/search/SearchPanel.hpp
        src/git/GitService.cpp
        src/git/GitService.hpp
        src/git/GitPanel.cpp
        src/git/GitPanel.hpp
        src/git/GitDiffView.cpp
        src/git/GitDiffView.hpp
        src/git/GitDiffSourcePane.cpp
        src/git/GitDiffSourcePane.hpp
        src/editor/EditorWidget.cpp
        src/editor/EditorWidget.hpp
        src/editor/UnicodeNavigation.cpp
        src/editor/UnicodeNavigation.hpp
        src/editor/SyntaxHighlighter.cpp
        src/editor/SyntaxHighlighter.hpp
        src/language/LanguageRegistry.cpp
        src/language/LanguageRegistry.hpp
        src/language/GrammarManager.cpp
        src/language/GrammarManager.hpp
        src/language/LspClient.cpp
        src/language/LspClient.hpp
        src/language/LspManager.cpp
        src/language/LspManager.hpp
        src/language/LspSettingsDialog.cpp
        src/language/LspSettingsDialog.hpp
    )
    qt_add_resources(turtle-code turtle_code_application_icon
        PREFIX "/app" BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/256x256/apps"
        FILES assets/icons/256x256/apps/turtle-code.png)
    target_include_directories(turtle-code PRIVATE src)
    if(TURTLE_CODE_ENABLE_TREE_SITTER)
        find_path(TREE_SITTER_INCLUDE_DIR tree_sitter/api.h REQUIRED)
        find_library(TREE_SITTER_LIBRARY NAMES tree-sitter tree_sitter REQUIRED)
        include(CheckCXXSymbolExists)
        include(CMakePushCheckState)
        cmake_push_check_state(RESET)
        set(CMAKE_REQUIRED_INCLUDES "${TREE_SITTER_INCLUDE_DIR}")
        set(CMAKE_REQUIRED_LIBRARIES "${TREE_SITTER_LIBRARY}")
        check_cxx_symbol_exists(ts_parser_parse_with_options "tree_sitter/api.h"
            TURTLE_CODE_TS_PARSE_WITH_OPTIONS)
        if(NOT TURTLE_CODE_TS_PARSE_WITH_OPTIONS)
            check_cxx_symbol_exists(ts_parser_set_timeout_micros "tree_sitter/api.h"
                TURTLE_CODE_TS_PARSE_TIMEOUT)
            if(NOT TURTLE_CODE_TS_PARSE_TIMEOUT)
                message(FATAL_ERROR "Tree-sitter must support parse progress callbacks or parser timeouts")
            endif()
        endif()
        cmake_pop_check_state()
        add_library(turtle-code-query-data STATIC src/language/HighlightQueries.cpp src/language/HighlightQueries.hpp)
        target_compile_definitions(turtle-code-query-data PUBLIC
            TURTLE_CODE_TS_PARSE_WITH_OPTIONS=$<BOOL:${TURTLE_CODE_TS_PARSE_WITH_OPTIONS}>)
        target_include_directories(turtle-code-query-data PUBLIC src "${TREE_SITTER_INCLUDE_DIR}")
        target_link_libraries(turtle-code-query-data PUBLIC Qt6::Core "${TREE_SITTER_LIBRARY}")
        qt_add_resources(turtle-code-query-data turtle_code_queries PREFIX "/highlighting" BASE "${CMAKE_CURRENT_SOURCE_DIR}"
            FILES queries/c/highlights.scm queries/cpp/highlights.scm)
        target_sources(turtle-code PRIVATE src/language/TreeSitterCpp.cpp src/language/TreeSitterCpp.hpp)
        target_include_directories(turtle-code PRIVATE "${TREE_SITTER_INCLUDE_DIR}")
        target_compile_definitions(turtle-code PRIVATE TURTLE_CODE_WITH_TREE_SITTER=1)
        target_link_libraries(turtle-code PRIVATE "${TREE_SITTER_LIBRARY}")
    endif()

    set_target_properties(turtle-code PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.turtlecode.turtlecode"
        MACOSX_BUNDLE_BUNDLE_NAME "Turtle Code"
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    )

    target_link_libraries(turtle-code PRIVATE
        turtle-code-terminal
        turtle-code-git-backend
        turtle-code-icons
        turtle-code-themes
        turtle-code-workspace-edits
        turtle-code-text-core
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Network
        Qt6::Concurrent
    )
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_package(Fontconfig QUIET)
        if(Fontconfig_FOUND)
            target_link_libraries(turtle-code PRIVATE Fontconfig::Fontconfig)
            target_compile_definitions(turtle-code PRIVATE TURTLE_CODE_WITH_FONTCONFIG)
        endif()
    endif()
    target_compile_options(turtle-code PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
    )

    if(BUILD_TESTING)
        add_executable(turtle-code-debug-input-fixture EXCLUDE_FROM_ALL examples/debug/input.cpp)
        target_compile_options(turtle-code-debug-input-fixture PRIVATE -g -O0)
        add_executable(turtle-code-debug-tests EXCLUDE_FROM_ALL tests/DebugTests.cpp src/debug/DapClient.cpp
            src/debug/DebugPanel.cpp src/debug/DebugTemplateDialog.cpp src/debug/DebugTemplates.cpp)
        target_include_directories(turtle-code-debug-tests PRIVATE src)
        target_link_libraries(turtle-code-debug-tests PRIVATE turtle-code-terminal turtle-code-icons turtle-code-i18n Qt6::Widgets Qt6::Network Qt6::Test)
        add_dependencies(turtle-code-debug-tests turtle-code-debug-input-fixture)
        target_compile_definitions(turtle-code-debug-tests PRIVATE DEBUG_INPUT_PROGRAM="$<TARGET_FILE:turtle-code-debug-input-fixture>" DEBUG_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME debug COMMAND turtle-code-debug-tests)
        set_tests_properties(debug PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 90)
        add_executable(turtle-code-terminal-tests EXCLUDE_FROM_ALL tests/TerminalTests.cpp)
        target_link_libraries(turtle-code-terminal-tests PRIVATE turtle-code-terminal Qt6::Test)
        add_test(NAME terminal COMMAND turtle-code-terminal-tests)
        set_tests_properties(terminal PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 45)
        get_target_property(TURTLE_CODE_WINDOW_TEST_SOURCES turtle-code SOURCES)
        list(FILTER TURTLE_CODE_WINDOW_TEST_SOURCES EXCLUDE REGEX "(^|/)main\\.cpp$")
        get_target_property(TURTLE_CODE_WINDOW_TEST_LIBRARIES turtle-code LINK_LIBRARIES)
        add_executable(turtle-code-window-tests EXCLUDE_FROM_ALL tests/MainWindowTests.cpp ${TURTLE_CODE_WINDOW_TEST_SOURCES})
        target_include_directories(turtle-code-window-tests PRIVATE src)
        target_link_libraries(turtle-code-window-tests PRIVATE ${TURTLE_CODE_WINDOW_TEST_LIBRARIES} Qt6::Test)
        target_compile_definitions(turtle-code-window-tests PRIVATE LSP_FAKE_SERVER="${CMAKE_CURRENT_SOURCE_DIR}/tests/LspFakeServer.py")
        target_compile_options(turtle-code-window-tests PRIVATE -UNDEBUG)
        if(TURTLE_CODE_ENABLE_TREE_SITTER)
            target_include_directories(turtle-code-window-tests PRIVATE "${TREE_SITTER_INCLUDE_DIR}")
            target_compile_definitions(turtle-code-window-tests PRIVATE TURTLE_CODE_WITH_TREE_SITTER=1)
        endif()
        if(TURTLE_CODE_MEMORY_TESTS)
            if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
                message(FATAL_ERROR "Memory regression tests require Linux /proc")
            endif()
            find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
            foreach(memory_size 10 100 500)
                add_test(NAME memory-${memory_size}mib
                    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/MemoryRegression.py
                        --binary $<TARGET_FILE:turtle-code-window-tests>
                        --size-mib ${memory_size} --cycles 6
                        --report ${CMAKE_BINARY_DIR}/memory-reports/${memory_size}mib)
                set_tests_properties(memory-${memory_size}mib PROPERTIES
                    LABELS "memory" RUN_SERIAL TRUE TIMEOUT 1260)
            endforeach()
        endif()
        add_test(NAME russian-interface COMMAND turtle-code-window-tests --russian-ui)
        set_tests_properties(russian-interface PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
        add_executable(turtle-code-recovery-tests EXCLUDE_FROM_ALL tests/RecoveryTests.cpp src/document/RecoveryStore.cpp)
        target_include_directories(turtle-code-recovery-tests PRIVATE src)
        target_link_libraries(turtle-code-recovery-tests PRIVATE turtle-code-text-core Qt6::Core)
        target_compile_options(turtle-code-recovery-tests PRIVATE -UNDEBUG)
        add_test(NAME terminal-window COMMAND turtle-code-window-tests --terminal)
        set_tests_properties(terminal-window PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 45)
        add_test(NAME git-window COMMAND turtle-code-window-tests --git)
        set_tests_properties(git-window PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 45)
        add_test(NAME project-scale-window COMMAND turtle-code-window-tests --project-scale)
        set_tests_properties(project-scale-window PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
        add_test(NAME vim-insert-window COMMAND turtle-code-window-tests --vim-insert)
        set_tests_properties(vim-insert-window PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 45)
        add_test(NAME vim-stage11-window COMMAND turtle-code-window-tests --vim-stage11)
        set_tests_properties(vim-stage11-window PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 45)
        add_test(NAME vim-window COMMAND turtle-code-window-tests --vim)
        add_test(NAME vim-workspace COMMAND turtle-code-window-tests --vim-workspace)
        set_tests_properties(vim-workspace PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 45)
        set_tests_properties(vim-window PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 45)
        add_test(NAME command-palette COMMAND turtle-code-window-tests --commands)
        set_tests_properties(command-palette PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
        add_test(NAME formatting COMMAND turtle-code-window-tests --formatting)
        set_tests_properties(formatting PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 45)
        add_test(NAME external-file-changes COMMAND turtle-code-window-tests --external-files)
        set_tests_properties(external-file-changes PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
        add_test(NAME document-safety COMMAND turtle-code-window-tests --safety)
        set_tests_properties(document-safety PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 45)
        add_test(NAME file-preview COMMAND turtle-code-window-tests --file-preview)
        set_tests_properties(file-preview PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
        add_test(NAME startup-projects COMMAND turtle-code-window-tests --startup-projects)
        set_tests_properties(startup-projects PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
        add_test(NAME recovery COMMAND turtle-code-recovery-tests)
        set_tests_properties(recovery PROPERTIES TIMEOUT 45)
        add_test(NAME window-shutdown COMMAND turtle-code-window-tests)
        set_tests_properties(window-shutdown PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
        add_executable(turtle-code-editor-tests EXCLUDE_FROM_ALL
            tests/EditorWidgetTests.cpp
            src/document/Document.cpp
            src/editor/EditorWidget.cpp
            src/editor/UnicodeNavigation.cpp
            src/editor/SyntaxHighlighter.cpp
            src/language/LanguageRegistry.cpp
            src/language/GrammarManager.cpp)
        target_include_directories(turtle-code-editor-tests PRIVATE src)
        if(TURTLE_CODE_ENABLE_TREE_SITTER)
            target_sources(turtle-code-editor-tests PRIVATE src/language/TreeSitterCpp.cpp)
            target_include_directories(turtle-code-editor-tests PRIVATE "${TREE_SITTER_INCLUDE_DIR}")
            target_compile_definitions(turtle-code-editor-tests PRIVATE TURTLE_CODE_WITH_TREE_SITTER=1)
            target_link_libraries(turtle-code-editor-tests PRIVATE "${TREE_SITTER_LIBRARY}")
        endif()
        target_link_libraries(turtle-code-editor-tests PRIVATE
            turtle-code-icons
            turtle-code-themes
            turtle-code-text-core Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Test)
        add_test(NAME vim-editor COMMAND turtle-code-editor-tests --vim)
        add_test(NAME vim-session COMMAND turtle-code-editor-tests --vim-session)
        add_test(NAME vim-ex COMMAND turtle-code-editor-tests --vim-ex)
        add_test(NAME vim-viewport COMMAND turtle-code-editor-tests --vim-viewport)
        set_tests_properties(vim-viewport PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
        add_test(NAME vim-insert COMMAND turtle-code-editor-tests --vim-insert)
        set_tests_properties(vim-insert PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 180)
        add_test(NAME vim-normal COMMAND turtle-code-editor-tests --vim-normal)
        set_tests_properties(vim-normal PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 180)
        add_test(NAME vim-ex-extended COMMAND turtle-code-editor-tests --vim-ex-extended)
        set_tests_properties(vim-ex-extended PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
        add_test(NAME vim-search COMMAND turtle-code-editor-tests --vim-search)
        set_tests_properties(vim-search PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
        set_tests_properties(vim-ex PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 90)
        set_tests_properties(vim-session PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 90)
        set_tests_properties(vim-editor PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 45)
        add_test(NAME smart-editing COMMAND turtle-code-editor-tests --smart-editing)
        set_tests_properties(smart-editing PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 45)
        add_test(NAME editor-widget COMMAND turtle-code-editor-tests)
        set_tests_properties(editor-widget PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()

    if(BUILD_TESTING)
        add_executable(turtle-code-unicode-tests EXCLUDE_FROM_ALL
            tests/UnicodeNavigationTests.cpp
            src/editor/UnicodeNavigation.cpp
        )
        target_link_libraries(turtle-code-unicode-tests PRIVATE Qt6::Core)
        target_include_directories(turtle-code-unicode-tests PRIVATE src)
        target_compile_options(turtle-code-unicode-tests PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
        )
        add_test(NAME unicode-navigation COMMAND turtle-code-unicode-tests)

        if(TURTLE_CODE_ENABLE_TREE_SITTER)
            if(TURTLE_CODE_CPP_GRAMMAR_DIR AND
               EXISTS "${TURTLE_CODE_CPP_GRAMMAR_DIR}/src/parser.c")
                add_library(turtle-code-test-cpp-grammar SHARED EXCLUDE_FROM_ALL
                    "${TURTLE_CODE_CPP_GRAMMAR_DIR}/src/parser.c")
                if(EXISTS "${TURTLE_CODE_CPP_GRAMMAR_DIR}/src/scanner.c")
                    target_sources(turtle-code-test-cpp-grammar PRIVATE "${TURTLE_CODE_CPP_GRAMMAR_DIR}/src/scanner.c")
                endif()
                set_target_properties(turtle-code-test-cpp-grammar PROPERTIES
                    OUTPUT_NAME tree-sitter-cpp
                    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test-grammars")
            endif()
            add_executable(turtle-code-syntax-tests EXCLUDE_FROM_ALL
            tests/SyntaxHighlighterTests.cpp
                src/editor/SyntaxHighlighter.cpp
                src/language/TreeSitterCpp.cpp
                src/language/LanguageRegistry.cpp
                src/language/GrammarManager.cpp)
            target_compile_definitions(turtle-code-syntax-tests PRIVATE TURTLE_CODE_WITH_TREE_SITTER=1)
            target_include_directories(turtle-code-syntax-tests PRIVATE src "${TREE_SITTER_INCLUDE_DIR}")
            target_link_libraries(turtle-code-syntax-tests PRIVATE
                turtle-code-text-core "${TREE_SITTER_LIBRARY}" Qt6::Core Qt6::Network Qt6::Concurrent)
            target_compile_options(turtle-code-syntax-tests PRIVATE
                $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>)
            add_test(NAME syntax-highlighter COMMAND turtle-code-syntax-tests)
            if(TARGET turtle-code-test-cpp-grammar)
                add_dependencies(turtle-code-syntax-tests turtle-code-test-cpp-grammar)
                set_tests_properties(syntax-highlighter PROPERTIES ENVIRONMENT
                    "TURTLE_CODE_CPP_GRAMMAR_LIBRARY=${CMAKE_BINARY_DIR}/test-grammars/libtree-sitter-cpp.so")
            endif()
        endif()
    endif()

endif()

if(BUILD_TESTING)
    add_executable(turtle-code-text-tests EXCLUDE_FROM_ALL tests/PieceTreeBufferTests.cpp)
    target_link_libraries(turtle-code-text-tests PRIVATE turtle-code-text-core)
    target_compile_options(turtle-code-text-tests PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
    )
    add_test(NAME text-core COMMAND turtle-code-text-tests)

    add_executable(turtle-code-history-tests EXCLUDE_FROM_ALL tests/EditHistoryTests.cpp)
    target_link_libraries(turtle-code-history-tests PRIVATE turtle-code-text-core)
    target_compile_options(turtle-code-history-tests PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
    )
    add_test(NAME edit-history COMMAND turtle-code-history-tests)

    if(TURTLE_CODE_BUILD_APP)
        add_executable(turtle-code-project-search-tests EXCLUDE_FROM_ALL
            tests/ProjectSearchServiceTests.cpp
            src/search/ProjectSearchService.cpp
            src/search/ProjectSearchService.hpp)
        target_include_directories(turtle-code-project-search-tests PRIVATE src)
        target_link_libraries(turtle-code-project-search-tests PRIVATE turtle-code-workspace Qt6::Core Qt6::Concurrent Qt6::Test)
        add_test(NAME project-search COMMAND turtle-code-project-search-tests)

        add_executable(turtle-code-git-tests EXCLUDE_FROM_ALL tests/GitTests.cpp
            src/git/GitService.cpp src/git/GitPanel.cpp src/git/GitDiffView.cpp src/git/GitDiffSourcePane.cpp)
        target_include_directories(turtle-code-git-tests PRIVATE src)
        target_link_libraries(turtle-code-git-tests PRIVATE turtle-code-git-backend turtle-code-text-core turtle-code-icons turtle-code-i18n turtle-code-themes Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent Qt6::Test)
        target_compile_options(turtle-code-git-tests PRIVATE -UNDEBUG)
        add_test(NAME git COMMAND turtle-code-git-tests)
        set_tests_properties(git PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)

        add_executable(turtle-code-search-panel-tests EXCLUDE_FROM_ALL
            tests/SearchPanelTests.cpp
            src/search/SearchPanel.cpp
            src/search/ProjectSearchService.cpp)
        target_include_directories(turtle-code-search-panel-tests PRIVATE src)
        target_link_libraries(turtle-code-search-panel-tests PRIVATE turtle-code-workspace
            turtle-code-icons
            Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent Qt6::Test)
        add_test(NAME search-panel COMMAND turtle-code-search-panel-tests)
        set_tests_properties(search-panel PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

        add_executable(turtle-code-workspace-edit-tests EXCLUDE_FROM_ALL tests/WorkspaceEditTests.cpp)
        target_link_libraries(turtle-code-workspace-edit-tests PRIVATE turtle-code-workspace-edits)
        target_compile_options(turtle-code-workspace-edit-tests PRIVATE -UNDEBUG)
        add_test(NAME workspace-edits COMMAND turtle-code-workspace-edit-tests)
        add_test(NAME debug-window COMMAND turtle-code-window-tests --debug)
        set_tests_properties(debug-window PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 45)
        add_test(NAME lsp-window COMMAND turtle-code-window-tests --lsp)
        set_tests_properties(lsp-window PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 45)
        add_executable(turtle-code-lsp-manager-tests EXCLUDE_FROM_ALL tests/LspManagerTests.cpp
            src/language/LspManager.cpp src/language/LspManager.hpp
            src/language/LspClient.cpp src/language/LspClient.hpp
            src/language/LanguageRegistry.cpp src/language/LanguageRegistry.hpp)
        target_include_directories(turtle-code-lsp-manager-tests PRIVATE src)
        target_link_libraries(turtle-code-lsp-manager-tests PRIVATE turtle-code-i18n Qt6::Core Qt6::Test)
        target_compile_options(turtle-code-lsp-manager-tests PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
        target_compile_definitions(turtle-code-lsp-manager-tests PRIVATE LSP_FAKE_SERVER="${CMAKE_CURRENT_SOURCE_DIR}/tests/LspFakeServer.py")
        add_test(NAME lsp-manager COMMAND turtle-code-lsp-manager-tests)
        set_tests_properties(lsp-manager PROPERTIES TIMEOUT 45)
        add_executable(turtle-code-lsp-protocol-tests EXCLUDE_FROM_ALL tests/LspProtocolTests.cpp src/language/LspClient.cpp src/language/LspClient.hpp)
        target_include_directories(turtle-code-lsp-protocol-tests PRIVATE src)
        target_link_libraries(turtle-code-lsp-protocol-tests PRIVATE Qt6::Core Qt6::Test)
        target_compile_options(turtle-code-lsp-protocol-tests PRIVATE -UNDEBUG)
        target_compile_definitions(turtle-code-lsp-protocol-tests PRIVATE LSP_FAKE_SERVER="${CMAKE_CURRENT_SOURCE_DIR}/tests/LspFakeServer.py")
        add_test(NAME lsp-protocol COMMAND turtle-code-lsp-protocol-tests)
        add_executable(turtle-code-lsp-tests EXCLUDE_FROM_ALL
            tests/LspClientTests.cpp
            src/language/LspClient.cpp
            src/language/LspClient.hpp)
        target_include_directories(turtle-code-lsp-tests PRIVATE src)
        target_link_libraries(turtle-code-lsp-tests PRIVATE Qt6::Core Qt6::Test)
        add_test(NAME lsp-client COMMAND turtle-code-lsp-tests)
    endif()
endif()

if(TURTLE_CODE_BUILD_BENCHMARKS)
    add_executable(turtle-code-text-bench tests/PieceTreeBufferBench.cpp)
    target_link_libraries(turtle-code-text-bench PRIVATE turtle-code-text-core)
endif()

if(BUILD_TESTING)
    foreach(test_target turtle-code-editor-tests turtle-code-unicode-tests
                        turtle-code-syntax-tests turtle-code-project-search-tests
                        turtle-code-search-panel-tests turtle-code-lsp-tests)
        if(TARGET ${test_target})
            target_compile_definitions(${test_target} PRIVATE QT_FORCE_ASSERTS)
        endif()
    endforeach()
endif()

if(TURTLE_CODE_BUILD_APP)
    if(APPLE)
        set(TURTLE_CODE_MACOS_ICON packaging/macos/turtle-code.icns)
        set_source_files_properties(${TURTLE_CODE_MACOS_ICON} PROPERTIES
            MACOSX_PACKAGE_LOCATION Resources)
        target_sources(turtle-code PRIVATE ${TURTLE_CODE_MACOS_ICON})
        set_target_properties(turtle-code PROPERTIES
            MACOSX_BUNDLE_ICON_FILE "turtle-code.icns"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/Info.plist.in")
    endif()

    install(TARGETS turtle-code RUNTIME BUNDLE DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(FILES packaging/turtle-code.desktop
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications")
    install(DIRECTORY assets/icons/
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor"
        FILES_MATCHING PATTERN "*.svg" PATTERN "*.png")
    install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/licenses/turtle-code")
    install(FILES third_party/libgit2/COPYING DESTINATION share/licenses/turtle-code RENAME libgit2.COPYING)
    install(FILES third_party/catppuccin/LICENSE DESTINATION share/licenses/turtle-code RENAME catppuccin.LICENSE)
    install(FILES third_party/vim-digraphs/COPYING DESTINATION share/licenses/turtle-code RENAME vim-digraphs.COPYING)
    install(FILES third_party/libvterm/LICENSE DESTINATION share/licenses/turtle-code RENAME libvterm.LICENSE)
    install(FILES third_party/codicons/LICENSE DESTINATION share/licenses/turtle-code RENAME codicons.LICENSE)
    install(FILES third_party/codicons/LICENSE-CODE DESTINATION share/licenses/turtle-code RENAME codicons.LICENSE-CODE)
    install(FILES third_party/seti/LICENSE DESTINATION share/licenses/turtle-code RENAME seti.LICENSE)
    install(FILES third_party/seti/LICENSE-VSCODE DESTINATION share/licenses/turtle-code RENAME seti.LICENSE-VSCODE)
    install(FILES third_party/tokyo-night-vscode/LICENSE.txt DESTINATION share/licenses/turtle-code RENAME tokyo-night-vscode.LICENSE)

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        include(packaging/LinuxPackages.cmake)
    endif()
endif()

foreach(editor_target turtle-code turtle-code-editor-tests turtle-code-window-tests)
    if(TARGET ${editor_target})
        target_link_libraries(${editor_target} PRIVATE turtle-code-editorconfig turtle-code-vim)
        target_sources(${editor_target} PRIVATE src/editor/EditorVim.cpp src/editor/EditorVimInsert.cpp src/editor/EditorVimMappings.cpp src/editor/EditorVimOptions.cpp src/editor/EditorVimUserCommands.cpp src/editor/EditorVimCompletion.cpp src/editor/EditorVimViewport.cpp src/editor/EditorScreenLayout.cpp src/editor/EditorVimEdits.cpp src/editor/EditorVimSession.cpp src/editor/EditorVimBlock.cpp src/editor/EditorVimEx.cpp src/editor/EditorVimSearch.cpp src/editor/EditorVimSubstitute.cpp src/editor/EditorVimExRuntime.cpp src/editor/EditorVimExLines.cpp src/editor/EditorVimNormal.cpp)
    endif()
endforeach()
# Tests are opt-in build targets, never part of the default application build.
# Remove the obsolete cached switch so existing build directories also migrate.
unset(TURTLE_CODE_RUN_TESTS_AFTER_BUILD CACHE)
if(BUILD_TESTING)
    get_property(turtle_code_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    set(turtle_code_test_targets)
    foreach(test_target IN LISTS turtle_code_targets)
        if(test_target MATCHES "^turtle-code-.*-tests$")
            list(APPEND turtle_code_test_targets ${test_target})
        endif()
    endforeach()
    add_custom_target(turtle-code-tests DEPENDS ${turtle_code_test_targets})
    add_custom_target(turtle-code-check
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -LE memory
        DEPENDS turtle-code-tests
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running Turtle Code tests"
        USES_TERMINAL)
endif()

if(TURTLE_CODE_BUILD_APP)
    foreach(localized_target turtle-code turtle-code-themes turtle-code-workspace-edits
            turtle-code-editor-tests turtle-code-project-search-tests turtle-code-search-panel-tests
            turtle-code-syntax-tests turtle-code-lsp-tests turtle-code-lsp-protocol-tests turtle-code-window-tests)
        if(TARGET ${localized_target})
            target_link_libraries(${localized_target} PRIVATE turtle-code-i18n)
        endif()
    endforeach()
endif()

if(TURTLE_CODE_BUILD_APP AND TURTLE_CODE_ENABLE_TREE_SITTER)
    foreach(query_target turtle-code turtle-code-editor-tests turtle-code-syntax-tests turtle-code-window-tests)
        if(TARGET ${query_target})
            target_link_libraries(${query_target} PRIVATE turtle-code-query-data)
        endif()
    endforeach()
endif()

if(TARGET turtle-code-lsp-protocol-tests)
    target_link_libraries(turtle-code-lsp-protocol-tests PRIVATE turtle-code-themes)
endif()
if(TARGET turtle-code-lsp-tests)
    target_link_libraries(turtle-code-lsp-tests PRIVATE turtle-code-themes)
endif()
