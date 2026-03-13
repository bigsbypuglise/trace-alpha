#include <QApplication>
#include <QStyleFactory>
#include "app/MainWindow.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("Trace");
    app.setOrganizationName("Trace Project");
    app.setStyle(QStyleFactory::create("Fusion"));

    QPalette p = app.palette();
    p.setColor(QPalette::Window, QColor(20, 20, 20));
    p.setColor(QPalette::WindowText, QColor(230, 230, 230));
    p.setColor(QPalette::Base, QColor(10, 10, 10));
    p.setColor(QPalette::Text, QColor(230, 230, 230));
    p.setColor(QPalette::Button, QColor(35, 35, 35));
    p.setColor(QPalette::ButtonText, QColor(230, 230, 230));
    app.setPalette(p);

    trace::app::MainWindow win;
    win.resize(1280, 760);
    win.show();
    return app.exec();
}
