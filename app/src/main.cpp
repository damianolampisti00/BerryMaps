#include "mapcontroller.hpp"
#include "placesclient.hpp"
#include "routeclient.hpp"
#include "navigator.hpp"
#include "voiceguide.hpp"
#include "bbportlog.hpp"

#include <bb/cascades/Application>
#include <bb/cascades/QmlDocument>
#include <bb/cascades/AbstractPane>
#include <QtDeclarative/QDeclarativeError>

using namespace bb::cascades;

static void logQmlErrors(QmlDocument *qml)
{
    if (!qml->hasErrors()) return;
    QList<QDeclarativeError> errs = qml->errors();
    for (int i = 0; i < errs.size(); ++i)
        bbportLog("[BerryMaps] QML error: " + errs.at(i).toString());
}

Q_DECL_EXPORT int main(int argc, char **argv)
{
    Application app(argc, argv);
    QCoreApplication::setOrganizationName("BerryMaps");
    QCoreApplication::setApplicationName("BerryMaps");
    bbportLog("===== BerryMaps avviato =====");

    MapController map;
    PlacesClient places;
    RouteClient routing;
    Navigator nav(&routing, map.locationService(), &map);
    VoiceGuide voice;
    nav.setVoice(&voice);

    QmlDocument *qml = QmlDocument::create("asset:///main.qml").parent(&map);
    qml->setContextProperty("map", &map);
    qml->setContextProperty("places", &places);
    qml->setContextProperty("routing", &routing);
    qml->setContextProperty("nav", &nav);
    qml->setContextProperty("voice", &voice);
    logQmlErrors(qml);
    AbstractPane *root = qml->createRootObject<AbstractPane>();
    if (!root) {
        // Otherwise just a black screen with no trace anywhere.
        bbportLog("[BerryMaps] FATAL: main.qml failed to create the root object");
        logQmlErrors(qml);
    }
    Application::instance()->setScene(root);

    int rc = Application::exec();
    map.saveState();
    bbportLog(QString("[BerryMaps] uscita, codice %1").arg(rc));
    return rc;
}
