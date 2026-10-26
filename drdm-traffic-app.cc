#include "drdm-traffic-app.h"

#include "ns3/log.h"
#include "ns3/inet-socket-address.h"
#include "ns3/udp-socket-factory.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"

#include <algorithm>
#include <limits>

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
DrdmTrafficApp::Configure(const std::vector<CandidatePath>& paths,
                          uint16_t port,
                          Time interval,
                          double delayWeight,
                          double lossWeight,
                          double congestionWeight)
{
  NS_ABORT_MSG_IF(paths.size() < 2,
                  "At least one primary and one backup candidate are required");

  NS_ABORT_MSG_IF(delayWeight < 0.0 ||
                  lossWeight < 0.0 ||
                  congestionWeight < 0.0,
                  "Scoring weights cannot be negative");

  double weightSum = delayWeight + lossWeight + congestionWeight;

  NS_ABORT_MSG_IF(weightSum <= 0.0,
                  "At least one scoring weight must be positive");

  m_paths = paths;
  m_port = port;
  m_interval = interval;

  m_delayWeight = delayWeight / weightSum;
  m_lossWeight = lossWeight / weightSum;
  m_congestionWeight = congestionWeight / weightSum;

  m_primaryIndex = 0;
  m_currentIndex = 0;

  for (uint32_t i = 0; i < m_paths.size(); ++i)
    {
      NS_ABORT_MSG_IF(m_paths[i].delayMs < 0.0,
                      "Candidate delay cannot be negative");

      NS_ABORT_MSG_IF(m_paths[i].lossRate < 0.0 ||
                      m_paths[i].lossRate > 1.0,
                      "Candidate loss rate must be in [0, 1]");

      NS_ABORT_MSG_IF(m_paths[i].congestion < 0.0 ||
                      m_paths[i].congestion > 1.0,
                      "Candidate congestion must be in [0, 1]");
    }
}

void
DrdmTrafficApp::UpdateMetrics(uint32_t index,
                              double delayMs,
                              double lossRate,
                              double congestion,
                              uint32_t alive)
{
  if (index >= m_paths.size())
    {
      return;
    }

  if (index == m_primaryIndex)
    {
      return;
    }

  if (lossRate < 0.0)
    {
      lossRate = 0.0;
    }
  if (lossRate > 1.0)
    {
      lossRate = 1.0;
    }
  if (congestion < 0.0)
    {
      congestion = 0.0;
    }
  if (congestion > 1.0)
    {
      congestion = 1.0;
    }
  if (delayMs < 0.0)
    {
      delayMs = 0.0;
    }

  m_paths[index].delayMs = delayMs;
  m_paths[index].lossRate = lossRate;
  m_paths[index].congestion = congestion;
  m_paths[index].alive = alive;
}

void
DrdmTrafficApp::StartApplication()
{
  m_running = true;

  m_socket = Socket::CreateSocket(
      GetNode(), UdpSocketFactory::GetTypeId());

  m_socket->Bind();

  m_sendEvent = Simulator::Schedule(
      Seconds(0), &DrdmTrafficApp::SendPacket, this);

  NS_LOG_UNCOND(
      "DRDM: configured " << m_paths.size()
      << " candidate paths; primary="
      << m_paths[m_primaryIndex].name);
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
  if (!m_running || m_paths.empty())
    {
      return;
    }

  Ptr<Packet> packet = Create<Packet>(512);

  m_socket->SendTo(
      packet,
      0,
      InetSocketAddress(
          InetSocketAddress::ConvertFrom(
              m_paths[m_currentIndex].destination).GetIpv4(),
          m_port));

  ++m_sent;

  if (m_sent % 25 == 0)
    {
      NS_LOG_UNCOND(
          "DRDM: sent " << m_sent << " traffic packets via "
          << m_paths[m_currentIndex].name);
    }

  m_sendEvent = Simulator::Schedule(
      m_interval, &DrdmTrafficApp::SendPacket, this);
}

double
DrdmTrafficApp::CalculateScore(const CandidatePath& path) const
{
  double maxDelay = 0.0;

  for (const auto& candidate : m_paths)
    {
      maxDelay = std::max(maxDelay, candidate.delayMs);
    }

  double normalizedDelay = 0.0;

  if (maxDelay > 0.0)
    {
      normalizedDelay = path.delayMs / maxDelay;
    }

  double score =
      m_delayWeight * normalizedDelay
      + m_lossWeight * path.lossRate
      + m_congestionWeight * path.congestion;

  return score;
}

void
DrdmTrafficApp::SelectBestBackup()
{
  double bestScore = std::numeric_limits<double>::infinity();
  uint32_t bestIndex = m_primaryIndex;
  uint32_t found = 0;

  NS_LOG_UNCOND(
      "DRDM: scoring backup candidates (lower score is better):");

  for (uint32_t i = 0; i < m_paths.size(); ++i)
    {
      if (i == m_primaryIndex)
        {
          continue;
        }

      double score = CalculateScore(m_paths[i]);

      NS_LOG_UNCOND(
          "DRDM: candidate=" << m_paths[i].name
          << " alive=" << m_paths[i].alive
          << " delay=" << m_paths[i].delayMs << " ms"
          << " loss=" << m_paths[i].lossRate * 100.0 << "%"
          << " congestion=" << m_paths[i].congestion * 100.0 << "%"
          << " score=" << score);

      if (m_paths[i].alive == 0)
        {
          continue;
        }

      if (score < bestScore)
        {
          bestScore = score;
          bestIndex = i;
          found = 1;
        }
    }

  m_usingBackup = true;

  if (found == 0)
    {
      NS_LOG_UNCOND("DRDM: no alive backup candidate available");
      return;
    }

  m_currentIndex = bestIndex;

  NS_LOG_UNCOND(
      "DRDM: selected backup candidate "
      << m_paths[m_currentIndex].name
      << " with score=" << bestScore);
}

void
DrdmTrafficApp::Failover()
{
  if (m_usingBackup || m_paths.size() < 2)
    {
      return;
    }

  SelectBestBackup();
}

} // namespace ns3