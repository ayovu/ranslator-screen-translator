#include <QtWidgets>

// A synthetic screen for live screenshot tests; never capture another application's content.
class Fixture : public QWidget {
    QString text;
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::white);
        painter.setPen(Qt::black);
        painter.setFont(QFont("Sans Serif", 32));
        painter.drawText(rect().adjusted(80, 80, -80, -80), Qt::AlignLeft | Qt::AlignTop, text);
    }

  public:
    explicit Fixture(QString value) : text(std::move(value)) {}
};
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (app.arguments().contains("--all-monitors")) {
        QJsonArray monitors;
        int index = 0;
        for (auto *screen : app.screens()) {
            const auto text = QString("Ranslator display %1.").arg(index++ == 0 ? "Alpha" : "Beta");
            auto *window = new Fixture(text);
            window->setWindowTitle("Ranslator monitor fixture " + screen->name());
            window->setScreen(screen);
            window->winId();
            window->windowHandle()->setScreen(screen);
            window->setGeometry(screen->geometry());
            window->showFullScreen();
            monitors.append(QJsonObject{{"name", screen->name()},
                                        {"text", text},
                                        {"window", QString::number(window->winId())},
                                        {"x", screen->geometry().x()},
                                        {"y", screen->geometry().y()},
                                        {"width", screen->geometry().width()},
                                        {"height", screen->geometry().height()}});
        }
        QTextStream(stdout) << QJsonDocument(monitors).toJson(QJsonDocument::Compact) << Qt::endl;
        QTimer::singleShot(180000, &app, &QApplication::quit);
        return app.exec();
    }
    Fixture window("Ranslator snapshot test.\nThis sentence is ready to translate.");
    window.setWindowTitle("Ranslator test fixture");
    if (auto *screen = QGuiApplication::screenAt(QCursor::pos())) {
        window.setScreen(screen);
        window.setGeometry(screen->geometry());
    }
    window.showFullScreen();
    QTimer::singleShot(180000, &app, &QApplication::quit);
    return app.exec();
}
