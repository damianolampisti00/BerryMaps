APP_NAME = BerryMaps

# BerryMaps, Fase 1: map engine (see ../PROGETTO.md). CARTO/OpenStreetMap tiles
# over a keep-alive TLS pool, disk cache with ETag revalidation, pan/fling/pinch.

CONFIG += qt warn_on cascades10 mobility
MOBILITY += sensors
QT += network
LIBS += -lQtGui -lbb -lbbdata -lbbdevice -lbbsystem -lQtLocationSubset

INCLUDEPATH += src src/tls

SOURCES += src/main.cpp \
           src/tilefetcher.cpp \
           src/tilestore.cpp \
           src/mapcontroller.cpp src/locationservice.cpp src/placesclient.cpp src/routeclient.cpp src/navigator.cpp \
           src/tls/tlsnetworkaccessmanager.cpp src/tls/tlsnetworkreply.cpp

HEADERS += src/geo.hpp \
           src/tilefetcher.hpp \
           src/tilestore.hpp \
           src/mapcontroller.hpp src/locationservice.hpp src/placesclient.hpp src/routeclient.hpp src/navigator.hpp src/polyline.hpp \
           src/tls/tlsnetworkaccessmanager.hpp src/tls/tlsnetworkreply.hpp \
           src/tls/bbportlog.hpp

# Native TLS 1.2 (mbedTLS) vendored in BBport, as in MiniBrowser/BerryProbe.
MBEDTLS_DIR = $$quote($$_PRO_FILE_PWD_/../../BBport/third_party/mbedtls)
device {
    DEFINES += BBPORT_HAVE_NATIVE_TLS
    INCLUDEPATH += $$quote($$MBEDTLS_DIR/include)
    LIBS += $$quote($$MBEDTLS_DIR/lib/armv7/libmbedtls_all.a)
}

OTHER_FILES += assets/main.qml assets/cacert.pem bar-descriptor.xml
