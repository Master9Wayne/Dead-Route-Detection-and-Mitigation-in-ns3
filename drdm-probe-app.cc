#include "drdm-probe-app.h"

#include "ns3/log.h"
#include "ns3/inet-socket-address.h"
#include "ns3/udp-socket-factory.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"

#include <cmath>

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
DrdmProbeApp::Configure(Address target, uint16_t port, Time interval,
                        Time timeout, uint32_t missThreshold)
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
DrdmProbeApp::SetAdaptiveTimeout(bool enabled)
{
  m_adaptiveTimeout = enabled;
}

void
DrdmProbeApp::SetTimeoutMultiplier(double multiplier)
{
  m_timeoutMultiplier = multiplier;
}

Time
DrdmProbeApp::GetCurrentTimeout() const
{
  if (!m_adaptiveTimeout || !m_hasRttSample)
    {
      return m_timeout;
    }

  double timeoutSeconds =
      m_srtt.GetSeconds() + m_timeoutMultiplier * m_rttVar.GetSeconds();

  if (timeoutSeconds < m_minTimeout.GetSeconds())
    {
      timeoutSeconds = m_minTimeout.GetSeconds();
    }
  if (timeoutSeconds > m_maxTimeout.GetSeconds())
    {
      timeoutSeconds = m_maxTimeout.GetSeconds();
    }

  return Seconds(timeoutSeconds);
}

void
DrdmProbeApp::UpdateRttEstimate(Time sample, uint32_t sequence)
{
  const double sampleSeconds = sample.GetSeconds();

  if (!m_hasRttSample)
    {
      m_srtt = sample;
      m_rttVar = Seconds(sampleSeconds / 2.0);
      m_hasRttSample = true;
    }
  else
    {
      const double difference =
          std::abs(sampleSeconds - m_srtt.GetSeconds());

      // Update RTTVAR using the old SRTT value.
      m_rttVar = Seconds(
          0.75 * m_rttVar.GetSeconds() + 0.25 * difference);
      m_srtt = Seconds(
          0.875 * m_srtt.GetSeconds() + 0.125 * sampleSeconds);
    }

  NS_LOG_UNCOND(
      "DRDM: probe " << sequence
      << " RTT=" << sampleSeconds * 1000.0 << " ms"
      << ", SRTT=" << m_srtt.GetSeconds() * 1000.0 << " ms"
      << ", RTTVAR=" << m_rttVar.GetSeconds() * 1000.0 << " ms"
      << ", K=" << m_timeoutMultiplier
      << ", timeout=" << GetCurrentTimeout().GetSeconds() * 1000.0
      << " ms");
}

void
DrdmProbeApp::StartApplication()
{
  m_running = true;

  NS_LOG_UNCOND(
      "DRDM: timeout mode = " << (m_adaptiveTimeout ? "ADAPTIVE" : "FIXED")
      << ", initial/fixed timeout = " << m_timeout.GetMilliSeconds()
      << " ms, K = " << m_timeoutMultiplier);

  m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
  m_socket->Bind();
  m_socket->SetRecvCallback(MakeCallback(&DrdmProbeApp::HandleRead, this));
  m_sendEvent = Simulator::Schedule(
      Seconds(0), &DrdmProbeApp::SendProbe, this);
}

void
DrdmProbeApp::StopApplication()
{
  m_running = false;
  Simulator::Cancel(m_sendEvent);

  for (auto& entry : m_probes)
    {
      Simulator::Cancel(entry.second.timeoutEvent);
    }
  m_probes.clear();

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
    {
      return;
    }

  const uint32_t sequence = ++m_sequence;
  m_probes[sequence] = ProbeRecord();
  m_probes[sequence].sentAt = Simulator::Now();

  const Time timeout = GetCurrentTimeout();
  m_probes[sequence].timeoutEvent = Simulator::Schedule(
      timeout, &DrdmProbeApp::CheckTimeout, this, sequence);

  Ptr<Packet> packet = Create<Packet>(
      reinterpret_cast<const uint8_t*>(&sequence), sizeof(sequence));
  m_socket->SendTo(
      packet, 0,
      InetSocketAddress(InetSocketAddress::ConvertFrom(m_target).GetIpv4(), m_port));

  m_sendEvent = Simulator::Schedule(
      m_interval, &DrdmProbeApp::SendProbe, this);
}

void
DrdmProbeApp::CheckTimeout(uint32_t sequence)
{
  if (!m_running || m_failureReported)
    {
      return;
    }

  auto it = m_probes.find(sequence);
  if (it == m_probes.end() || it->second.completed)
    {
      return;
    }

  it->second.completed = true;
  it->second.received = false;
  ProcessCompletedProbes();
}

void
DrdmProbeApp::HandleRead(Ptr<Socket> socket)
{
  Address from;
  Ptr<Packet> packet;

  while ((packet = socket->RecvFrom(from)))
    {
      if (packet->GetSize() < sizeof(uint32_t))
        {
          continue;
        }

      uint32_t sequence = 0;
      packet->CopyData(reinterpret_cast<uint8_t*>(&sequence), sizeof(sequence));

      if (m_failureReported)
        {
          continue;
        }

      auto it = m_probes.find(sequence);
      if (it == m_probes.end() || it->second.completed)
        {
          continue;
        }

      const Time sample = Simulator::Now() - it->second.sentAt;
      it->second.completed = true;
      it->second.received = true;
      Simulator::Cancel(it->second.timeoutEvent);

      if (m_adaptiveTimeout)
        {
          UpdateRttEstimate(sample, sequence);
        }

      ProcessCompletedProbes();
    }
}

void
DrdmProbeApp::ProcessCompletedProbes()
{
  while (true)
    {
      auto it = m_probes.find(m_nextSequenceToProcess);
      if (it == m_probes.end() || !it->second.completed)
        {
          return;
        }

      const uint32_t sequence = it->first;
      const bool received = it->second.received;
      m_probes.erase(it);
      ++m_nextSequenceToProcess;

      if (received)
        {
          m_consecutiveMisses = 0;
        }
      else
        {
          ++m_consecutiveMisses;
          NS_LOG_UNCOND(
              "DRDM: probe " << sequence
              << " timed out; consecutive misses=" << m_consecutiveMisses
              << " at " << Simulator::Now().GetSeconds() << " s");
        }

      if (m_consecutiveMisses >= m_missThreshold)
        {
          m_failureReported = true;
          Simulator::Cancel(m_sendEvent);

          for (auto& entry : m_probes)
            {
              Simulator::Cancel(entry.second.timeoutEvent);
            }

          NS_LOG_UNCOND(
              "DRDM: PRIMARY PATH DECLARED DEAD at "
              << Simulator::Now().GetSeconds() << " s after "
              << m_consecutiveMisses << " missed probes");

          if (!m_failureCallback.IsNull())
            {
              m_failureCallback();
            }
          return;
        }
    }
}

} // namespace ns3
