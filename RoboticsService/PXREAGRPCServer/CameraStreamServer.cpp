#include "CameraStreamServer.h"

#include <QDateTime>
#include <QDebug>
#include <QtEndian>

namespace {
constexpr quint32 kMaxCameraPacket = 8u * 1024u * 1024u;
constexpr qint64 kMaxPendingSdkBytes = 2 * 1024 * 1024;
constexpr quint16 kProtocolVersion = 1;
constexpr quint16 kCodecH264 = 1;
constexpr quint32 kDefaultWidth = 1280;
constexpr quint32 kDefaultHeight = 480;

template <typename T>
void appendBigEndian(QByteArray &out, T value)
{
    const T encoded = qToBigEndian(value);
    out.append(reinterpret_cast<const char *>(&encoded), sizeof(encoded));
}
}

CameraStreamServer::CameraStreamServer(QObject *parent) : QObject(parent)
{
    connect(&m_deviceServer, &QTcpServer::newConnection,
            this, &CameraStreamServer::acceptDeviceConnection);
    connect(&m_sdkServer, &QTcpServer::newConnection,
            this, &CameraStreamServer::acceptSdkConnection);
}

bool CameraStreamServer::start(quint16 devicePort, quint16 sdkPort)
{
    const bool deviceOk = m_deviceServer.listen(QHostAddress::AnyIPv4, devicePort);
    // Camera frames are exposed only to SDKs on the same PC.
    const bool sdkOk = m_sdkServer.listen(QHostAddress::LocalHost, sdkPort);
    if (!deviceOk || !sdkOk) {
        qWarning() << "camera stream server failed" << m_deviceServer.errorString()
                   << m_sdkServer.errorString();
        stop();
        return false;
    }
    qDebug() << "camera stream ports: device" << devicePort << "sdk" << sdkPort;
    return true;
}

void CameraStreamServer::stop()
{
    m_deviceServer.close();
    m_sdkServer.close();
    const auto devices = m_deviceBuffers.keys();
    for (QTcpSocket *socket : devices) {
        socket->disconnectFromHost();
        socket->deleteLater();
    }
    const auto clients = m_sdkClients.values();
    for (QTcpSocket *socket : clients) {
        socket->disconnectFromHost();
        socket->deleteLater();
    }
    m_deviceBuffers.clear();
    m_pendingFrameSizes.clear();
    m_sdkClients.clear();
}

void CameraStreamServer::acceptDeviceConnection()
{
    while (m_deviceServer.hasPendingConnections()) {
        QTcpSocket *socket = m_deviceServer.nextPendingConnection();
        m_deviceBuffers.insert(socket, {});
        m_pendingFrameSizes.insert(socket, 0);
        connect(socket, &QTcpSocket::readyRead, this, &CameraStreamServer::readDeviceData);
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] { removeDevice(socket); });
        qDebug() << "Pico camera connected from" << socket->peerAddress();
    }
}

void CameraStreamServer::acceptSdkConnection()
{
    while (m_sdkServer.hasPendingConnections()) {
        QTcpSocket *socket = m_sdkServer.nextPendingConnection();
        socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        m_sdkClients.insert(socket);
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            m_sdkClients.remove(socket);
            socket->deleteLater();
        });
    }
}

void CameraStreamServer::removeDevice(QTcpSocket *socket)
{
    m_deviceBuffers.remove(socket);
    m_pendingFrameSizes.remove(socket);
    socket->deleteLater();
}

void CameraStreamServer::readDeviceData()
{
    auto *socket = qobject_cast<QTcpSocket *>(sender());
    if (!socket || !m_deviceBuffers.contains(socket)) {
        return;
    }
    QByteArray &buffer = m_deviceBuffers[socket];
    buffer.append(socket->readAll());

    while (true) {
        quint32 &frameSize = m_pendingFrameSizes[socket];
        if (frameSize == 0) {
            if (buffer.size() < 4) return;
            frameSize = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(buffer.constData()));
            buffer.remove(0, 4);
            if (frameSize == 0 || frameSize > kMaxCameraPacket) {
                qWarning() << "invalid Pico camera packet size" << frameSize;
                socket->disconnectFromHost();
                return;
            }
        }
        if (buffer.size() < static_cast<int>(frameSize)) return;
        const QByteArray frame = buffer.left(frameSize);
        buffer.remove(0, frameSize);
        frameSize = 0;
        relayFrame(frame);
    }
}

void CameraStreamServer::relayFrame(const QByteArray &h264)
{
    QByteArray packet;
    packet.reserve(36 + h264.size());
    packet.append("XRCF", 4);
    appendBigEndian(packet, kProtocolVersion);
    appendBigEndian(packet, kCodecH264);
    appendBigEndian(packet, kDefaultWidth);
    appendBigEndian(packet, kDefaultHeight);
    appendBigEndian(packet, static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()) * 1000000ULL);
    appendBigEndian(packet, ++m_sequence);
    appendBigEndian(packet, static_cast<quint32>(h264.size()));
    packet.append(h264);

    // A slow client is skipped instead of allowing backpressure or unbounded RAM.
    for (QTcpSocket *client : m_sdkClients) {
        if (client->state() == QAbstractSocket::ConnectedState &&
            client->bytesToWrite() <= kMaxPendingSdkBytes) {
            client->write(packet);
        }
    }
}
