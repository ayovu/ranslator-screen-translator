#include "../native/ui.cpp"
#include <QAccessible>
#include <QTranslator>
#include <QtTest>

class ScopedUiTranslator final : public QTranslator {
  public:
    ScopedUiTranslator() { QCoreApplication::installTranslator(this); }
    ~ScopedUiTranslator() override { QCoreApplication::removeTranslator(this); }
    bool isEmpty() const override { return false; }
    QString translate(const char *context, const char *source, const char *, int) const override {
        if (qstrcmp(context, "Ranslator") != 0)
            return {};
        if (qstrcmp(source, "Open settings") == 0)
            return "TEST Open settings";
        if (qstrcmp(source, "Translation") == 0)
            return "TEST Translation";
        if (qstrcmp(source, "Save") == 0)
            return "TEST Save";
        return {};
    }
};

static QJsonObject sent;
static QList<QJsonObject> messages;
static void record(const char *text) {
    sent = QJsonDocument::fromJson(text).object();
    messages.append(sent);
}
static QPushButton *named(QWidget *root, const QString &name) {
    for (auto *button : root->findChildren<QPushButton *>())
        if (button->text() == name || button->accessibleName() == name)
            return button;
    return nullptr;
}
static QJsonObject defaults() {
    QFile file(QString(RANSLATOR_SOURCE_DIR) + "/config.example.json");
    file.open(QIODevice::ReadOnly);
    return QJsonDocument::fromJson(file.readAll()).object();
}
class UiTests : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        callback = record;
        Design::initialize();
        Design::configure(defaults());
    }
    void partial_ui_catalog_changes_labels_but_not_content_or_protocol() {
        {
            ScopedUiTranslator translator;
            const auto config = defaults();
            Window window(config, true);
            QCOMPARE(window.windowTitle(), "Ranslator");
            auto *open = named(&window, "TEST Open settings");
            QVERIFY(open);
            auto *source = window.findChild<QPlainTextEdit *>("sourceText");
            QVERIFY(source);
            source->setPlainText("Synthetic user text: Save");
            QCOMPARE(source->toPlainText(), "Synthetic user text: Save");
            open->click();
            auto *dialog = window.findChild<QDialog *>();
            QVERIFY(dialog);
            QVERIFY(named(dialog, "TEST Translation"));
            QVERIFY(named(dialog, "Languages")); // Missing catalog entry falls back to English.
            auto *provider = dialog->findChild<QComboBox *>("providerSelect");
            QVERIFY(provider);
            QCOMPARE(provider->currentData().toString(), config["provider"].toString());
            QVERIFY(provider->findData("compatible") >= 0);
            auto *presses = dialog->findChild<QSpinBox *>("shortcutTaps_once");
            QVERIFY(presses);
            presses->setValue(1);
            QCOMPARE(presses->suffix(), " press");
            presses->setValue(2);
            QCOMPARE(presses->suffix(), " presses");
            auto *save = named(dialog, "TEST Save");
            QVERIFY(save);
            save->click();
            QCOMPARE(sent["type"].toString(), "save_config");
            const auto saved = sent["config"].toObject();
            QCOMPARE(saved["provider"], config["provider"]);
            QCOMPARE(saved["preferred_language"], config["preferred_language"]);
            QCOMPARE(saved["once"].toObject()["sequence"], config["once"].toObject()["sequence"]);
        }
        QCOMPARE(UiText::tr("Save"), "Save"); // RAII also removes the translator after failures.
    }
    void settings_save_stays_open_and_fields_do_not_overlap() {
        Window window(defaults(), true);
        named(&window, "Open settings")->click();
        auto *dialog = window.findChild<QDialog *>();
        QVERIFY(dialog);
        named(dialog, "Translation")->click();
        auto *provider = dialog->findChild<QComboBox *>("providerSelect");
        provider->setCurrentIndex(provider->findData("ollama"));
        QTest::qWait(40);
        auto *timeout = dialog->findChild<QSpinBox *>("requestTimeout");
        QVERIFY(timeout->height() >= 36);
        QVERIFY(timeout->findChild<QLineEdit *>()->textMargins().left() >= 12);
        auto *caption = timeout->parentWidget()->findChild<QLabel *>();
        QVERIFY(caption);
        QVERIFY(caption->geometry().bottom() < timeout->geometry().top());
        named(dialog, "Save")->click();
        QCOMPARE(sent["type"].toString(), "save_config");
        window.receive({{"type", "config_saved"}, {"config", sent["config"]}});
        QVERIFY(dialog->isVisible());
        named(dialog, "Close")->click();
    }
    void shortcut_recorder_waits_for_release_and_keeps_modifier_only_gestures() {
        QList<bool> recording;
        ShortcutRecorder field("Ctrl+Alt+R", [&](bool active) { recording << active; });
        field.show();
        field.click();
        QVERIFY(field.isRecording());
        QVERIFY(field.text().contains("Listening"));
        QCOMPARE(field.sequence(), QString("Ctrl+Alt+R"));
        QTest::keyPress(&field, Qt::Key_Control);
        QTest::keyPress(&field, Qt::Key_Shift);
        // Modifiers are already held; QTest's modifier argument would synthesize
        // additional modifier releases together with the ordinary key release.
        QTest::keyPress(&field, Qt::Key_K);
        QTest::keyRelease(&field, Qt::Key_K);
        QVERIFY(field.isRecording());
        QTest::keyRelease(&field, Qt::Key_Shift);
        QVERIFY(field.isRecording());
        QTest::keyRelease(&field, Qt::Key_Control);
        QVERIFY(!field.isRecording());
        QCOMPARE(field.sequence(), QString("Ctrl+Shift+K"));
        QCOMPARE(recording, QList<bool>({true, false}));
        field.click();
        QTest::keyPress(&field, Qt::Key_Shift);
        QKeyEvent repeated(QEvent::KeyRelease, Qt::Key_Shift, Qt::ShiftModifier, {}, true);
        QCoreApplication::sendEvent(&field, &repeated);
        QVERIFY(field.isRecording());
        QTest::keyRelease(&field, Qt::Key_Shift);
        QCOMPARE(field.sequence(), QString("Shift"));
    }
    void shortcut_recorder_cancels_without_saving_and_intercepts_local_shortcuts() {
        QDialog dialog;
        auto *layout = new QVBoxLayout(&dialog);
        auto *field = new ShortcutRecorder("Ctrl+Alt+R", {});
        auto *other = new QLineEdit;
        layout->addWidget(field);
        layout->addWidget(other);
        auto *translate = new QShortcut(QKeySequence("Ctrl+Return"), &dialog);
        QSignalSpy triggered(translate, &QShortcut::activated);
        dialog.show();
        QApplication::setActiveWindow(&dialog);
        field->click();
        QTest::keyClick(field, Qt::Key_Escape);
        QVERIFY(dialog.isVisible());
        QVERIFY(!field->isRecording());
        QCOMPARE(field->sequence(), QString("Ctrl+Alt+R"));
        field->click();
        QTest::keyClick(field, Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(triggered.count(), 0);
        QCOMPARE(field->sequence(), QString("Ctrl+Return"));
        field->click();
        QTest::keyPress(field, Qt::Key_Shift);
        other->setFocus();
        QTRY_VERIFY(!field->isRecording());
        QCOMPARE(field->sequence(), QString("Ctrl+Return"));
        field->click();
        QTest::keyClick(field, Qt::Key_Tab, Qt::ControlModifier);
        QCOMPARE(field->sequence(), QString("Ctrl+Tab"));
        field->click();
        dialog.hide();
        QVERIFY(!field->isRecording());
        QVERIFY(!ShortcutRecorder::active());
    }
    void shortcut_recorder_normalizes_backtab_and_its_release() {
        ShortcutRecorder field("Ctrl+Alt+R", {});
        field.show();
        field.begin();
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Backtab, Qt::ShiftModifier);
        QCoreApplication::sendEvent(&field, &press);
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_Tab, Qt::NoModifier);
        QCoreApplication::sendEvent(&field, &release);
        QVERIFY(!field.isRecording());
        QCOMPARE(field.sequence(), QString("Shift+Tab"));
    }
    void shortcut_recorder_keeps_registered_chord_active_until_physical_modifiers_release() {
        if (QGuiApplication::platformName() != "xcb" ||
            qEnvironmentVariable("RANSLATOR_TEST_X11") != "1")
            QSKIP("Requires real key input on the isolated Xvfb regression display.");
        ShortcutRecorder field("Ctrl+Alt+R", {});
        field.show();
        field.activateWindow();
        field.setFocus();
        QVERIFY(QTest::qWaitForWindowExposed(&field));
        field.begin();
        QCOMPARE(QProcess::execute("xdotool", {"keydown", "ctrl+alt"}), 0);
        QTest::qWait(20);
        field.acceptRegisteredShortcut("Ctrl+Alt+O");
        QVERIFY(field.isRecording());
        QTest::qWait(310); // Exceeds the post-recording guard used by the controller.
        QVERIFY(field.isRecording());
        QCOMPARE(field.sequence(), QString("Ctrl+Alt+R"));
        QCOMPARE(QProcess::execute("xdotool", {"keyup", "alt+ctrl"}), 0);
        QTRY_VERIFY(!field.isRecording());
        QCOMPARE(field.sequence(), QString("Ctrl+Alt+O"));
        field.begin();
        QCOMPARE(QProcess::execute("xdotool", {"keydown", "ctrl"}), 0);
        field.acceptRegisteredShortcut("Ctrl+O");
        field.cancel();
        QCOMPARE(QProcess::execute("xdotool", {"keyup", "ctrl"}), 0);
        QTest::qWait(40);
        QCOMPARE(field.sequence(), QString("Ctrl+Alt+O"));
        QVERIFY(!field.isRecording());
    }
    void shortcut_recorder_rejects_multiple_keys_and_accepts_known_desktop_activation() {
        ShortcutRecorder field("Ctrl+Alt+R", {});
        field.show();
        field.click();
        QTest::keyPress(&field, Qt::Key_A);
        QTest::keyPress(&field, Qt::Key_B);
        QTest::keyRelease(&field, Qt::Key_A);
        QTest::keyRelease(&field, Qt::Key_B);
        QVERIFY(field.isRecording());
        QCOMPARE(field.sequence(), QString("Ctrl+Alt+R"));
        field.interceptedShortcut();
        QVERIFY(field.isRecording());
        QCOMPARE(field.sequence(), QString("Ctrl+Alt+R"));
        field.acceptRegisteredShortcut("Alt+F10");
        QTRY_COMPARE(field.sequence(), QString("Alt+F10"));
        QTRY_VERIFY(!field.isRecording());
    }
    void shortcut_recorder_captures_native_grab_and_cancels_on_real_focus_loss() {
        if (QGuiApplication::platformName() != "xcb" ||
            qEnvironmentVariable("RANSLATOR_TEST_X11") != "1")
            QSKIP("Requires real key input on the isolated Xvfb regression display.");
        auto click = [](QWidget *widget) {
            QVERIFY(widget);
            const auto point = widget->mapToGlobal(widget->rect().center());
            QCOMPARE(QProcess::execute("xdotool", {"mousemove", QString::number(point.x()),
                                                   QString::number(point.y()), "click", "1"}),
                     0);
            QTest::qWait(60);
        };
        Window window(defaults(), false); // Real native grabs on the private display/bus.
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        click(named(&window, "Open settings"));
        auto *dialog = window.findChild<QDialog *>();
        QVERIFY(dialog);
        click(named(dialog, "Shortcuts"));
        auto *field =
            dynamic_cast<ShortcutRecorder *>(dialog->findChild<QPushButton *>("shortcut_once"));
        QVERIFY(field);
        click(field);
        QVERIFY(field->isRecording());
        QCOMPARE(QProcess::execute("xdotool", {"key", "ctrl+alt+k"}), 0);
        QTRY_VERIFY(!field->isRecording());
        QCOMPARE(field->sequence(), QString("Ctrl+Alt+K"));
        QTest::qWait(270);
        QCOMPARE(QProcess::execute("xdotool", {"key", "ctrl+alt+o"}), 0);
        QTRY_COMPARE(sent["type"].toString(), QString("shortcut"));
        QCOMPARE(sent["action"].toString(), QString("snapshot"));

        click(field);
        QVERIFY(field->isRecording());
        const auto start = messages.size();
        QCOMPARE(QProcess::execute("xdotool", {"keydown", "ctrl", "alt", "o"}), 0);
        QTest::qWait(310);
        const bool retained = field->isRecording();
        // Always release the synthetic keys before any assertion can return early.
        QCOMPARE(QProcess::execute("xdotool", {"keyup", "o", "alt", "ctrl"}), 0);
        QVERIFY(retained);
        QTRY_VERIFY(!field->isRecording());
        QCOMPARE(field->sequence(), QString("Ctrl+Alt+O"));
        for (const auto &message : messages.mid(start)) {
            QVERIFY(message["type"] != "shortcut");
            QVERIFY(message["type"] != "action");
            QVERIFY(message["type"] != "snapshot_start");
        }

        click(field);
        QVERIFY(field->isRecording());
        click(dialog->findChild<QSpinBox *>("shortcutTaps_once"));
        QVERIFY(!field->isRecording());
        QCOMPARE(field->sequence(), QString("Ctrl+Alt+O"));
        click(field);
        QVERIFY(field->isRecording());
        QWidget otherWindow;
        otherWindow.show();
        QVERIFY(QTest::qWaitForWindowExposed(&otherWindow));
        QCOMPARE(QProcess::execute("xdotool",
                                   {"windowfocus", QString::number(otherWindow.winId())}),
                 0);
        QTRY_VERIFY(!field->isRecording());
        QCOMPARE(field->sequence(), QString("Ctrl+Alt+O"));
    }
    void settings_save_recorded_binding_and_press_count_without_triggering_capture() {
        Window window(defaults(), true);
        named(&window, "Open settings")->click();
        auto *dialog = window.findChild<QDialog *>();
        named(dialog, "Shortcuts")->click();
        auto *field =
            dynamic_cast<ShortcutRecorder *>(dialog->findChild<QPushButton *>("shortcut_once"));
        QVERIFY(field);
        field->click();
        QCOMPARE(sent["type"].toString(), QString("shortcut_recording"));
        QVERIFY(sent["active"].toBool());
        window.launch({{"_action", "snapshot"}});
        QCOMPARE(sent["type"].toString(), QString("shortcut_recording"));
        QTest::keyClick(field, Qt::Key_Shift);
        QVERIFY(!sent["active"].toBool());
        dialog->findChild<QSpinBox *>("shortcutTaps_once")->setValue(3);
        named(dialog, "Save")->click();
        QCOMPARE(sent["type"].toString(), QString("save_config"));
        const auto binding = sent["config"].toObject()["once"].toObject();
        QCOMPARE(binding["sequence"].toString(), QString("Shift"));
        QCOMPARE(binding["taps"].toInt(), 3);
    }
    void desktop_command_actions_leave_the_full_translator_hidden() {
        Window window(defaults(), true);
        for (const auto &id : {"once", "continuous", "snapshot"}) {
            window.launch({{"_action", id}});
            QCOMPARE(sent["type"].toString(), "action");
            QCOMPARE(sent["action"].toString(), QString(id));
            QVERIFY(!window.isVisible());
        }
    }
    void quick_result_survives_delayed_monitor_placement() {
        QuickPopup popup(nullptr, [] {}, [](QString, QString) {}, [] {});
        popup.begin(defaults(), nullptr, false);
        QVERIFY(!popup.isVisible());
        popup.display({{"text", "翻訳"}, {"source", "en"}, {"target", "ja"}});
        popup.showPrepared(QGuiApplication::primaryScreen(), QPoint(80, 80));
        QVERIFY(popup.isVisible());
        auto *text = popup.findChild<QPlainTextEdit *>();
        QCOMPARE(text->toPlainText(), QString("翻訳"));
        QVERIFY(text->font().families().contains("Noto Sans CJK JP"));
    }
    void theme_changes_keep_child_backgrounds_transparent() {
        GlassWindow window;
        window.resize(360, 260);
        auto *container = new QWidget(&window);
        container->setGeometry(60, 90, 160, 70);
        // Native styles can request a palette background. Our material must override it.
        container->setAutoFillBackground(true);
        auto *caption = new QLabel("Transparent caption", container);
        caption->setGeometry(8, 8, 145, 28);
        window.show();
        for (const auto &appearance : {"light", "dark", "light", "dark"}) {
            auto config = defaults();
            config["appearance"] = appearance;
            Design::configure(config);
            Design::configure(config); // Save after live appearance preview.
            window.setProperty("desktopBlur", true);
            QCoreApplication::processEvents();
            container->hide();
            const auto background = window.grab().toImage();
            container->show();
            const auto foreground = window.grab().toImage();
            const QPoint sample = container->geometry().bottomRight() - QPoint(8, 8);
            QCOMPARE(foreground.pixelColor(sample), background.pixelColor(sample));
            QVERIFY(foreground.pixelColor(sample).alpha() < 255);
        }
        Design::configure(defaults());
    }
    void desktop_palette_updates_keep_reopened_translator_transparent() {
        auto config = defaults();
        config["appearance"] = "dark";
        Design::configure(config);
        Window window(config, true);
        window.show();
        QTest::qWait(180);
        const auto originalPalette = qApp->palette();
        auto changedPalette = originalPalette;
        for (const auto role : {QPalette::Window, QPalette::Base, QPalette::Button})
            changedPalette.setColor(role, QColor("#181d25"));
        qApp->setPalette(changedPalette); // A desktop theme can update this after startup.
        for (int reopen = 0; reopen < 3; ++reopen) {
            window.hide();
            window.show();
            QCoreApplication::processEvents();
            window.setProperty("desktopBlur", true);
            QList<QWidget *> children;
            children.append(window.findChild<QLabel *>("brand"));
            children.append(window.findChild<QLabel *>("status"));
            for (auto *widget : window.findChildren<QWidget *>())
                if (dynamic_cast<GlassSurface *>(widget))
                    children.append(widget);
            for (auto *child : children) {
                QVERIFY(child);
                if (!child->isVisible())
                    continue;
                const QPoint sample = child->mapTo(&window, QPoint(0, child->height() - 1));
                const auto visible = window.grab().toImage();
                child->hide();
                const auto hidden = window.grab().toImage();
                child->show();
                QCOMPARE(visible.pixelColor(sample), hidden.pixelColor(sample));
            }
        }
        qApp->setPalette(originalPalette);
        Design::configure(defaults());
    }
    void glass_partial_repaint_does_not_accumulate_opacity() {
        auto config = defaults();
        config["appearance"] = "dark";
        Design::configure(config);
        GlassWindow window;
        window.resize(360, 260);
        window.ensurePolished();
        window.setProperty("desktopBlur", true);
        QImage retained(window.size(), QImage::Format_ARGB32_Premultiplied);
        retained.fill(Qt::transparent);
        auto paint = [&](const QRegion &damage) {
            QPainter painter(&retained);
            painter.setClipRegion(damage);
            window.render(&painter, damage.boundingRect().topLeft(), damage, QWidget::DrawChildren);
        };
        paint(window.rect());
        const auto firstFrame = retained.copy();
        const QRect damage(70, 80, 160, 90);
        paint(damage);
        const auto firstPartialFrame = retained.copy();
        for (int update = 0; update < 5; ++update)
            paint(damage);
        firstFrame.save(QString(RANSLATOR_SOURCE_DIR) + "/build/retained-theme-first.png");
        retained.save(QString(RANSLATOR_SOURCE_DIR) + "/build/retained-theme-repaint.png");
        QCOMPARE(retained.pixelColor(damage.center()), firstFrame.pixelColor(damage.center()));
        QCOMPARE(retained, firstPartialFrame);
        for (int y = 0; y < retained.height(); ++y) {
            for (int x = 0; x < retained.width(); ++x) {
                const auto actual = retained.pixelColor(x, y),
                           expected = firstFrame.pixelColor(x, y);
                if (!damage.contains(x, y)) {
                    QCOMPARE(actual, expected);
                } else {
                    // A clipped gradient can round its color channels slightly differently.
                    QVERIFY(qAbs(actual.red() - expected.red()) <= 2);
                    QVERIFY(qAbs(actual.green() - expected.green()) <= 2);
                    QVERIFY(qAbs(actual.blue() - expected.blue()) <= 2);
                    QVERIFY(qAbs(actual.alpha() - expected.alpha()) <= 1);
                }
            }
        }
        Design::configure(defaults());
    }
    void dropdown_container_has_no_opaque_rectangular_corners() {
        Window window(defaults(), true);
        named(&window, "Open settings")->click();
        auto *dialog = window.findChild<QDialog *>();
        named(dialog, "Translation")->click();
        auto *provider = dialog->findChild<QComboBox *>("providerSelect");
        for (const auto &appearance : {"dark", "light", "dark"}) {
            auto config = defaults();
            config["appearance"] = appearance;
            Design::configure(config);
            provider->setCurrentIndex(provider->findData("auto"));
            provider->showPopup();
            QCoreApplication::processEvents();
            auto *popup = provider->view()->window();
            const auto rendered = popup->grab().toImage();
            rendered.save(QString(RANSLATOR_SOURCE_DIR) + "/build/dropdown-" + appearance + ".png");
            QVERIFY(popup->testAttribute(Qt::WA_TranslucentBackground));
            QCOMPARE(rendered.pixelColor(0, 0).alpha(), 0);
            QCOMPARE(rendered.pixelColor(rendered.width() - 1, rendered.height() - 1).alpha(), 0);
            QTest::keyClick(provider->view(), Qt::Key_Down);
            QTest::keyClick(provider->view(), Qt::Key_Return);
            QCOMPARE(provider->currentData().toString(), QString("ollama"));
        }
        dialog->close();
        Design::configure(defaults());
    }
    void dark_material_stays_dark_over_a_white_background() {
        GlassWindow window;
        window.resize(360, 260);
        window.show();
        auto config = defaults();
        config["appearance"] = "dark";
        Design::configure(config);
        window.setProperty("desktopBlur", true);
        const auto material = window.grab().toImage();
        const QPoint center(material.width() / 2, material.height() / 2);
        QVERIFY(material.pixelColor(center).alpha() < 255);
        QImage composite(material.size(), QImage::Format_ARGB32_Premultiplied);
        composite.fill(Qt::white);
        QPainter painter(&composite);
        painter.drawImage(0, 0, material);
        painter.end();
        QVERIFY(composite.pixelColor(center).lightness() < 100);
        Design::configure(defaults());
    }
    void discovered_runtime_populates_editable_command_and_requests_models() {
        Window window(defaults(), true);
        named(&window, "Open settings")->click();
        auto *dialog = window.findChild<QDialog *>();
        named(dialog, "Translation")->click();
        auto *provider = dialog->findChild<QComboBox *>("providerSelect");
        provider->setCurrentIndex(provider->findData("cli"));
        window.receive({{"type", "runtimes"},
                        {"runtimes", QJsonArray{QJsonObject{
                                         {"id", "opencode"},
                                         {"name", "OpenCode"},
                                         {"installed", true},
                                         {"path", "/usr/bin/opencode"},
                                         {"args", QJsonArray{"run", "--format", "json"}}}}}});
        auto *runtime = dialog->findChild<QComboBox *>("runtimeSelect");
        runtime->setCurrentIndex(1);
        QMetaObject::invokeMethod(runtime, "activated", Q_ARG(int, 1));
        QCOMPARE(sent["type"].toString(), "list_models");
        QCOMPARE(sent["config"].toObject()["cli_program"].toString(), "/usr/bin/opencode");
        named(dialog, "Save")->click();
        const auto config = sent["config"].toObject();
        QCOMPARE(config["cli_runtime"].toString(), "opencode");
        QCOMPARE(config["cli_args"].toArray(), QJsonArray({"run", "--format", "json"}));
        QVERIFY(config["cli_free_only"].toBool());
        dialog->close();
    }
    void switching_opencode_to_ollama_ignores_stale_results_and_saves_the_selected_model() {
        auto config = defaults();
        config["provider"] = "cli";
        config["cli_runtime"] = "opencode";
        config["cli_program"] = "/custom/opencode";
        config["cli_args"] = QJsonArray{"run", "--custom"};
        config["model"] = "opencode/demo-free";
        config["endpoint"] = "https://previous.example/v1";
        config["timeout_seconds"] = 37;
        Window window(config, true);
        named(&window, "Open settings")->click();
        auto *dialog = window.findChild<QDialog *>();
        named(dialog, "Translation")->click();
        const QJsonValue oldId = sent["id"];
        auto *provider = dialog->findChild<QComboBox *>("providerSelect");
        auto *model = dialog->findChild<QComboBox *>("providerModel");
        auto *endpoint = dialog->findChild<QLineEdit *>("providerEndpoint");
        provider->setCurrentIndex(provider->findData("ollama"));
        QCOMPARE(sent["type"].toString(), "list_models");
        const QJsonValue ollamaId = sent["id"];
        QVERIFY(ollamaId != oldId);
        QVERIFY(endpoint->text().isEmpty());
        QVERIFY(model->currentText().isEmpty());
        QCOMPARE(sent["config"].toObject()["provider"].toString(), "ollama");
        // CLI discovery must not invalidate the current Ollama lookup.
        window.receive({{"type", "runtimes"}, {"runtimes", QJsonArray{}}});
        window.receive(
            {{"type", "models"}, {"id", oldId}, {"models", QJsonArray{"opencode/wrong-free"}}});
        QCOMPARE(model->count(), 0);
        window.receive({{"type", "models"},
                        {"id", ollamaId},
                        {"models", QJsonArray{"qwen2.5:3b", "llama3.2:3b"}}});
        QCOMPARE(model->currentText(), "qwen2.5:3b");
        named(dialog, "Save")->click();
        const auto saved = sent["config"].toObject();
        QCOMPARE(saved["provider"].toString(), "ollama");
        QCOMPARE(saved["model"].toString(), "qwen2.5:3b");
        QVERIFY(saved["endpoint"].toString().isEmpty());
        window.receive({{"type", "config_saved"}, {"config", saved}});
        dialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        // Restore even after saving and reopening, including manually customized command args.
        named(&window, "Open settings")->click();
        dialog = window.findChild<QDialog *>();
        provider = dialog->findChild<QComboBox *>("providerSelect");
        provider->setCurrentIndex(provider->findData("cli"));
        QCOMPARE(dialog->findChild<QComboBox *>("providerModel")->currentText(),
                 "opencode/demo-free");
        QCOMPARE(dialog->findChild<QLineEdit *>("runtimeExecutable")->text(), "/custom/opencode");
        QCOMPARE(dialog->findChild<QSpinBox *>("requestTimeout")->value(), 37);
        named(dialog, "Save")->click();
        QCOMPARE(sent["config"].toObject()["cli_args"].toArray(), QJsonArray({"run", "--custom"}));
        dialog->close();
    }
    void provider_profiles_keep_endpoints_credentials_models_and_timeouts_separate() {
        auto config = defaults();
        config["provider"] = "compatible";
        config["endpoint"] = "https://api.example/v1";
        config["api_key"] = "test-only-token";
        config["api_key_env"] = "EXAMPLE_TOKEN";
        config["model"] = "private-model";
        config["timeout_seconds"] = 42;
        Window window(config, true);
        named(&window, "Open settings")->click();
        auto *dialog = window.findChild<QDialog *>();
        auto *provider = dialog->findChild<QComboBox *>("providerSelect");
        auto *endpoint = dialog->findChild<QLineEdit *>("providerEndpoint");
        auto *model = dialog->findChild<QComboBox *>("providerModel");
        auto *key = dialog->findChild<QLineEdit *>("providerKey");
        provider->setCurrentIndex(provider->findData("ollama"));
        QVERIFY(key->text().isEmpty());
        endpoint->setText("http://127.0.0.1:11435");
        model->setEditText("qwen3:4b");
        provider->setCurrentIndex(provider->findData("compatible"));
        QCOMPARE(endpoint->text(), "https://api.example/v1");
        QCOMPARE(key->text(), "test-only-token");
        QCOMPARE(dialog->findChild<QSpinBox *>("requestTimeout")->value(), 42);
        window.receive(
            {{"type", "models"}, {"id", sent["id"]}, {"models", QJsonArray{"public-model"}}});
        QCOMPARE(model->currentText(), "private-model");
        provider->setCurrentIndex(provider->findData("ollama"));
        QCOMPARE(endpoint->text(), "http://127.0.0.1:11435");
        QCOMPARE(model->currentText(), "qwen3:4b");
        QVERIFY(key->text().isEmpty());
        dialog->close();
    }
    void editing_endpoint_or_reopening_settings_invalidates_inflight_model_lists() {
        auto config = defaults();
        config["provider"] = "ollama";
        config["model"] = "removed-model";
        Window window(config, true);
        named(&window, "Open settings")->click();
        auto *dialog = window.findChild<QDialog *>();
        named(dialog, "Translation")->click();
        const QJsonValue oldId = sent["id"];
        auto *model = dialog->findChild<QComboBox *>("providerModel");
        dialog->findChild<QLineEdit *>("providerEndpoint")->setText("http://127.0.0.1:11435");
        window.receive({{"type", "models"}, {"id", oldId}, {"models", QJsonArray{"wrong-model"}}});
        QCOMPARE(model->count(), 0);
        named(dialog, "Refresh available models")->click();
        const QJsonValue currentId = sent["id"];
        window.receive({{"type", "models"}, {"id", currentId}, {"models", QJsonArray{"qwen3:4b"}}});
        QCOMPARE(model->currentText(), "qwen3:4b");
        named(dialog, "Refresh available models")->click();
        const QJsonValue closingId = sent["id"];
        dialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        named(&window, "Open settings")->click();
        dialog = window.findChild<QDialog *>();
        named(dialog, "Translation")->click();
        model = dialog->findChild<QComboBox *>("providerModel");
        window.receive(
            {{"type", "models"}, {"id", closingId}, {"models", QJsonArray{"wrong-model"}}});
        QCOMPARE(model->count(), 0);
        window.receive(
            {{"type", "models"}, {"id", sent["id"]}, {"models", QJsonArray{"qwen2.5:3b"}}});
        QCOMPARE(model->currentText(), "qwen2.5:3b");
        dialog->close();
    }
    void packs_can_download_the_entire_catalog() {
        // Language packs belong to Tesseract; macOS defaults to Apple Vision, which needs none.
        auto config = defaults();
        config["ocr_engine"] = "tesseract";
        Window window(config, true);
        named(&window, "Open settings")->click();
        auto *settings = window.findChild<QDialog *>();
        named(settings, "Screen text")->click();
        named(settings, "Language packs…")->click();
        auto *packs = settings->findChild<QDialog *>();
        QVERIFY(packs);
        const auto id = sent["id"];
        QJsonArray catalog;
        for (int i = 0; i < 24; ++i)
            catalog.append(QJsonObject{{"code", QString("lang%1").arg(i)},
                                       {"name", QString("Language %1").arg(i)},
                                       {"bytes", 1000},
                                       {"installed", false}});
        window.receive({{"type", "packs_list"}, {"id", id}, {"packs", catalog}});
        named(packs, "Select all")->click();
        for (auto *check : packs->findChildren<QCheckBox *>())
            QVERIFY(check->isChecked());
        named(packs, "Download all packs")->click();
        QCOMPARE(sent["type"].toString(), "install_packs");
        QCOMPARE(sent["codes"].toArray().size(), 24);
        packs->close();
        settings->close();
    }
    void capture_modes_are_saved_and_select_distinct_capture_commands() {
        Window window(defaults(), true);
        named(&window, "Open settings")->click();
        auto *dialog = window.findChild<QDialog *>();
        named(dialog, "Screen text")->click();
        auto *mode = dialog->findChild<QComboBox *>("captureMode");
        QVERIFY(mode);
#ifdef Q_OS_MACOS
        // macOS has no desktop screenshot service to delegate the picker mode to.
        QCOMPARE(mode->findData("picker"), -1);
        for (const auto &value : {"auto", "desktop", "region"}) {
#else
        for (const auto &value : {"auto", "desktop", "region", "picker"}) {
#endif
            mode->setCurrentIndex(mode->findData(value));
            named(dialog, "Save")->click();
            QCOMPARE(sent["config"].toObject()["capture_mode"].toString(), value);
            QVERIFY(dialog->isVisible());
        }
        auto args = Capture::spectacleArguments("auto", "/tmp/snapshot.png");
        QVERIFY(args.contains("--current"));
        QVERIFY(!args.contains("--fullscreen"));
        QVERIFY(Capture::spectacleArguments("desktop", "a").contains("--fullscreen"));
        QVERIFY(Capture::spectacleArguments("region", "a").contains("--region"));
        QCOMPARE(args.last(), "/tmp/snapshot.png");
        QCOMPARE(args[args.indexOf("--delay") + 1], "0");
        dialog->close();
    }
    void ocr_engine_setting_saves_and_disables_model_controls_vision_does_not_use() {
        auto config = defaults();
        config["ocr_engine"] = "auto";
        Window window(config, true);
        named(&window, "Open settings")->click();
        auto *dialog = window.findChild<QDialog *>();
        named(dialog, "Screen text")->click();
        auto *engine = dialog->findChild<QComboBox *>("ocrEngine");
        auto *packs = dialog->findChild<QPushButton *>("ocrLanguagePacks");
        auto *models = dialog->findChild<QLineEdit *>("ocrModelDirectory");
        QVERIFY(engine && packs && models);
#ifdef Q_OS_MACOS
        QCOMPARE(engine->currentData().toString(), "vision");
        QVERIFY(engine->isVisible());
        QVERIFY(!packs->isEnabled() && !models->isEnabled());
        engine->setCurrentIndex(engine->findData("tesseract"));
        QVERIFY(packs->isEnabled() && models->isEnabled());
        named(dialog, "Save")->click();
        QCOMPARE(sent["config"].toObject()["ocr_engine"].toString(), "tesseract");
        engine->setCurrentIndex(engine->findData("vision"));
        named(dialog, "Save")->click();
        QCOMPARE(sent["config"].toObject()["ocr_engine"].toString(), "vision");
#else
        // Tesseract is the only engine here; a value copied from a Mac is preserved, not shown.
        QVERIFY(!engine->isVisible());
        QVERIFY(packs->isEnabled() && models->isEnabled());
        named(dialog, "Save")->click();
        QCOMPARE(sent["config"].toObject()["ocr_engine"].toString(), "auto");
#endif
        dialog->close();
    }
#ifdef Q_OS_MACOS
    void vision_reads_small_mixed_script_screen_text_in_reading_order() {
        // A synthetic Retina screen: light and dark rows, Cyrillic and Latin, 9–14 pt at 2x.
        struct Row {
            QString text;
            int points;
            QColor color, background;
        };
        const QList<Row> rows{
            {"File Edit View History Bookmarks Window Help", 13, Qt::black, Qt::white},
            {"Settings and privacy controls for your account", 11, QColor(70, 70, 70), Qt::white},
            {"Настройки конфиденциальности и безопасности", 12, Qt::black, Qt::white},
            {"Последнее обновление было вчера в 18:42", 10, QColor(120, 120, 120), Qt::white},
            {"Нажмите кнопку, чтобы продолжить установку", 13, Qt::white, QColor(40, 40, 46)},
            {"Small caption text under a thumbnail image", 9, QColor(120, 120, 120), Qt::white},
        };
        QImage image(1800, 760, QImage::Format_RGB32);
        image.fill(Qt::white);
        QList<QRect> expected;
        {
            QPainter painter(&image);
            int top = 40;
            for (const auto &row : rows) {
                QFont font;
                font.setPixelSize(row.points * 2);
                painter.setFont(font);
                const QFontMetrics metrics(font);
                const QRect box(60, top, metrics.horizontalAdvance(row.text), metrics.height());
                painter.fillRect(box.adjusted(-20, -12, 20, 12), row.background);
                painter.setPen(row.color);
                painter.drawText(box, Qt::AlignLeft | Qt::AlignVCenter, row.text);
                expected << box;
                top += metrics.height() + 72;
            }
        }
        QTemporaryDir directory;
        const auto path = directory.filePath("screen.png");
        QVERIFY(image.save(path));
        QString error;
        const auto tsv = NativeMac::recognizeText(path, "eng+rus", &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QStringList lines;
        QString current;
        int row = -1;
        for (const auto &record : tsv.split('\n', Qt::SkipEmptyParts).mid(1)) {
            const auto fields = record.split('\t');
            QCOMPARE(fields.size(), 12);
            const QRect box(fields[6].toInt(), fields[7].toInt(), fields[8].toInt(),
                            fields[9].toInt());
            // Each word lies on the row it was drawn on, and rows are read top to bottom.
            int match = -1;
            for (int i = 0; i < expected.size(); ++i)
                if (expected[i].adjusted(-8, -8, 8, 8).intersects(box))
                    match = i;
            QVERIFY2(match >= row, qPrintable(record));
            if (match != row) {
                if (row >= 0)
                    lines << current;
                current.clear();
                row = match;
            }
            current += (current.isEmpty() ? "" : " ") + fields[11];
        }
        lines << current;
        QStringList drawn;
        for (const auto &item : rows)
            drawn << item.text;
        QCOMPARE(lines, drawn);
    }
#endif
    void image_output_metadata_is_used_only_for_single_monitor_capture() {
        auto *first = QGuiApplication::screens().first();
        auto *last = QGuiApplication::screens().last();
        QImage image(100, 100, QImage::Format_RGB32);
        image.setText("screen", last->name());
        QCOMPARE(Capture::imageScreen(image, first, true), last);
        QCOMPARE(Capture::imageScreen(image, first, false), first);
        image.setText("screen", "disconnected-output");
        QCOMPARE(Capture::imageScreen(image, first, true), first);
        QCOMPARE(Capture::imageScreen(image, nullptr, true), QGuiApplication::primaryScreen());
    }
    void snapshot_binds_the_native_surface_to_each_monitor() {
        if (qEnvironmentVariable("RANSLATOR_LIVE_MONITOR_TEST") != "1")
            QSKIP("Opt-in test: briefly displays synthetic snapshots on each physical monitor");
        QVERIFY(QGuiApplication::screens().size() >= 2);
        for (auto *target : QGuiApplication::screens()) {
            QImage image(800, 600, QImage::Format_RGB32);
            image.fill(QColor("#e5efff"));
            auto *view = new SnapshotView(image, false, {}, target);
            QVERIFY(QTest::qWaitForWindowExposed(view));
            QTest::qWait(200);
            QCOMPARE(view->windowHandle()->screen(), target);
            QCOMPARE(view->size(), target->geometry().size());
            QCOMPARE(view->windowHandle()->visibility(), QWindow::FullScreen);
            qInfo() << "PASS fullscreen output" << target->name() << view->size();
            view->close();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
    }
    void snapshot_accepts_selection_before_ocr_and_copies_source() {
        QImage image(800, 600, QImage::Format_RGB32);
        image.fill(Qt::white);
        QPoint offset;
        bool requested = false;
        auto *view = new SnapshotView(image, true, [&](QImage crop, QPoint origin) {
            requested = !crop.isNull();
            offset = origin;
        });
        QTest::qWait(30);
        QCOMPARE(view->windowType(), Qt::Tool);
        const double scale = qMin(view->width() / 800.0, view->height() / 600.0);
        const QPoint origin((view->width() - 800 * scale) / 2, (view->height() - 600 * scale) / 2);
        QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier,
                          origin + QPoint(75 * scale, 95 * scale));
        QTest::mouseMove(view, origin + QPoint(190 * scale, 130 * scale));
        QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier,
                            origin + QPoint(190 * scale, 130 * scale));
        QVERIFY(requested);
        view->setWords({QJsonObject{{"x", 80 - offset.x()},
                                    {"y", 100 - offset.y()},
                                    {"width", 100},
                                    {"height", 20},
                                    {"line", "1"},
                                    {"text", "Snapshot"}}},
                       offset);
        QCOMPARE(sent["type"].toString(), "ocr_selected");
        QCOMPARE(sent["text"].toString(), "Snapshot");
        QCOMPARE(QApplication::clipboard()->text(), "Snapshot");
        QVERIFY(view->isVisible()); // Controller switches directly to the quick card.
        view->close();
    }
    void multiple_snapshot_cards_data() {
        QTest::addColumn<int>("modifier");
        QTest::newRow("shift") << int(Qt::ShiftModifier);
        QTest::newRow("control") << int(Qt::ControlModifier);
    }
    void multiple_snapshot_cards() {
        QFETCH(int, modifier);
        QImage image(800, 600, QImage::Format_RGB32);
        image.fill(Qt::white);
        auto *view = new SnapshotView(image, false, {});
        QTest::qWait(30);
        const QString session = sent["session"].toString();
        QJsonArray words;
        for (int i = 0; i < 3; ++i)
            words.append(QJsonObject{{"x", 90},
                                     {"y", 150 + 100 * i},
                                     {"width", 100},
                                     {"height", 22},
                                     {"line", QString::number(i)},
                                     {"text", QString("Phrase %1").arg(i + 1)}});
        view->setWords(words);
        const double scale = qMin(view->width() / 800.0, view->height() / 600.0);
        const QPoint origin((view->width() - 800 * scale) / 2, (view->height() - 600 * scale) / 2);
        QList<QJsonObject> requests;
        for (int i = 0; i < 3; ++i) {
            auto mods = i == 0 ? Qt::KeyboardModifiers(modifier) : Qt::NoModifier;
            QTest::mousePress(view, Qt::LeftButton, mods,
                              origin + QPoint(85 * scale, (145 + i * 100) * scale));
            QTest::mouseMove(view, origin + QPoint(195 * scale, (175 + i * 100) * scale));
            // Releasing the modifier before the mouse must not lose the keep-open gesture.
            QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier,
                                origin + QPoint(195 * scale, (175 + i * 100) * scale));
            QCOMPARE(sent["type"].toString(), "snapshot_request");
            QCOMPARE(sent["text"].toString(), QString("Phrase %1").arg(i + 1));
            requests.append(sent);
        }
        QVERIFY(view->isVisible());
        // Deliberately deliver translations in a different order than selections.
        for (int i : {2, 0, 1}) {
            auto value = requests[i];
            value["phase"] = "translation";
            value["text"] = QString("Result %1").arg(i + 1);
            value["source"] = "en";
            value["target"] = "ru";
            view->receiveRegion(value);
        }
        QWidget *cards[3];
        for (int i = 0; i < 3; ++i) {
            cards[i] = view->findChild<QWidget *>(QString("snapshotCard%1").arg(i + 1));
            QVERIFY(cards[i]);
            QVERIFY(!cards[i]->isWindow());
            QCOMPARE(cards[i]->findChild<QPlainTextEdit *>()->toPlainText(),
                     QString("Result %1").arg(i + 1));
        }
        // Overlap and click the text viewport: the whole card, not just its header, raises.
        cards[0]->move(180, 180);
        cards[1]->move(200, 200);
        auto *editor = cards[0]->findChild<QPlainTextEdit *>();
        QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(5, 5));
        QWidget *top = view->childAt(QPoint(220, 220));
        QVERIFY(top == cards[0] || cards[0]->isAncestorOf(top));
        auto *handle = cards[0]->findChild<QWidget *>("cardDragHandle");
        const QPoint previous = cards[0]->pos();
        QTest::mousePress(handle, Qt::LeftButton, Qt::NoModifier, QPoint(75, 10));
        QTest::mouseMove(handle, QPoint(125, 45));
        QTest::mouseRelease(handle, Qt::LeftButton, Qt::NoModifier, QPoint(125, 45));
        QVERIFY(cards[0]->pos() != previous);
        QVERIFY(view->rect().contains(cards[0]->geometry()));
        // A stale result from another screenshot may never change a visible card.
        auto stale = requests[0];
        stale["session"] = "previous-screenshot";
        stale["phase"] = "translation";
        stale["text"] = "STALE";
        view->receiveRegion(stale);
        QCOMPARE(editor->toPlainText(), "Result 1");
        // Changing one target creates a new request and ignores its old response.
        auto *target = cards[0]->findChild<QComboBox *>("snapshotTargetLanguage");
        target->setCurrentIndex(target->findData("de"));
        QMetaObject::invokeMethod(target, "activated", Q_ARG(int, target->currentIndex()));
        QCOMPARE(sent["target"].toString(), "de");
        QVERIFY(sent["id"].toInt() != requests[0]["id"].toInt());
        stale = requests[0];
        stale["phase"] = "translation";
        stale["text"] = "OLD TARGET";
        view->receiveRegion(stale);
        QVERIFY(editor->toPlainText().isEmpty());
        named(cards[1], "Close this translation")->click();
        QCOMPARE(sent["type"].toString(), "snapshot_cancel_region");
        QCOMPARE(sent["id"], requests[1]["id"]);
        QVERIFY(cards[0]->isVisible());
        QVERIFY(cards[2]->isVisible());
        // Escape must work even when an embedded text editor has focus.
        editor->setFocus();
        messages.clear();
        QTest::keyClick(editor, Qt::Key_Escape);
        QVERIFY(!view->isVisible());
        QVERIFY(std::any_of(messages.begin(), messages.end(), [session](auto v) {
            return v["type"] == "snapshot_end" && v["session"] == session;
        }));
    }
    void multiple_regions_before_ocr_keep_independent_crops_and_errors() {
        QImage image(800, 600, QImage::Format_RGB32);
        image.fill(Qt::white);
        auto *view = new SnapshotView(image, true, [](QImage, QPoint) {
            QFAIL("Multiple selections must not replace the full-screen OCR request");
        });
        QTest::qWait(30);
        QList<QJsonObject> requests;
        QStringList files;
        for (int i = 0; i < 3; ++i) {
            QTest::mousePress(view, Qt::LeftButton, Qt::ShiftModifier, QPoint(90, 160 + i * 100));
            QTest::mouseRelease(view, Qt::LeftButton, Qt::ShiftModifier,
                                QPoint(190, 180 + i * 100));
            QCOMPARE(sent["type"].toString(), "snapshot_request");
            requests.append(sent);
            files.append(sent["path"].toString());
            QVERIFY(QFile::exists(files.last()));
        }
        QCOMPARE(QSet<QString>(files.begin(), files.end()).size(), 3);
        auto source = requests[1];
        source["phase"] = "source";
        source["text"] = "Second region";
        source["words"] =
            QJsonArray{QJsonObject{{"x", 90}, {"y", 260}, {"width", 100}, {"height", 20}}};
        view->receiveRegion(source);
        QCOMPARE(QApplication::clipboard()->text(), "Second region");
        auto *second = view->findChild<QWidget *>("snapshotCard2");
        QCOMPARE(second->accessibleDescription(), "Second region");
        auto error = requests[0];
        error["phase"] = "error";
        error["text"] = "Provider is offline";
        view->receiveRegion(error);
        auto *first = view->findChild<QWidget *>("snapshotCard1");
        QVERIFY(named(first, "Retry")->isVisible());
        QVERIFY(second->findChild<QPlainTextEdit *>()->toPlainText().isEmpty());
        named(first, "Retry")->click();
        QCOMPARE(sent["type"].toString(), "snapshot_request");
        QCOMPARE(sent["path"], requests[0]["path"]);
        // A late full-screen OCR result must neither close the scene nor submit a fourth card.
        view->setWords({QJsonObject{
            {"x", 90}, {"y", 160}, {"width", 100}, {"height", 20}, {"text", "Full screen"}}});
        QVERIFY(view->isVisible());
        QVERIFY(!view->findChild<QWidget *>("snapshotCard4"));
        view->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        for (const auto &path : files)
            QVERIFY(!QFile::exists(path));
    }
    void quick_translation_dismisses_when_another_window_is_activated() {
        bool dismissed = false;
        QuickPopup popup(nullptr, [] {}, [](QString, QString) {}, [&] { dismissed = true; });
        popup.begin(defaults());
        QTest::qWait(160);
        QWidget other;
        other.show();
        other.activateWindow();
        QTRY_VERIFY_WITH_TIMEOUT(dismissed, 1000);
        QVERIFY(!popup.isVisible());
    }
    void quick_translation_uses_tool_window_and_expands_explicitly() {
        QWidget owner;
        bool expanded = false, dismissed = false;
        QuickPopup popup(
            &owner, [&] { expanded = true; }, [](QString, QString) {}, [&] { dismissed = true; });
        popup.begin(defaults());
        popup.display({{"text", "Hola"}, {"source", "en"}, {"target", "es"}});
        QCOMPARE(popup.windowType(), Qt::Tool);
        QVERIFY(!owner.isVisible());
        named(&popup, "Open full translator")->click();
        QVERIFY(expanded);
        QVERIFY(!popup.isVisible());
        QVERIFY(!dismissed);
        popup.begin(defaults());
        named(&popup, "Dismiss translation")->click();
        QVERIFY(dismissed);
        QVERIFY(!popup.isVisible());
    }
    void snapshot_source_override_is_per_card_and_ignores_old_results() {
        QImage image(800, 600, QImage::Format_RGB32);
        image.fill(Qt::white);
        auto *view = new SnapshotView(image, false, {});
        QTest::qWait(30);
        view->setWords({QJsonObject{{"x", 70},
                                    {"y", 160},
                                    {"width", 110},
                                    {"height", 25},
                                    {"line", "1"},
                                    {"text", "repository"}},
                        QJsonObject{{"x", 70},
                                    {"y", 300},
                                    {"width", 110},
                                    {"height", 25},
                                    {"line", "2"},
                                    {"text", "second"}}});
        const double scale = qMin(view->width() / 800.0, view->height() / 600.0);
        const QPoint origin((view->width() - 800 * scale) / 2, (view->height() - 600 * scale) / 2);
        QList<QJsonObject> requests;
        for (int y : {160, 300}) {
            QTest::mousePress(view, Qt::LeftButton, Qt::ShiftModifier,
                              origin + QPoint(65 * scale, (y - 5) * scale));
            QTest::mouseRelease(view, Qt::LeftButton, Qt::ShiftModifier,
                                origin + QPoint(185 * scale, (y + 30) * scale));
            requests.append(sent);
        }
        auto *first = view->findChild<QWidget *>("snapshotCard1");
        auto *second = view->findChild<QWidget *>("snapshotCard2");
        QVERIFY(first && second);
        auto initial = requests[0];
        initial["phase"] = "translation";
        initial["source"] = "ro";
        initial["target"] = "en";
        initial["text"] = "Incorrect guess";
        view->receiveRegion(initial);
        auto *source = first->findChild<QComboBox *>("snapshotSourceLanguage");
        auto *target = first->findChild<QComboBox *>("snapshotTargetLanguage");
        QCOMPARE(languageValue(source), "auto");
        QCOMPARE(source->currentText(), "Auto · RO");
        setLanguage(target, "de");
        QMetaObject::invokeMethod(target, "activated", Q_ARG(int, target->currentIndex()));
        setLanguage(source, "en");
        QMetaObject::invokeMethod(source, "activated", Q_ARG(int, source->currentIndex()));
        const auto corrected = sent;
        QCOMPARE(corrected["source"].toString(), "en");
        QCOMPARE(corrected["target"].toString(), "de");
        QCOMPARE(corrected["text"].toString(), "repository");
        QVERIFY(corrected["id"] != requests[0]["id"]);
        QCOMPARE(languageValue(second->findChild<QComboBox *>("snapshotSourceLanguage")), "auto");
        view->receiveRegion(initial);
        QVERIFY(first->findChild<QPlainTextEdit *>()->toPlainText().isEmpty());
        auto failure = corrected;
        failure["phase"] = "error";
        failure["text"] = "Offline";
        view->receiveRegion(failure);
        named(first, "Retry")->click();
        QCOMPARE(sent["source"].toString(), "en");
        QCOMPARE(sent["target"].toString(), "de");
        QCOMPARE(sent["text"].toString(), "repository");
        // Free-form source codes use the same request path, with no duplicate submit on blur.
        source->setEditText("eo");
        QTest::keyClick(source->lineEdit(), Qt::Key_Return);
        QCOMPARE(sent["source"].toString(), "eo");
        QVERIFY(!view->findChild<QWidget *>("snapshotCard3"));
        const auto count = messages.size();
        QMetaObject::invokeMethod(source->lineEdit(), "editingFinished");
        QCOMPARE(messages.size(), count);
        view->close();
    }
    void quick_source_override_retries_original_text_and_rejects_stale_reply() {
        Window window(defaults(), true);
        window.receive(
            {{"type", "selected"}, {"text", "repository"}, {"request_id", "initial-selection"}});
        window.receive({{"type", "translation"},
                        {"text", "Wrong guess"},
                        {"source", "ro"},
                        {"target", "en"},
                        {"request_id", "initial-selection"}});
        auto *source = window.findChild<QComboBox *>("quickSourceLanguage");
        auto *target = window.findChild<QComboBox *>("quickTargetLanguage");
        auto *result = window.findChild<QPlainTextEdit *>("quickTranslation");
        QVERIFY(source && target && result);
        QCOMPARE(source->currentText(), "Auto · RO");
        setLanguage(target, "ru");
        QMetaObject::invokeMethod(target, "activated", Q_ARG(int, target->currentIndex()));
        const auto previous = sent;
        setLanguage(source, "en");
        QMetaObject::invokeMethod(source, "activated", Q_ARG(int, source->currentIndex()));
        auto corrected = sent;
        QCOMPARE(corrected["type"].toString(), "translate");
        QCOMPARE(corrected["source"].toString(), "en");
        QCOMPARE(corrected["target"].toString(), "ru");
        QCOMPARE(corrected["text"].toString(), "repository");
        QVERIFY(corrected["request_id"] != previous["request_id"]);
        window.receive({{"type", "translation"},
                        {"text", "Stale"},
                        {"source", "ro"},
                        {"target", "ru"},
                        {"request_id", previous["request_id"]}});
        QVERIFY(result->toPlainText().isEmpty());
        window.receive(
            {{"type", "error"}, {"text", "Old failure"}, {"request_id", previous["request_id"]}});
        QVERIFY(result->toPlainText().isEmpty());
        window.receive(
            {{"type", "error"}, {"text", "Offline"}, {"request_id", corrected["request_id"]}});
        named(&window, "Retry quick translation")->click();
        QVERIFY(sent["request_id"] != corrected["request_id"]);
        corrected = sent;
        QCOMPARE(corrected["source"].toString(), "en");
        QCOMPARE(corrected["target"].toString(), "ru");
        QCOMPARE(corrected["text"].toString(), "repository");
        window.receive({{"type", "translation"},
                        {"text", "Репозиторий"},
                        {"source", "en"},
                        {"target", "ru"},
                        {"request_id", corrected["request_id"]}});
        QCOMPARE(result->toPlainText(), QString("Репозиторий"));
        QCOMPARE(languageValue(source), "en");
        // A new independent selection always starts in automatic mode.
        window.receive(
            {{"type", "selected"}, {"text", "Another phrase"}, {"request_id", "next-selection"}});
        QCOMPARE(languageValue(source), "auto");
        QCOMPARE(languageValue(target), "auto");
        window.receive({{"type", "translation"},
                        {"text", "Old selection"},
                        {"source", "en"},
                        {"target", "ru"},
                        {"request_id", corrected["request_id"]}});
        QVERIFY(result->toPlainText().isEmpty());
    }
    void full_translator_focuses_a_visible_editor_every_time_it_opens() {
        Window window(defaults(), true);
        window.launch({{"_window", true}});
        auto *source = window.findChild<QPlainTextEdit *>("sourceText");
        auto *surface = window.findChild<QWidget *>("sourceEditorSurface");
        QVERIFY(source && surface);
        QTRY_VERIFY(source->hasFocus());
        QVERIFY(source->height() >= 96);
        QVERIFY(surface->rect().contains(QRect(source->mapTo(surface, QPoint()), source->size())));
        QTest::keyClicks(QApplication::focusWidget(), "Text entered immediately");
        QCOMPARE(source->toPlainText(), QString("Text entered immediately"));
        named(&window, "Translate")->setFocus();
        QTest::mouseClick(surface, Qt::LeftButton, Qt::NoModifier, QPoint(5, 70));
        QTRY_VERIFY(source->hasFocus());
        named(&window, "Translate")->setFocus();
        window.hide();
        QTest::qWait(30);
        window.launch({{"_window", true}});
        if (QGuiApplication::platformName() == "offscreen")
            QApplication::setActiveWindow(&window);
        QTRY_VERIFY(source->hasFocus());
        QCOMPARE(source->toPlainText(), QString("Text entered immediately"));
    }
    void listen_hides_the_editor_and_clears_stale_application_focus() {
        Window window(defaults(), true);
        window.launch({{"_window", true}});
        auto *source = window.findChild<QPlainTextEdit *>("sourceText");
        QTRY_VERIFY(source->hasFocus());
        auto focus = [] {
            for (auto it = messages.crbegin(); it != messages.crend(); ++it)
                if ((*it)["type"] == "focus")
                    return (*it)["active"].toBool();
            return false;
        };
        QTRY_VERIFY(focus());
        named(&window, "Keep listening")->click();
        QCOMPARE(sent["type"].toString(), QString("action"));
        QCOMPARE(sent["action"].toString(), QString("continuous"));
        window.receive({{"type", "hide"}});
        window.receive({{"type", "listening"}, {"mode", "continuous"}, {"generation", 1}});
        QVERIFY(!window.isVisible());
        QVERIFY(!focus());
        // Some Wayland compositors retain application activation after the last surface hides.
        QMetaObject::invokeMethod(qApp, "applicationStateChanged", Qt::DirectConnection,
                                  Q_ARG(Qt::ApplicationState, Qt::ApplicationActive));
        QVERIFY(!focus());
        auto *listen = named(&window, "Keep listening");
        QVERIFY(listen->isChecked());
        QCOMPARE(listen->text(), QString("Auto: on"));
        QTest::qWait(30);
        window.launch({{"_window", true}});
        if (QGuiApplication::platformName() == "offscreen")
            QApplication::setActiveWindow(&window);
        QTRY_VERIFY(source->hasFocus());
        QTRY_VERIFY(focus());
        listen->click();
        QCOMPARE(sent["action"].toString(), QString("continuous"));
        window.receive({{"type", "listening"}, {"mode", "off"}, {"generation", 2}});
        QVERIFY(!listen->isChecked());
        QCOMPARE(listen->text(), QString("Auto translate"));
    }
    void snapshot_reader_preserves_other_cards_and_shift_selection() {
        QImage image(800, 600, QImage::Format_RGB32);
        image.fill(Qt::white);
        auto *view = new SnapshotView(image, false, {});
        QTest::qWait(20);
        view->setWords({QJsonObject{{"x", 60},
                                    {"y", 220},
                                    {"width", 100},
                                    {"height", 24},
                                    {"line", "1"},
                                    {"text", "First"}},
                        QJsonObject{{"x", 60},
                                    {"y", 380},
                                    {"width", 100},
                                    {"height", 24},
                                    {"line", "2"},
                                    {"text", "Second"}}});
        const double scale = qMin(view->width() / 800.0, view->height() / 600.0);
        const QPoint origin((view->width() - 800 * scale) / 2, (view->height() - 600 * scale) / 2);
        QList<QJsonObject> requests;
        for (int y : {220, 380}) {
            QTest::mousePress(view, Qt::LeftButton, Qt::ShiftModifier,
                              origin + QPoint(55 * scale, (y - 5) * scale));
            QTest::mouseRelease(view, Qt::LeftButton, Qt::ShiftModifier,
                                origin + QPoint(165 * scale, (y + 28) * scale));
            requests.append(sent);
        }
        auto *first = static_cast<SnapshotCard *>(view->findChild<QWidget *>("snapshotCard1"));
        auto *second = view->findChild<QWidget *>("snapshotCard2");
        QVERIFY(first && second);
        auto response = requests[0];
        response["phase"] = "translation";
        response["text"] = QString("A long translation for comfortable reading.\n").repeated(120);
        response["source"] = "en";
        response["target"] = "ru";
        view->receiveRegion(response);
        auto *result = first->findChild<QPlainTextEdit *>();
        const QRect compact = first->geometry(), other = second->geometry();
        const auto before = messages.size();
        named(first, "Read this translation full screen")->click();
        QVERIFY(first->isReading());
        QCOMPARE(first->geometry(), view->rect().adjusted(16, 16, -16, -16));
        QVERIFY(second->isVisible());
        QVERIFY(!second->isEnabled());
        QCOMPARE(result->toPlainText(), response["text"].toString());
        QVERIFY(result->verticalScrollBar()->maximum() > 0);
        // The overlay's exposed margin cannot create new regions or pan/zoom behind a reader.
        QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier, QPoint(4, 4));
        QTest::mousePress(view, Qt::RightButton, Qt::NoModifier, QPoint(4, 4));
        QTest::mouseMove(view, QPoint(12, 12));
        QTest::mouseRelease(view, Qt::RightButton, Qt::NoModifier, QPoint(12, 12));
        QWheelEvent wheel(QPointF(4, 4), view->mapToGlobal(QPoint(4, 4)), QPoint(), QPoint(0, 120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(view, &wheel);
        QTest::keyClick(view, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(view, Qt::Key_Return);
        QVERIFY(first->isReading());
        QCOMPARE(view->findChildren<QWidget *>(QRegularExpression("^snapshotCard[0-9]+$")).size(),
                 2);
        QCOMPARE(first->geometry(), view->rect().adjusted(16, 16, -16, -16));
        // Dropdown navigation takes priority over reader shortcuts.
        auto *source = first->findChild<QComboBox *>("snapshotSourceLanguage");
        source->showPopup();
        QTRY_VERIFY(QApplication::activePopupWidget());
        QTest::keyClick(result, Qt::Key_Shift);
        QVERIFY(first->isReading());
        QTest::keyClick(QApplication::activePopupWidget(), Qt::Key_Escape);
        QTRY_VERIFY(!QApplication::activePopupWidget());
        QVERIFY(first->isReading());
        result->setFocus();
        auto cursor = result->textCursor();
        cursor.movePosition(QTextCursor::Start);
        result->setTextCursor(cursor);
        QTest::keyPress(result, Qt::Key_Shift);
        QTest::keyClick(result, Qt::Key_Right, Qt::ShiftModifier);
        QTest::keyRelease(result, Qt::Key_Shift);
        QVERIFY(first->isReading());
        QVERIFY(result->textCursor().hasSelection());
        QTest::keyPress(result, Qt::Key_Shift);
        QTest::mouseClick(result->viewport(), Qt::LeftButton, Qt::ShiftModifier, QPoint(60, 20));
        QTest::keyRelease(result, Qt::Key_Shift);
        QVERIFY(first->isReading());
        QTest::keyPress(result, Qt::Key_Shift);
        QVERIFY(first->isReading());
        QTest::keyRelease(result, Qt::Key_Shift);
        QVERIFY(!first->isReading());
        QCOMPARE(first->geometry(), compact);
        QCOMPARE(second->geometry(), other);
        QVERIFY(second->isEnabled());
        named(first, "Read this translation full screen")->click();
        QTest::keyClick(result, Qt::Key_Escape);
        QVERIFY(!first->isReading());
        QVERIFY(view->isVisible());
        QCOMPARE(view->findChildren<QWidget *>(QRegularExpression("^snapshotCard[0-9]+$")).size(),
                 2);
        QCOMPARE(result->toPlainText(), response["text"].toString());
        for (const auto &message : messages.mid(before))
            QVERIFY(message["type"] != "snapshot_end" && message["type"] != "snapshot_request");
        view->close();
    }
    void translation_widgets_resize_and_quick_reader_restores_the_widget() {
        auto resizeBy = [](QWidget *widget, QPoint delta) {
            auto *handle = widget->findChild<QWidget *>("translationResizeHandle");
            QVERIFY(handle);
            const QPoint origin = handle->rect().center();
            const QPoint end = handle->mapToGlobal(origin) + delta;
            QTest::mousePress(handle, Qt::LeftButton, Qt::NoModifier, origin);
            QMouseEvent move(QEvent::MouseMove, handle->mapFromGlobal(end), end, Qt::NoButton,
                             Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(handle, &move);
            QMouseEvent release(QEvent::MouseButtonRelease, handle->mapFromGlobal(end), end,
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(handle, &release);
        };
        QWidget surface;
        surface.resize(900, 700);
        SnapshotCard card(&surface, 1, QColor("#477edb"));
        card.move(20, 100);
        surface.show();
        card.show();
        const QSize cardSize = card.size();
        resizeBy(&card, QPoint(65, 50));
        QCOMPARE(card.size(), cardSize + QSize(65, 50));
        surface.hide();
        bool dismissed = false;
        QuickPopup popup(nullptr, [] {}, [](QString, QString) {}, [&] { dismissed = true; });
        popup.begin(defaults());
        popup.display({{"text", "Short"}, {"source", "en"}, {"target", "ru"}});
        popup.move(10, 10);
        const QSize compactSize = popup.size();
        resizeBy(&popup, QPoint(60, 45));
        QCOMPARE(popup.size(), compactSize + QSize(60, 45));
        const QRect compact = popup.geometry();
        popup.display({{"text", QString("Long reading text.\n").repeated(100)},
                       {"source", "en"},
                       {"target", "ru"}});
        QCOMPARE(popup.geometry(), compact); // New results retain the user's chosen size.
        auto *result = popup.findChild<QPlainTextEdit *>("quickTranslation");
        named(&popup, "Read translation full screen")->click();
        QVERIFY(popup.isReading());
        QVERIFY(result->verticalScrollBar()->maximum() > 0);
        QTest::keyClick(result, Qt::Key_Escape);
        QVERIFY(!popup.isReading());
        QVERIFY(popup.isVisible());
        QVERIFY(!dismissed);
        QCOMPARE(popup.geometry(), compact);
        named(&popup, "Read translation full screen")->click();
        QTest::keyPress(result, Qt::Key_Shift);
        QVERIFY(popup.isReading());
        QTest::keyRelease(result, Qt::Key_Shift);
        QVERIFY(!popup.isReading());
        QVERIFY(!dismissed);
        QCOMPARE(popup.geometry(), compact);
    }
    void snapshot_all_actions_are_visible_and_translate_without_opening_zoom_controls() {
        QImage image(800, 600, QImage::Format_RGB32);
        image.fill(Qt::white);
        auto *view = new SnapshotView(image, false, {});
        auto *all = named(view, "Select all");
        auto *translateAll = named(view, "Translate all");
        QVERIFY(all && translateAll);
        QVERIFY(all->isVisible() && translateAll->isVisible());
        QVERIFY(!all->isEnabled() && !translateAll->isEnabled());
        view->setWords({QJsonObject{{"x", 80},
                                    {"y", 220},
                                    {"width", 100},
                                    {"height", 24},
                                    {"line", "1"},
                                    {"text", "All text"}}});
        QVERIFY(all->isEnabled() && translateAll->isEnabled());
        translateAll->click();
        QCOMPARE(sent["type"].toString(), QString("ocr_selected"));
        QCOMPARE(sent["text"].toString(), QString("All text"));
        view->close();
    }
    void autostart_tray_and_settings_follow_actual_registration_and_failed_changes() {
#ifdef Q_OS_LINUX
        QTemporaryDir directory;
        Autostart::Environment env;
        env.configHome = directory.path();
        env.executable = QCoreApplication::applicationFilePath();
        Autostart startup(env);
        Window window(defaults(), true, &startup);
        named(&window, "Open settings")->click();
        auto *dialog = window.findChild<QDialog *>();
        named(dialog, "Startup")->click();
        auto *toggle = dialog->findChild<QCheckBox *>("autostartToggle");
        auto *action = window.findChild<QAction *>("autostartAction");
        QVERIFY(toggle && action);
        QVERIFY(!toggle->isChecked() && !action->isChecked());
        action->trigger();
        QVERIFY(startup.isBusy());
        QVERIFY(!toggle->isEnabled() && !action->isEnabled());
        // Closing/reopening settings during the operation must not update stale widgets.
        dialog->deleteLater();
        QCoreApplication::sendPostedEvents(dialog, QEvent::DeferredDelete);
        named(&window, "Open settings")->click();
        dialog = window.findChild<QDialog *>();
        named(dialog, "Startup")->click();
        toggle = dialog->findChild<QCheckBox *>("autostartToggle");
        QTRY_VERIFY(!startup.isBusy());
        QVERIFY(toggle->isChecked() && action->isChecked());
        QVERIFY(toggle->isEnabled() && action->isEnabled());
        const auto path = directory.path() + "/autostart/io.github.ranslator.Ranslator.desktop";
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("[Desktop Entry]\nHidden=true\n");
        file.close();
        startup.refresh();
        QVERIFY(!toggle->isChecked() && !action->isChecked());
        // Assistive clients can invoke either advertised checkbox action. Both must
        // register the change, rather than only changing the visible check mark.
        auto *accessible = QAccessible::queryAccessibleInterface(toggle);
        QVERIFY(accessible && accessible->actionInterface());
        auto *actions = accessible->actionInterface();
        actions->doAction(QAccessibleActionInterface::toggleAction());
        QVERIFY(startup.isBusy());
        QTRY_VERIFY(!startup.isBusy());
        QVERIFY(toggle->isChecked() && action->isChecked());
        QVERIFY(startup.state().enabled);
        actions->doAction(QAccessibleActionInterface::pressAction());
        QTRY_VERIFY(!toggle->isChecked() && !action->isChecked());
        QTRY_VERIFY(!startup.isBusy());
        QVERIFY(!startup.state().enabled);
        QVERIFY(QFile::remove(path));
        QVERIFY(QDir(directory.path() + "/autostart").removeRecursively());
        QFile obstruction(directory.path() + "/autostart");
        QVERIFY(obstruction.open(QIODevice::WriteOnly));
        obstruction.close();
        toggle->click();
        QTRY_VERIFY(!startup.isBusy());
        QVERIFY(!toggle->isChecked() && !action->isChecked());
        QVERIFY(dialog->findChild<QLabel *>("autostartDetail")->text().contains("Cannot"));
#else
        QSKIP("This fixture writes a private XDG profile; macOS requires its native test machine.");
#endif
    }
};
QTEST_MAIN(UiTests)
#include "ui.moc"
