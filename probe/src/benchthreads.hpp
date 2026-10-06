#ifndef BENCHTHREADS_HPP_
#define BENCHTHREADS_HPP_

#include <QThread>
#include <QString>
#include <QStringList>

// Both benchmarks run off the UI thread and report only through message():
// connected (queued) to Probe::log, so every line lands in the log file and on
// screen in order.

// GPU capabilities and texture upload cost, on an offscreen EGL pbuffer (no
// Screen window needed just to query the driver).
class GlBenchThread : public QThread
{
    Q_OBJECT
public:
    explicit GlBenchThread(QObject *parent = 0) : QThread(parent) {}
signals:
    void message(const QString &line);
protected:
    virtual void run();
    // Written to berryprobe.log from this thread right away (thread-safe), and
    // sent to the UI through message(): a crash can't lose already-run steps.
    void say(const QString &line);
};

// Decoding time of 512x512 tiles: Qt's QImage vs QNX libimg (straight to
// RGB565 / XRGB8888), for every file given.
class DecodeBenchThread : public QThread
{
    Q_OBJECT
public:
    DecodeBenchThread(const QStringList &files, QObject *parent = 0)
        : QThread(parent), m_files(files) {}
signals:
    void message(const QString &line);
protected:
    virtual void run();
    void say(const QString &line);
private:
    QStringList m_files;
};

#endif /* BENCHTHREADS_HPP_ */
