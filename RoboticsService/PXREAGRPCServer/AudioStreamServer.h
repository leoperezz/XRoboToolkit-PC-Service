#ifndef AUDIOSTREAMSERVER_H
#define AUDIOSTREAMSERVER_H

#include <QObject>
#include <QHash>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>

// Dedicated PCM audio relay. Audio never enters the tracking socket, camera
// socket, or gRPC feedback writer.
class AudioStreamServer final : public QObject
{
    Q_OBJECT
public:
    explicit AudioStreamServer(QObject *parent = nullptr);
    bool start(quint16 devicePort = 63903, quint16 sdkPort = 60063);
    void stop();

private slots:
    void acceptDeviceConnection();
    void acceptSdkConnection();
    void readDeviceData();

private:
    void removeDevice(QTcpSocket *socket);
    void relayPacket(const QByteArray &packet);

    QTcpServer m_deviceServer;
    QTcpServer m_sdkServer;
    QHash<QTcpSocket *, QByteArray> m_deviceBuffers;
    QSet<QTcpSocket *> m_sdkClients;
};

#endif // AUDIOSTREAMSERVER_H
