#ifndef DRDM_TRAFFIC_APP_H
#define DRDM_TRAFFIC_APP_H

#include "ns3/application.h"
#include "ns3/event-id.h"
#include "ns3/socket.h"
#include "ns3/address.h"

#include <string>
#include <vector>

namespace ns3 {

class DrdmTrafficApp : public Application
{
public:
  struct CandidatePath
  {
    std::string name;
    Address destination;
    double delayMs{0.0};
    double lossRate{0.0};
    double congestion{0.0};
    uint32_t alive{1};
  };

  static TypeId GetTypeId();

  void Configure(const std::vector<CandidatePath>& paths,
                 uint16_t port,
                 Time interval,
                 double delayWeight,
                 double lossWeight,
                 double congestionWeight);

  void UpdateMetrics(uint32_t index,
                     double delayMs,
                     double lossRate,
                     double congestion,
                     uint32_t alive);

  void Failover();

private:
  void StartApplication() override;
  void StopApplication() override;
  void SendPacket();
  void SelectBestBackup();
  double CalculateScore(const CandidatePath& path) const;

  Ptr<Socket> m_socket;
  std::vector<CandidatePath> m_paths;

  uint32_t m_primaryIndex{0};
  uint32_t m_currentIndex{0};

  uint16_t m_port{9000};
  Time m_interval{MilliSeconds(20)};

  double m_delayWeight{0.5};
  double m_lossWeight{0.3};
  double m_congestionWeight{0.2};

  bool m_running{false};
  bool m_usingBackup{false};

  uint64_t m_sent{0};
  EventId m_sendEvent;
};

} // namespace ns3

#endif