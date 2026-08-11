#include "app/Settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#include <cstdio>

namespace trace::app {

namespace {

struct Home {
    QString path;
    bool portable = false;
};

bool logging() {
    return qgetenv("TRACE_SETTINGS_LOG") == "1";
}

Home resolveHome() {
    // The harness knob. Phase 11's checks need a settings file they can put a
    // known list into and delete afterwards, and pointing them at the real
    // per-user file would mean a measurement that edits the machine it runs on.
    // Diagnostic only; a default launch never sets it.
    const QByteArray forced = qgetenv("TRACE_SETTINGS_FILE");
    if (!forced.isEmpty()) {
        Home home;
        home.path = QDir::toNativeSeparators(QString::fromLocal8Bit(forced));
        home.portable = false;
        QDir().mkpath(QFileInfo(home.path).absolutePath());
        if (logging()) {
            fprintf(stderr, "[settings] forced by TRACE_SETTINGS_FILE: %s\n",
                    home.path.toLocal8Bit().constData());
            fflush(stderr);
        }
        return home;
    }

    // Portable mode: a trace.ini the user placed beside the executable.
    // EXISTENCE is the opt-in and Trace never creates this file.
    const QString beside =
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("trace.ini"));
    const QFileInfo besideInfo(beside);
    if (besideInfo.exists()) {
        if (besideInfo.isWritable()) {
            Home home;
            home.path = QDir::toNativeSeparators(beside);
            home.portable = true;
            if (logging()) {
                fprintf(stderr, "[settings] portable: %s\n",
                        home.path.toLocal8Bit().constData());
                fflush(stderr);
            }
            return home;
        }
        // Read-only portable file: fall through rather than accept a settings
        // home that silently discards every write. Announced, because a user
        // who created that file asked for portable mode and is entitled to know
        // they did not get it.
        fprintf(stderr,
                "Trace: %s is not writable; using the per-user settings file instead.\n",
                beside.toLocal8Bit().constData());
        fflush(stderr);
    }

    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (dir.isEmpty()) {
        // QStandardPaths can return empty in a stripped environment. Home is
        // the last resort that still is not the registry.
        dir = QDir(QDir::homePath()).filePath(QStringLiteral(".trace"));
    }
    QDir().mkpath(dir);

    Home home;
    home.path = QDir::toNativeSeparators(QDir(dir).filePath(QStringLiteral("trace.ini")));
    home.portable = false;
    if (logging()) {
        fprintf(stderr, "[settings] appconfig: %s\n", home.path.toLocal8Bit().constData());
        fflush(stderr);
    }
    return home;
}

const Home& home() {
    static const Home resolved = resolveHome();
    return resolved;
}

} // namespace

QSettings& settings() {
    // Deliberately leaked. A function-local static QSettings is destroyed
    // during static teardown, after QCoreApplication is gone, and its
    // destructor writes to disk; every caller syncs explicitly instead (see
    // syncSettings), so there is nothing left for that destructor to do.
    static QSettings* instance = new QSettings(home().path, QSettings::IniFormat);
    return *instance;
}

void syncSettings() {
    settings().sync();
}

const char* settingsModeName() {
    return home().portable ? "portable" : "appconfig";
}

QString settingsFilePath() {
    return home().path;
}

} // namespace trace::app
