APP_NAME = Jukeberry

CONFIG += qt warn_on cascades10
QT += network
LIBS += -lbbdata -lbbsystem -lbbmultimedia -lasound -laudio_manager

INCLUDEPATH += src

SOURCES += src/main.cpp \
           src/player.cpp \
           src/link.cpp \
           src/audiooutput.cpp \
           src/websocketclient.cpp

HEADERS += src/player.hpp \
           src/link.hpp \
           src/audiooutput.hpp \
           src/audioring.hpp \
           src/websocketclient.hpp

# libopus 1.x, prebuilt for armv7 (third_party/opus, BSD licence): decodes
# the gateway's 20 ms Opus frames.
OPUS_DIR = $$quote($$_PRO_FILE_PWD_/../third_party/opus)
INCLUDEPATH += $$quote($$OPUS_DIR/include/opus)
device {
    LIBS += $$quote($$OPUS_DIR/lib/armv7/libopus.a)
}

OTHER_FILES += assets/main.qml bar-descriptor.xml
