#include "probe.hpp"
#include "bbportlog.hpp"

#include <bb/cascades/Application>
#include <bb/cascades/QmlDocument>
#include <bb/cascades/AbstractPane>
#include <QtDeclarative/QDeclarativeError>

using namespace bb::cascades;

Q_DECL_EXPORT int main(int argc, char **argv)
{
    Application app(argc, argv);

    Probe probe;

    QmlDocument *qml = QmlDocument::create("asset:///main.qml").parent(&probe);
    qml->setContextProperty("probe", &probe);

    // A QML load failure gives no message anywhere else (the app just shows a
    // black screen), so the parser's own errors go to the log.
    if (qml->hasErrors()) {
        QList<QDeclarativeError> errs = qml->errors();
        for (int i = 0; i < errs.size(); ++i)
            bbportLog("[BerryProbe] QML error: " + errs.at(i).toString());
    }
    AbstractPane *root = qml->createRootObject<AbstractPane>();
    if (!root) {
        bbportLog("[BerryProbe] FATAL: main.qml failed to create the root object");
        if (qml->hasErrors()) {
            QList<QDeclarativeError> errs = qml->errors();
            for (int i = 0; i < errs.size(); ++i)
                bbportLog("[BerryProbe] QML error: " + errs.at(i).toString());
        }
    }
    Application::instance()->setScene(root);

    int rc = Application::exec();
    // An unexpected exit with code 0 (no crash) is otherwise invisible.
    bbportLog(QString("[BerryProbe] uscita dal ciclo eventi, codice %1").arg(rc));
    return rc;
}
