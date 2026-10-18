#include "autostart.hpp"
#include "i18n.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#ifdef Q_OS_MACOS
#include "macos.hpp"
#endif

namespace {
constexpr auto desktopName = "io.github.ranslator.Ranslator.desktop";
QString normalized(const QString &path) {
    const auto canonical = QFileInfo(path).canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(path) : canonical;
}
QString desktopString(const QString &value) {
    QString result;
    for (const auto c : value) {
        if (c == '\\')
            result += "\\\\";
        else if (c == ' ')
            result += "\\s";
        else if (c == '\t')
            result += "\\t";
        else if (c == '\n')
            result += "\\n";
        else if (c == '\r')
            result += "\\r";
        else
            result += c;
    }
    return result;
}
QString unescapeString(const QString &value) {
    QString result;
    for (qsizetype i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) {
            const auto c = value[++i];
            if (c == 's')
                result += ' ';
            else if (c == 'n')
                result += '\n';
            else if (c == 'r')
                result += '\r';
            else if (c == 't')
                result += '\t';
            else
                result += c;
        } else
            result += value[i];
    }
    return result;
}
QMap<QString, QString> desktopValues(const QByteArray &bytes) {
    QMap<QString, QString> values;
    bool entry = false;
    for (const auto &line : QString::fromUtf8(bytes).split('\n')) {
        if (line.startsWith('[')) {
            entry = line.trimmed() == "[Desktop Entry]";
            continue;
        }
        if (!entry || line.startsWith('#'))
            continue;
        const auto separator = line.indexOf('=');
        if (separator > 0)
            values.insert(line.left(separator).trimmed(), line.mid(separator + 1).trimmed());
    }
    return values;
}
QString writeFile(const QString &path, const QByteArray &contents) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return UiText::tr("Cannot create the autostart directory. Check its permissions: %1")
            .arg(path);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return UiText::tr("Cannot update autostart: %1 (%2)").arg(file.errorString(), path);
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.write(contents) != contents.size() || !file.commit())
        return UiText::tr("Could not save autostart: %1").arg(file.errorString());
    return {};
}
} // namespace

Autostart::Environment Autostart::Environment::current() {
    Environment env;
    env.configHome = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    env.configDirs =
        qEnvironmentVariable("XDG_CONFIG_DIRS", "/etc/xdg").split(':', Qt::SkipEmptyParts);
    env.desktops = qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(':', Qt::SkipEmptyParts);
    env.executable = QCoreApplication::applicationFilePath();
    env.appImage = qEnvironmentVariable("APPIMAGE");
    env.appDir = qEnvironmentVariable("APPDIR");
    env.flatpak = QFile::exists("/.flatpak-info");
    return env;
}
Autostart::Autostart(QObject *parent, bool allowChanges)
    : Autostart(Environment::current(), parent, allowChanges) {}
