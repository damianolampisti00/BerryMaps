#ifndef INVOKEHANDLER_HPP_
#define INVOKEHANDLER_HPP_

#include <QObject>
#include <QString>

namespace bb { namespace system { class InvokeManager; class InvokeRequest; } }
class PlacesClient;

// Places sent to BerryMaps by other apps (PROGETTO.md §24). The Q5 has no
// BlackBerry Maps app any more and Contacts/Calendar address it by name, so
// BerryMaps registers for what is invoked without a fixed target:
//  - Share of an address (application/vnd.blackberry.string.address)
//  - geo: URIs (open, view, share)
//  - full Google Maps links (maps.google.*, google.*/maps)
//  - BB10 map requests (application/vnd.rim.map.action-v1)
// Text addresses are turned into coordinates with Google Geocoding.
class InvokeHandler : public QObject
{
    Q_OBJECT
public:
    InvokeHandler(PlacesClient *places, QObject *parent = 0);

signals:
    void openPlace(const QString &name, const QString &address, double lat, double lon);
    void message(const QString &text);

private slots:
    void onInvoked(const bb::system::InvokeRequest &request);

private:
    bool handleLink(const QString &link);
    void geocode(const QString &address);

    bb::system::InvokeManager *m_invoke;
    PlacesClient *m_places;
};

#endif /* INVOKEHANDLER_HPP_ */
