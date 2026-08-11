#ifndef CAMERASTREAMSERVER_H
#define CAMERASTREAMSERVER_H

#include <QObject>
#include <QHash>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>

// Receives the length-prefixed H.264 access units emitted by CameraHandle and
// relays them to local SDK clients.  This deliberately does not use the pose
// TCP connection or the gRPC feedback writer.
class CameraStreamServer final : public QObject
{
    Q_OBJECT
public:
    explicit CameraStreamServer(QObject *parent = nullptr);
    bool start(quint16 devicePort = 63902, quint16 sdkPort = 60062);
    void stop();

private slots:
    void acceptDeviceConnection();
    void acceptSdkConnection();
    void readDeviceData();

private:
    void removeDevice(QTcpSocket *socket);
    void relayFrame(const QByteArray &h264);

    QTcpServer m_deviceServer;
    QTcpServer m_sdkServer;
    QHash<QTcpSocket *, QByteArray> m_deviceBuffers;
    QHash<QTcpSocket *, quint32> m_pendingFrameSizes;
    QSet<QTcpSocket *> m_sdkClients;
    quint64 m_sequence = 0;
};

#endif // CAMERASTREAMSERVER_H
