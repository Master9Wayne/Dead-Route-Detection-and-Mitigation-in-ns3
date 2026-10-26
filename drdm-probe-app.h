#ifndef DRDM_PROBE_APP_H
#define DRDM_PROBE_APP_H

#include "ns3/application.h"
#include "ns3/address.h"
#include "ns3/callback.h"
#include "ns3/event-id.h"
#include "ns3/nstime.h"
#include "ns3/socket.h"

#include <map>

namespace ns3 {

class DrdmProbeApp : public Application
{
public:
  static TypeId GetTypeId();

  void Configure(Address target, uint16_t port, Time interval,
                 Time timeout, uint32_t missThreshold);
  void SetFailureCallback(Callback<void> callback);
  void SetAdaptiveTimeout(bool enabled);
  void SetTimeoutMultiplier(double multiplier);

private:
  struct ProbeRecord
  {
    Time sentAt;
    EventId timeoutEvent;
    bool completed{false};
    bool received{false};
  };

  void StartApplication() override;
  void StopApplication() override;
  void SendProbe();
  void CheckTimeout(uint32_t sequence);
  void HandleRead(Ptr<Socket> socket);
  void ProcessCompletedProbes();
  void UpdateRttEstimate(Time sample, uint32_t sequence);
  Time GetCurrentTimeout() const;

  Ptr<Socket> m_socket;
  Address m_target;
  uint16_t m_port{9001};
  Time m_interval{MilliSeconds(100)};
  Time m_timeout{MilliSeconds(60)};
  uint32_t m_missThreshold{3};
  Callback<void> m_failureCallback;

  bool m_adaptiveTimeout{false};
  double m_timeoutMultiplier{4.0};
  bool m_hasRttSample{false};
  Time m_srtt{Seconds(0)};
  Time m_rttVar{Seconds(0)};
  Time m_minTimeout{MilliSeconds(10)};
  Time m_maxTimeout{MilliSeconds(500)};

  bool m_running{false};
  bool m_failureReported{false};
  uint32_t m_sequence{0};
  uint32_t m_nextSequenceToProcess{1};
  uint32_t m_consecutiveMisses{0};
  std::map<uint32_t, ProbeRecord> m_probes;
  EventId m_sendEvent;
};

} // namespace ns3

#endif