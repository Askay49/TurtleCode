#pragma once
#include <QStatusBar>

static void formattingHighlightTests(QTemporaryDir& directory) {
    QSettings settings;
    settings.setValue("lsp/enabled", true);
    settings.setValue("editor/recoveryEnabled", false);
    settings.setValue("lsp/semanticHighlighting", true);
    settings.setValue("lsp/clangd/command", QStandardPaths::findExecutable("python3"));
    settings.setValue("lsp/clangd/arguments", QString("\"%1\" --format-many --semantic-range").arg(LSP_FAKE_SERVER));
    const auto path = directory.filePath("format-many.cpp");
    QByteArray original;
    for (int row = 0; row < 300; ++row) original += "int  value_" + QByteArray::number(row) + "=1;\n";
    {
        QFile file(path);
        assert(file.open(QIODevice::WriteOnly) && file.write(original) == original.size());
    }
    MainWindow window;
    window.show();
    window.openPaths({path});
    auto* editor = window.findChild<EditorWidget*>();
    auto* client = window.findChild<LspClient*>();
    auto* action = window.findChild<QAction*>("code.formatDocument");
    assert(editor && client && action);
    client->setLogLevel(2);
    auto tokenAt = [](const EditorWidget* view, int row, int column) {
        const auto tokens = view->semanticSnapshot().value(row);
        return std::any_of(tokens.cbegin(), tokens.cend(), [row, column](const auto& token) {
            return token.line == row && token.start == column && token.type == "parameter";
        });
    };
    assert(waitFor([&] { return !editor->isBusy() && tokenAt(editor, 12, 5); }));
    EditorWidget secondView(nullptr, editor->document());
    secondView.setSemanticHighlighting({editor->semanticSnapshot(), {}, true},
                                       secondView.lspRevision(), secondView.semanticGeneration());
    editor->setNavigationPosition(13, 6);
    editor->verticalScrollBar()->setValue(10);
    bool keptColors = false;
    const auto statusConnection = QObject::connect(window.statusBar(), &QStatusBar::messageChanged, &window, [&](const QString& message) {
        if (message.startsWith("Document formatted"))
            keptColors = tokenAt(editor, 13, 4) && tokenAt(&secondView, 13, 4) &&
                editor->verticalScrollBar()->value() == 11;
    });
    action->trigger();
    assert(waitFor([&] { return keptColors; }));
    QObject::disconnect(statusConnection);
    assert(editor->lspPosition() == QPoint(13, 4));
    assert(waitFor([&] {
        return client->log().contains("Response textDocument/semanticTokens/range")
            && editor->property("semanticAppliedRevision").toULongLong() == editor->lspRevision();
    }));
    assert(tokenAt(editor, 13, 4));

    // Replay the same batch through the streaming history path as well.
    auto limits = editor->document()->history().limits();
    limits.memoryBytes = 0;
    editor->document()->history().setLimits(limits);
    assert(editor->document()->history().memoryBytes() == 0 && editor->document()->history().diskBytes() > 0);
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    assert(waitFor([&] { return !editor->isBusy() && tokenAt(editor, 12, 5) && tokenAt(&secondView, 12, 5); }));
    assert(QByteArray::fromStdString(editor->document()->buffer().text()) == original);
    QTest::keyClick(editor, Qt::Key_Y, Qt::ControlModifier);
    assert(waitFor([&] { return !editor->isBusy() && tokenAt(editor, 13, 4) && tokenAt(&secondView, 13, 4); }));
    assert(QByteArray::fromStdString(editor->document()->buffer().text()) == "\n" + QByteArray(original).replace("int  ", "int "));
    closeDiscard(window);
}

