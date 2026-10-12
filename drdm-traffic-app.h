#ifndef DRDM_TRAFFIC_APP_H
#define DRDM_TRAFFIC_APP_H

#include "ns3/application.h"
#include "ns3/event-id.h"
#include "ns3/socket.h"
#include "ns3/address.h"

namespace ns3 {

class DrdmTrafficApp : public Application
{
public:
  static TypeId GetTypeId();
  void Configure(Address primary, Address backup, uint16_t port, Time interval);
  void Failover();

private:
  void StartApplication() override;
  void StopApplication() override;
  void SendPacket();

  Ptr<Socket> m_socket;
  Address m_primary;
  Address m_backup;
  Address m_currentDestination;
  uint16_t m_port{9000};
  Time m_interval{MilliSeconds(20)};
  bool m_running{false};
  bool m_usingBackup{false};
  uint64_t m_sent{0};
  EventId m_sendEvent;
};

} // namespace ns3

#endif
