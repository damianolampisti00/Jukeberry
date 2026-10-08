#include "player.hpp"

#include <bb/cascades/AbstractPane>
#include <bb/cascades/Application>
#include <bb/cascades/QmlDocument>

#include <QDebug>
#include <QLocale>
#include <QTranslator>

using namespace bb::cascades;

Q_DECL_EXPORT int main(int argc, char **argv)
{
    Application app(argc, argv);

    // English by default; Jukeberry_<language>.qm (translations/*.ts, compiled
    // by package.ps1) when the phone uses that language.
    QTranslator translator;
    if (translator.load("Jukeberry_" + QLocale().name(), "app/native/assets/translations")) {
        app.installTranslator(&translator);
    }

    Player player;

    QmlDocument *qml = QmlDocument::create("asset:///main.qml").parent(&player);
    qml->setContextProperty("player", &player);
    AbstractPane *root = qml->createRootObject<AbstractPane>();
    if (!root) qWarning() << "[Jukeberry] FATAL: main.qml failed to create the root object";
    Application::instance()->setScene(root);

    return Application::exec();
}
