#ifdef NETPLAY

#include "StreamViewerProtocol.hpp"

#include <QtEndian>

using namespace StreamViewer;

QByteArray StreamViewer::encode(Message type, const QByteArray& fields)
{
    const quint32 length = static_cast<quint32>(fields.size()) + 1;
    QByteArray out;
    out.reserve(static_cast<qsizetype>(length) + 4);
    const quint32 lengthLE = qToLittleEndian(length);
    out.append(reinterpret_cast<const char*>(&lengthLE), 4);
    out.append(static_cast<char>(type));
    out.append(fields);
    return out;
}

bool Reader::next(Message& type, QByteArray& fields)
{
    if (m_failed || m_buffer.size() < 4)
        return false;
    const quint32 length = qFromLittleEndian<quint32>(m_buffer.constData());
    if (length == 0 || length > kMaxMessageBytes)
    {
        m_failed = true;
        m_buffer.clear();
        return false;
    }
    if (static_cast<quint64>(m_buffer.size()) < 4ull + length)
        return false;
    type = static_cast<Message>(static_cast<quint8>(m_buffer.at(4)));
    fields = m_buffer.mid(5, static_cast<qsizetype>(length) - 1);
    m_buffer.remove(0, static_cast<qsizetype>(length) + 4);
    return true;
}

#endif // NETPLAY
