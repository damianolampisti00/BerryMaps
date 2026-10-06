APP_NAME = BerryProbe

# Diagnostic app for BerryMaps "Fase 0" (see ../PROGETTO.md): measures GPU, image
# decoding, Cascades ImageView memory, Google Map Tiles network behaviour, sensors
# and GPS on the real Q5, writing everything to berryprobe.log.

CONFIG += qt warn_on cascades10 mobility
MOBILITY += sensors
QT += network
LIBS += -lQtGui -lbb -lbbdata -lbbdevice -lQtLocationSubset -lEGL -lGLESv2 -limg

INCLUDEPATH += src src/tls

SOURCES += src/main.cpp \
           src/probe.cpp \
           src/benchthreads.cpp \
           src/tls/tlsnetworkaccessmanager.cpp \
           src/tls/tlsnetworkreply.cpp

HEADERS += src/probe.hpp \
           src/benchthreads.hpp \
           src/tls/tlsnetworkaccessmanager.hpp \
           src/tls/tlsnetworkreply.hpp \
           src/tls/bbportlog.hpp

# Native TLS 1.2 (mbedTLS) for https://, same static library MiniBrowser links
# (vendored in BBport). Device only.
MBEDTLS_DIR = $$quote($$_PRO_FILE_PWD_/../../BBport/third_party/mbedtls)
device {
    DEFINES += BBPORT_HAVE_NATIVE_TLS
    INCLUDEPATH += $$quote($$MBEDTLS_DIR/include)
    LIBS += $$quote($$MBEDTLS_DIR/lib/armv7/libmbedtls_all.a)
}

OTHER_FILES += assets/main.qml assets/cacert.pem bar-descriptor.xml
