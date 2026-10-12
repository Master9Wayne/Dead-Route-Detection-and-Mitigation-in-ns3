#include "drdm-probe-app.h"

#include "ns3/log.h"
#include "ns3/inet-socket-address.h"
#include "ns3/udp-socket-factory.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("DrdmProbeApp");
NS_OBJECT_ENSURE_REGISTERED(DrdmProbeApp);

TypeId
DrdmProbeApp::GetTypeId()
{
  static TypeId tid = TypeId("ns3::DrdmProbeApp")
    .SetParent<Application>()
    .SetGroupName("Applications")
    .AddConstructor<DrdmProbeApp>();
  return tid;
}

void
DrdmProbeApp::Configure(Address target, uint16_t port, Time interval, Time timeout,
                        uint32_t missThreshold)
{
  m_target = target;
  m_port = port;
  m_interval = interval;
  m_timeout = timeout;
  m_missThreshold = missThreshold;
}

void
DrdmProbeApp::SetFailureCallback(Callback<void> callback)
{
  m_failureCallback = callback;
}

void
DrdmProbeApp::StartApplication()
{
  m_running = true;
  m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
  m_socket->Bind();
  m_socket->SetRecvCallback(MakeCallback(&DrdmProbeApp::HandleRead, this));
  m_sendEvent = Simulator::Schedule(Seconds(0), &DrdmProbeApp::SendProbe, this);
}

void
DrdmProbeApp::StopApplication()
{
  m_running = false;
  Simulator::Cancel(m_sendEvent);
  Simulator::Cancel(m_timeoutEvent);
  if (m_socket)
    {
      m_socket->Close();
      m_socket = nullptr;
    }
}

void
DrdmProbeApp::SendProbe()
{
  if (!m_running || m_failureReported)
    return;

  ++m_sequence;
  m_repliedSequence = 0;

  Ptr<Packet> packet = Create<Packet>(
    reinterpret_cast<const uint8_t*>(&m_sequence), sizeof(m_sequence));
  m_socket->SendTo(packet, 0, InetSocketAddress(
    InetSocketAddress::ConvertFrom(m_target).GetIpv4(), m_port));

  const uint32_t sentSequence = m_sequence;
  m_timeoutEvent = Simulator::Schedule(
    m_timeout, &DrdmProbeApp::CheckTimeout, this, sentSequence);
  m_sendEvent = Simulator::Schedule(m_interval, &DrdmProbeApp::SendProbe, this);
}

void
DrdmProbeApp::CheckTimeout(uint32_t sequence)
{
  if (!m_running || m_failureReported || sequence != m_sequence)
    return;

  if (m_repliedSequence != sequence)
    {
      ++m_consecutiveMisses;
      NS_LOG_INFO("Probe " << sequence << " timed out; consecutive misses="
                           << m_consecutiveMisses);
    }
  else
    {
      m_consecutiveMisses = 0;
    }

  if (m_consecutiveMisses >= m_missThreshold)
    {
      m_failureReported = true;
      NS_LOG_UNCOND("DRDM: PRIMARY PATH DECLARED DEAD after "
                    << m_consecutiveMisses << " missed probes");
      if (!m_failureCallback.IsNull())
        m_failureCallback();
    }
}

void
DrdmProbeApp::HandleRead(Ptr<Socket> socket)
{
  Address from;
  Ptr<Packet> packet;
  while ((packet = socket->RecvFrom(from)))
    {
      if (packet->GetSize() < sizeof(uint32_t))
        continue;
      uint32_t sequence = 0;
      packet->CopyData(reinterpret_cast<uint8_t*>(&sequence), sizeof(sequence));
      if (sequence == m_sequence)
        m_repliedSequence = sequence;
    }
}

} // namespace ns3
