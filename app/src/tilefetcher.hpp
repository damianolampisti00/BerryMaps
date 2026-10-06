#ifndef TILEFETCHER_HPP_
#define TILEFETCHER_HPP_

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QList>
#include <QSet>
#include <QString>
#include <QByteArray>

// Keep-alive HTTPS pool for one tile host (PROGETTO.md §4.1, §17.1).
// BerryProbe measured 400-650 ms per tile when every request opens a new
// TCP+TLS connection; here a few worker threads each keep one TLS connection
// open and send requests back to back on it.
//
// Jobs carry a priority (0 = visible) and a key; setWanted() drops queued jobs
// whose tile has scrolled away before they are ever sent. The request path
// contains the session and API key: it is never logged.

struct TileJob
{
    QString key;        // "z/x/y"
    QByteArray path;    // e.g. "/v1/2dtiles/z/x/y?session=...&key=..." (never logged)
    QByteArray etag;    // non-empty -> conditional request (If-None-Match)
    int priority;       // lower is more urgent
};

class TileFetcher;

class TileConnection : public QThread
{
    Q_OBJECT
public:
    TileConnection(TileFetcher *owner, int index, const QByteArray &host)
        : m_owner(owner), m_index(index), m_host(host) {}
protected:
    virtual void run();
private:
    TileFetcher *m_owner;
    int m_index;
    QByteArray m_host;
};

class TileFetcher : public QObject
{
    Q_OBJECT
public:
    TileFetcher(const QByteArray &host, int connections, QObject *parent = 0);
    virtual ~TileFetcher();

    void enqueue(const TileJob &job);           // replaces a queued job with the same key
    void setWanted(const QSet<QString> &keys);  // queued jobs not in here are dropped

    // Worker side (called from TileConnection threads).
    bool takeJob(TileJob *job, int waitMs);
    bool stopping();
    void deliver(const QString &key, int status, const QByteArray &body,
                 const QByteArray &etag, int maxAge, const QString &error);

signals:
    // status 0 = network/TLS failure (error says why); 200 body = image; 304 = unchanged.
    void finished(const QString &key, int status, const QByteArray &body,
                  const QByteArray &etag, int maxAge, const QString &error);

private:
    QMutex m_mutex;
    QWaitCondition m_cond;
    QList<TileJob> m_queue;
    QSet<QString> m_wanted;
    bool m_stopping;
    QList<TileConnection *> m_workers;
};

#endif /* TILEFETCHER_HPP_ */
