#include "player.hpp"

#include <bb/cascades/AbstractPane>
#include <bb/cascades/Application>
#include <bb/cascades/QmlDocument>

#include <QDebug>
#include <QLocale>
#include <QTranslator>

#include <malloc.h>

using namespace bb::cascades;

Q_DECL_EXPORT int main(int argc, char **argv)
{
    // QNX's malloc serves a block bigger than an arena (32 KB) with its own
    // mmap and gives it back with munmap on free. Qt's socket read buffers
    // (32 KB blocks, ~64 KB once QByteArray rounds them up) are freed whenever
    // they run empty, i.e. after every packet: ~200 mmap + munmap a second
    // while playing, each with fresh zeroed pages, ~320 us of wall time per
    // round and ~20% of a core on the network thread (kernel trace,
    // 2026-10-24). With 256 KB arenas those blocks are reused from an arena.
    mallopt(MALLOC_ARENA_SIZE, 256 * 1024);

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
