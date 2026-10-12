#ifndef DRDM_PROBE_APP_H
#define DRDM_PROBE_APP_H

#include "ns3/application.h"
#include "ns3/event-id.h"
#include "ns3/callback.h"
#include "ns3/socket.h"
#include "ns3/address.h"
#include <cstdint>

namespace ns3 {

class DrdmProbeApp : public Application
{
public:
  static TypeId GetTypeId();
  void Configure(Address target, uint16_t port, Time interval, Time timeout,
                 uint32_t missThreshold);
  void SetFailureCallback(Callback<void> callback);

private:
  void StartApplication() override;
  void StopApplication() override;
  void SendProbe();
  void CheckTimeout(uint32_t sequence);
  void HandleRead(Ptr<Socket> socket);

  Ptr<Socket> m_socket;
  Address m_target;
  uint16_t m_port{9001};
  Time m_interval{MilliSeconds(100)};
  Time m_timeout{MilliSeconds(60)};
  uint32_t m_missThreshold{3};
  uint32_t m_sequence{0};
  uint32_t m_repliedSequence{0};
  uint32_t m_consecutiveMisses{0};
  bool m_running{false};
  bool m_failureReported{false};
  EventId m_sendEvent;
  EventId m_timeoutEvent;
  Callback<void> m_failureCallback;
};

} // namespace ns3

#endif
