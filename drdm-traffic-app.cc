#include "drdm-traffic-app.h"

#include "ns3/log.h"
#include "ns3/inet-socket-address.h"
#include "ns3/udp-socket-factory.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("DrdmTrafficApp");
NS_OBJECT_ENSURE_REGISTERED(DrdmTrafficApp);

TypeId
DrdmTrafficApp::GetTypeId()
{
  static TypeId tid = TypeId("ns3::DrdmTrafficApp")
    .SetParent<Application>()
    .SetGroupName("Applications")
    .AddConstructor<DrdmTrafficApp>();
  return tid;
}

void
DrdmTrafficApp::Configure(Address primary, Address backup, uint16_t port, Time interval)
{
  m_primary = primary;
  m_backup = backup;
  m_currentDestination = primary;
  m_port = port;
  m_interval = interval;
}

void
DrdmTrafficApp::StartApplication()
{
  m_running = true;
  m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
  m_socket->Bind();
  m_sendEvent = Simulator::Schedule(Seconds(0), &DrdmTrafficApp::SendPacket, this);
}

void
DrdmTrafficApp::StopApplication()
{
  m_running = false;
  Simulator::Cancel(m_sendEvent);
  if (m_socket)
    {
      m_socket->Close();
      m_socket = nullptr;
    }
}

void
DrdmTrafficApp::SendPacket()
{
  if (!m_running)
    return;

  Ptr<Packet> packet = Create<Packet>(512);
  m_socket->SendTo(packet, 0, InetSocketAddress(
    InetSocketAddress::ConvertFrom(m_currentDestination).GetIpv4(), m_port));
  ++m_sent;

  if (m_sent % 25 == 0)
    {
      NS_LOG_UNCOND("DRDM: sent " << m_sent << " traffic packets via "
                    << (m_usingBackup ? "BACKUP" : "PRIMARY") << " destination");
    }
  m_sendEvent = Simulator::Schedule(m_interval, &DrdmTrafficApp::SendPacket, this);
}

void
DrdmTrafficApp::Failover()
{
  if (m_usingBackup)
    return;

  m_currentDestination = m_backup;
  m_usingBackup = true;
  NS_LOG_UNCOND("DRDM: switched traffic to backup path");
}

} // namespace ns3