static void formattingTests(QTemporaryDir& directory) {
    const auto read=[](const QString& path) {
        QFile file(path);assert(file.open(QIODevice::ReadOnly));return file.readAll();
    };
    for(const auto& mode:QStringList{"", "--format-tabs", "--format-invalid", "--format-error", "--no-format", "--format-null"}) {
        QSettings settings;settings.setValue("lsp/enabled",true);
        settings.setValue("editor/recoveryEnabled",false);
        settings.setValue("lsp/semanticHighlighting",false);
        settings.setValue("lsp/clangd/command",QStandardPaths::findExecutable("python3"));
        settings.setValue("lsp/clangd/arguments",QString("\"%1\" %2").arg(QString::fromUtf8(LSP_FAKE_SERVER),mode));
        const auto folder=mode=="--format-tabs"?directory.filePath("format-tabs"):directory.path();
        assert(QDir().mkpath(folder));
        if(mode=="--format-tabs") {
            QFile config(folder+"/.editorconfig");assert(config.open(QIODevice::WriteOnly));
            config.write("root=true\n[*]\nindent_style=tab\nindent_size=tab\ntab_width=8\n");
        }
        const auto path=folder+"/format.cpp";
        const QByteArray original=QByteArray("\xef\xbb\xbf")+QString::fromUtf8("// привет 😀\r\nint  value=1;\r\n").toUtf8();
        {QFile file(path);assert(file.open(QIODevice::WriteOnly));assert(file.write(original)==original.size());}
        MainWindow window;window.show();window.openPaths({path});
        auto* client=window.findChild<LspClient*>();assert(client);
        assert(waitFor([&]{auto* editor=window.findChild<EditorWidget*>();return editor && !editor->isBusy();}));
        auto* editor=window.findChild<EditorWidget*>();
        auto* action=window.findChild<QAction*>("code.formatDocument");assert(action);
        assert(action->shortcut()==QKeySequence("Ctrl+Shift+I"));
        // An unsaved edit must be synchronized before the request, without saving.
        editor->applyWorkspaceEdits({EditChange{3,"","// unsaved\r\n"}});
        const auto unformatted=QByteArray::fromStdString(editor->document()->buffer().text());
        auto formatted=unformatted;formatted.replace("int  value=1;","int value = 1;");
        editor->setFocus();window.activateWindow();QTest::qWait(30);
        QTest::keyClick(editor,Qt::Key_I,Qt::ControlModifier|Qt::ShiftModifier);
        assert(waitFor([&]{return client->ready();}));
        if(mode.isEmpty() || mode=="--format-tabs") {
            assert(waitFor([&]{return QByteArray::fromStdString(editor->document()->buffer().text())==formatted;}));
            assert(editor->isModified() && read(path)==original);
            QTest::keyClick(editor,Qt::Key_Z,Qt::ControlModifier);
            assert(QByteArray::fromStdString(editor->document()->buffer().text())==unformatted);
            QTest::keyClick(editor,Qt::Key_Z,Qt::ControlModifier);
            assert(QByteArray::fromStdString(editor->document()->buffer().text())==original && !editor->isModified());
            QTest::keyClick(editor,Qt::Key_Y,Qt::ControlModifier);
            QTest::keyClick(editor,Qt::Key_Y,Qt::ControlModifier);
            assert(QByteArray::fromStdString(editor->document()->buffer().text())==formatted);
            const auto revision=editor->lspRevision();
            action->trigger();
            assert(waitFor([&]{return window.statusBar()->currentMessage()=="No formatting changes needed.";}));
            assert(editor->lspRevision()==revision);
            QTest::keyClick(editor,Qt::Key_Z,Qt::ControlModifier);
            assert(QByteArray::fromStdString(editor->document()->buffer().text())==unformatted);
            // A delayed formatter response must not overwrite text typed meanwhile.
            action->trigger();
            const auto end=editor->byteSize();editor->applyWorkspaceEdits({EditChange{end,"","// late edit\r\n"}});
            QTest::qWait(350);
            assert(QByteArray::fromStdString(editor->document()->buffer().text())==unformatted+"// late edit\r\n");
            // Switching tabs must not apply a response to either document.
            QTest::keyClick(editor,Qt::Key_Z,Qt::ControlModifier);
            const auto other=directory.filePath("other.txt");
            {QFile file(other);assert(file.open(QIODevice::WriteOnly));file.write("other\n");}
            action->trigger();window.openPaths({other});QTest::qWait(350);
            assert(QByteArray::fromStdString(editor->document()->buffer().text())==unformatted);
            auto* tabs=window.findChild<QTabWidget*>();assert(tabs);tabs->setCurrentWidget(editor);
            action->trigger(); // Close while the delayed response is in flight.
        } else {
            const QString expected=mode=="--format-invalid" ? "Formatting not applied:" :
                mode=="--format-error" ? "Fake" : mode=="--no-format" ? "The language server does not support" : "No formatting changes needed.";
            if(mode=="--format-error") {
                assert(waitFor([&]{return window.statusBar()->currentMessage().contains("test formatting error");}));
            } else assert(waitFor([&]{return window.statusBar()->currentMessage().startsWith(expected);}));
            assert(QByteArray::fromStdString(editor->document()->buffer().text())==unformatted && read(path)==original);
        }
        closeDiscard(window);QTest::qWait(200);
    }
    formattingHighlightTests(directory);
}
