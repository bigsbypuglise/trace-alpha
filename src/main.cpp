#include <QApplication>
#include <QStyleFactory>
#include <QIcon>
#include <QStringList>
#include "app/MainWindow.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("Trace");
    app.setOrganizationName("Trace Project");
    app.setStyle(QStyleFactory::create("Fusion"));
    QIcon appIcon;
    appIcon.addFile(QStringLiteral(":/icons/trace-16.png"));
    appIcon.addFile(QStringLiteral(":/icons/trace-32.png"));
    appIcon.addFile(QStringLiteral(":/icons/trace-48.png"));
    appIcon.addFile(QStringLiteral(":/icons/trace-256.png"));
    app.setWindowIcon(appIcon);

    QPalette p = app.palette();
    p.setColor(QPalette::Window, QColor(20, 20, 20));
    p.setColor(QPalette::WindowText, QColor(230, 230, 230));
    p.setColor(QPalette::Base, QColor(10, 10, 10));
    p.setColor(QPalette::Text, QColor(230, 230, 230));
    p.setColor(QPalette::Button, QColor(35, 35, 35));
    p.setColor(QPalette::ButtonText, QColor(230, 230, 230));
    app.setPalette(p);

    trace::app::MainWindow win;
    win.setWindowIcon(appIcon);
    win.resize(1280, 760);
    win.show();

    // Optional media path: Trace.exe "D:\Media\Clip.mov". Applied after show()
    // so the window exists to display it. Anything unusable is ignored, so a
    // bad argument still leaves a normally running app.
    const QStringList args = app.arguments();
    if (args.size() > 1) {
        win.openMediaPath(args.at(1));
    }

    return app.exec();
}
