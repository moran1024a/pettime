#include "ApplicationController.h"
#include "SingleInstance.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QMessageBox>
#include <QRandomGenerator>
#include <cmath>
#include <cstdio>
#include <exception>

int main(int argc, char **argv) {
    // Export works without a desktop and must not touch a running pet or its save data.
    for (int i = 1; i < argc; ++i)
        if (QByteArray(argv[i]) == "--export" && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
            qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setOrganizationName("Pettime");
    app.setApplicationName("Pettime");
    app.setApplicationVersion("0.2.0");
    app.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser;
    parser.setApplicationDescription("Pettime · Qt 桌宠");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"export", "导出全部动作帧后退出。"});
    parser.addOption({"smoke", "运行桌面烟雾测试后退出（使用独立测试存档）。"});
    parser.addOption({"output", "导出或测试输出目录。", "directory", "preview"});
    parser.addOption({"smoke-pets", "测试中的总宠物数量，1–72。", "count", "1"});
    parser.addOption({"duration", "烟雾测试秒数，3–120。", "seconds", "6"});
    parser.addOption({"data-dir", "用户数据目录（默认使用系统用户数据位置）。", "directory"});
    parser.process(app);
    const bool smoke = parser.isSet("smoke"), exporting = parser.isSet("export");
    auto fail = [](const QString &message) {
        std::fprintf(stderr, "%s\n", message.toUtf8().constData());
        return 1;
    };
    if (smoke && exporting)
        return fail("--smoke 和 --export 不能同时使用。");
    bool countOk = false, durationOk = false;
    const int count = parser.value("smoke-pets").toInt(&countOk);
    const double duration = parser.value("duration").toDouble(&durationOk);
    if (!countOk || count < 1 || count > 72 || !durationOk || !std::isfinite(duration) ||
        duration < 3 || duration > 120)
        return fail("测试参数无效：数量应为 1–72，时长应为 3–120 秒。");
    if (!smoke && (parser.isSet("smoke-pets") || parser.isSet("duration")))
        return fail("--smoke-pets 和 --duration 需要配合 --smoke。");
    if (!exporting && QGuiApplication::platformName().startsWith("wayland")) {
        const QString message =
            "当前版本支持 X11 桌宠模式。Wayland 不允许普通窗口自由定位。请在 X11 "
            "会话运行；XWayland 可用时可尝试 QT_QPA_PLATFORM=xcb，但其兼容性未保证。";
        if (!smoke)
            QMessageBox::information(nullptr, "Pettime", message);
        return fail(message);
    }
    try {
        const QString output = QDir(parser.value("output")).absolutePath();
        pettime::SettingsStore store(smoke ? QDir(output).filePath("run-data")
                                           : parser.value("data-dir"));
        pettime::SingleInstance instance;
        if (!smoke && !exporting) {
            const QString scope = qEnvironmentVariable("XDG_SESSION_ID") + "|" +
                                  qEnvironmentVariable("DISPLAY") + "|" +
                                  qEnvironmentVariable("SESSIONNAME");
            QString error;
            const auto result = instance.start(store.directory(), scope, &error);
            if (result == pettime::SingleInstance::Result::Recalled)
                return 0;
            if (result == pettime::SingleInstance::Result::Error) {
                QMessageBox::warning(nullptr, "Pettime", error);
                return fail(error);
            }
        }
        pettime::AnimationLibrary animation;
        if (exporting) {
            QString error;
            if (!animation.exportFrames(output, &error))
                return fail(error);
            qInfo().noquote() << "Exported animations to" << output;
            return 0;
        }
        pettime::RunOptions options;
        options.smoke = smoke;
        options.population = count;
        options.duration = duration;
        options.output = output;
        options.seed = smoke ? 42 : QRandomGenerator::global()->generate();
        pettime::ApplicationController controller(animation, std::move(store), options);
        QObject::connect(&instance, &pettime::SingleInstance::recallRequested, &controller,
                         &pettime::ApplicationController::recall);
        controller.start();
        return app.exec();
    } catch (const std::exception &error) {
        if (!smoke && !exporting)
            QMessageBox::critical(nullptr, "Pettime", QString::fromUtf8(error.what()));
        return fail(QString::fromUtf8(error.what()));
    }
}