Autostart::Autostart(Environment env, QObject *parent, bool allowChanges)
    : QObject(parent), environment(std::move(env)), portal(this), allowed(allowChanges) {
    refresh();
}
QString Autostart::entryPath() const {
    return environment.configHome + "/autostart/" + desktopName;
}
QString Autostart::confirmationPath() const {
    return environment.configHome + "/ranslator/autostart-confirmation.json";
}
QString Autostart::launcher(const Environment &env, QString *error) {
    QString command = env.executable;
    // APPIMAGE can belong to the terminal's parent application (for example an editor).
    const bool ownAppImage =
        !env.appDir.isEmpty() &&
        normalized(env.executable) == normalized(env.appDir + "/usr/bin/ranslator");
    if (ownAppImage && !env.appImage.isEmpty()) {
        command = env.appImage;
    } else {
        const auto root = QDir::cleanPath(QFileInfo(env.executable).absolutePath() + "/../..");
        const auto appRun = root + "/AppRun";
        if (normalized(env.executable) == normalized(root + "/usr/bin/ranslator") &&
            QFileInfo(appRun).isExecutable())
            command = normalized(appRun);
    }
    if (!QDir::isAbsolutePath(command) || !QFileInfo(command).isFile() ||
        !QFileInfo(command).isExecutable()) {
        *error =
            UiText::tr("Keep Ranslator in a permanent location, then enable Start at login again. "
                       "The launcher is unavailable: %1")
                .arg(command);
        return {};
    }
    if (command.contains("/.mount_") || command.contains("/appimage_extracted_")) {
        *error = UiText::tr(
            "The AppImage's original launcher could not be found. Run the saved AppImage "
            "from a permanent location and enable Start at login again.");
        return {};
    }
    if (command.contains('=') || command.contains('\n') || command.contains('\r') ||
        command.contains(QChar::Null)) {
        *error = UiText::tr(
            "Move Ranslator to a path without '=' or line breaks, then enable Start at login.");
        return {};
    }
    return QDir::cleanPath(command);
}
QString Autostart::desktopExec(const QString &executable) {
    // Desktop Entry has two escaping layers: string values, then quoted Exec arguments.
    QString quoted;
    for (const auto c : executable) {
        if (c == '%')
            quoted += "%%";
        else {
            if (c == '\\' || c == '"' || c == '$' || c == '`')
                quoted += '\\';
            quoted += c;
        }
    }
    quoted.replace("\\", "\\\\");
    quoted.replace('\t', "\\t");
    return QString("/usr/bin/env -- \"") + quoted + '"';
}
AutostartState Autostart::readNative() const {
    AutostartState state;
    QString path = entryPath();
    if (!QFile::exists(path)) {
        for (const auto &directory : environment.configDirs) {
            const auto candidate = directory + "/autostart/" + desktopName;
            if (QFile::exists(candidate)) {
                path = candidate;
                break;
            }
        }
    }
    QFile file(path);
    if (!file.exists()) {
        state.detail = UiText::tr("Off. Ranslator will start only when you open it.");
        return state;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        state.known = false;
        state.detail =
            UiText::tr("Cannot read the system autostart entry: %1").arg(file.errorString());
        return state;
    }
    const auto values = desktopValues(file.readAll());
    state.enabled = values.value("Hidden") != "true" &&
                    values.value("X-GNOME-Autostart-enabled", "true") != "false" &&
                    values.value("Type") == "Application" && !values.value("Exec").isEmpty();
    const auto only = values.value("OnlyShowIn").split(';', Qt::SkipEmptyParts);
    const auto notIn = values.value("NotShowIn").split(';', Qt::SkipEmptyParts);
    if (!only.isEmpty()) {
        bool matched = false;
        for (const auto &desktop : environment.desktops)
            matched |= only.contains(desktop);
        state.enabled &= matched;
    }
    for (const auto &desktop : environment.desktops)
        state.enabled &= !notIn.contains(desktop);
    const auto tryExec = unescapeString(values.value("TryExec"));
    if (!tryExec.isEmpty())
        state.enabled &= !QStandardPaths::findExecutable(tryExec).isEmpty();
    state.detail =
        state.enabled ? UiText::tr("Registered to start in the tray at login. Your desktop session "
                                   "must process XDG autostart entries.")
                      : UiText::tr("Off. Ranslator will start only when you open it.");
    return state;
}
QString Autostart::writeNative(bool enabled) {
    QString error;
    const auto command = enabled ? launcher(environment, &error) : QString();
    if (!error.isEmpty())
        return error;
    QString entry = "[Desktop Entry]\nType=Application\nName=Ranslator\n"
                    "Comment=Translate selected text\nTerminal=false\n";
    if (enabled)
        entry += "Exec=" + desktopExec(command) +
                 " --background\nTryExec=" + desktopString(command) + "\nHidden=false\n";
    else
        entry += "Hidden=true\n"; // Also overrides a system-wide entry with the same name.
    return writeFile(entryPath(), entry.toUtf8());
}
void Autostart::refresh() {
    if (pending)
        return;
    if (!allowed) {
        current.supported = false;
        current.detail = UiText::tr("Autostart is disabled in previews and test sessions.");
    } else if (environment.flatpak) {
        current = {};
        current.known = false;
        QFile confirmation(confirmationPath());
        if (confirmation.open(QIODevice::ReadOnly)) {
            const auto saved = QJsonDocument::fromJson(confirmation.readAll()).object();
            if (saved["enabled"].isBool()) {
                current.enabled = saved["enabled"].toBool();
                current.detail = current.enabled
                                     ? UiText::tr("Last confirmed by the desktop: on. ")
                                     : UiText::tr("Last confirmed by the desktop: off. ");
            }
        }
        if (current.detail.isEmpty())
            current.detail = UiText::tr("Not yet confirmed by the desktop. ");
        current.detail +=
            UiText::tr("The portal cannot report changes made elsewhere. This change applies "
                       "immediately and may ask for permission.");
    } else {
#ifdef Q_OS_MACOS
        current = NativeMac::autostartState();
#elif defined(Q_OS_LINUX)
        current = readNative();
#else
        current = {};
        current.supported = false;
        current.detail = UiText::tr("Autostart is not available on this platform.");
#endif
    }
    emit changed();
}
void Autostart::finish(AutostartState state, const QString &error) {
    pending = false;
    current = std::move(state);
    emit changed();
    if (!error.isEmpty())
        emit failed(error);
}
void Autostart::setEnabled(bool enabled) {
    if (pending || !allowed || !current.supported)
        return;
    const auto previous = current;
    pending = true;
    emit changed();
    if (environment.flatpak) {
        portal.setAutostart(enabled, [this, previous, enabled](bool confirmed,
                                                               const QString &error) {
            if (!error.isEmpty()) {
                finish(previous, error);
                return;
            }
            const auto saveError = writeFile(
                confirmationPath(),
                QJsonDocument(QJsonObject{{"enabled", confirmed}}).toJson(QJsonDocument::Compact));
            pending = false;
            refresh();
            current.enabled = confirmed;
            current.detail = confirmed ? UiText::tr("Desktop confirmed autostart on. Changes made "
                                                    "elsewhere cannot be checked by this portal.")
                                       : UiText::tr("Desktop confirmed autostart off. Changes made "
                                                    "elsewhere cannot be checked by this portal.");
            emit changed();
            if (confirmed != enabled)
                emit failed(
                    UiText::tr("The desktop did not approve the requested autostart change. "
                               "Review the app's background permissions in system settings."));
            if (!saveError.isEmpty())
                emit failed(UiText::tr("Autostart changed, but its last-confirmed state "
                                       "could not be saved: %1")
                                .arg(saveError));
        });
    } else {
#ifdef Q_OS_MACOS
        NativeMac::setAutostart(this, enabled, [this](AutostartState state, QString error) {
            finish(std::move(state), error);
        });
#else
        QTimer::singleShot(0, this, [this, enabled] {
            const auto error = writeNative(enabled);
            finish(readNative(), error);
        });
#endif
    }
}
void Autostart::openSystemSettings() {
#ifdef Q_OS_MACOS
    NativeMac::openLoginItemsSettings();
#endif
}
