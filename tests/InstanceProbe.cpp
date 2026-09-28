#include "SingleInstance.h"
#include <QCoreApplication>
#include <QTimer>
#include <cstdio>
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc != 2)
        return 2;
    pettime::SingleInstance instance;
    QString error;
    const auto result = instance.start(QString::fromLocal8Bit(argv[1]), "test", &error);
    if (result == pettime::SingleInstance::Result::Error) {
        std::fprintf(stderr, "%s\n", error.toUtf8().constData());
        return 1;
    }
    if (result == pettime::SingleInstance::Result::Recalled) {
        std::puts("RECALLED");
        return 0;
    }
    std::puts("READY");
    std::fflush(stdout);
    QTimer::singleShot(15000, &app, &QCoreApplication::quit);
    return app.exec();
}
