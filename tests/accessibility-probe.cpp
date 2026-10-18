#include "accessibility.hpp"
#include <QCoreApplication>
#include <QTimer>
#include <cstdio>

// Runs in the real Flatpak sandbox against the test's synthetic host AT-SPI bus.
// It intentionally has no QtTest dependency, matching production dependencies.
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    AccessibilitySelection selection;
    QObject::connect(&selection, &AccessibilitySelection::availability, &app,
                     [&](bool available, const QString &detail, quint64 generation) {
                         if (available && generation == 93) {
                             printf("READY\n");
                             fflush(stdout);
                         } else {
                             fprintf(stderr, "%s\n", qPrintable(detail));
                             app.exit(2);
                         }
                     });
    QObject::connect(&selection, &AccessibilitySelection::selected, &app,
                     [&](const QString &text, quint64 generation) {
                         if (text == "Hello" && generation == 93) {
                             printf("PASS: synthetic host selection reached Flatpak.\n");
                             app.exit(0);
                         } else {
                             app.exit(4);
                         }
                     });
    QTimer::singleShot(0, [&] { selection.setArmed(true, 93); });
    QTimer::singleShot(10000, &app, [&] { app.exit(3); });
    return app.exec();
}
