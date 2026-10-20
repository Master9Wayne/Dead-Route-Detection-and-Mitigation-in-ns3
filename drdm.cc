#include "drdm-probe-app.h"
#include "drdm-traffic-app.h"

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/error-model.h"
#include "ns3/random-variable-stream.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("DrdmScenario");

class DrdmProbeEchoApp : public Application
{
public:
  static TypeId GetTypeId();

  void Configure(uint16_t port, double jitterMs)
  {
    m_port = port;
    m_jitterMs = jitterMs;
  }

private:
  void StartApplication() override
  {
    m_running = true;
    m_rng = CreateObject<UniformRandomVariable>();
    m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());

    const int result = m_socket->Bind(
        InetSocketAddress(Ipv4Address::GetAny(), m_port));
    NS_ABORT_MSG_IF(result != 0, "Probe echo socket bind failed");

    m_socket->SetRecvCallback(
        MakeCallback(&DrdmProbeEchoApp::HandleRead, this));
  }

  void StopApplication() override
  {
    m_running = false;
    if (m_socket)
      {
        m_socket->Close();
        m_socket = nullptr;
      }
  }

  void HandleRead(Ptr<Socket> socket)
  {
    Address from;
    Ptr<Packet> packet;

    while ((packet = socket->RecvFrom(from)))
      {
        double extraDelayMs = 0.0;
        if (m_jitterMs > 0.0)
          {
            extraDelayMs = m_rng->GetValue(0.0, m_jitterMs);
          }

        Simulator::Schedule(
            Seconds(extraDelayMs / 1000.0),
            &DrdmProbeEchoApp::SendDelayedReply,
            this, packet, from);
      }
  }

  void SendDelayedReply(Ptr<Packet> packet, Address destination)
  {
    if (!m_running || !m_socket)
      {
        return;
      }
    m_socket->SendTo(packet, 0, destination);
  }

  Ptr<Socket> m_socket;
  Ptr<UniformRandomVariable> m_rng;
  uint16_t m_port{9001};
  double m_jitterMs{0.0};
  bool m_running{false};
};

NS_OBJECT_ENSURE_REGISTERED(DrdmProbeEchoApp);

TypeId
DrdmProbeEchoApp::GetTypeId()
{
  static TypeId tid = TypeId("ns3::DrdmProbeEchoApp")
    .SetParent<Application>()
    .SetGroupName("Applications")
    .AddConstructor<DrdmProbeEchoApp>();
  return tid;
}

int
main(int argc, char* argv[])
{
  double failureTime = 4.0;
  double stopTime = 12.0;
  double probeIntervalMs = 100.0;
  double probeTimeoutMs = 60.0;
  double linkDelayMs = 2.0;
  double jitterMs = 0.0;
  double lossRate = 0.0;
  double timeoutMultiplier = 4.0;

  uint32_t threshold = 3;
  uint32_t seed = 1;
  uint64_t runNumber = 1;
  bool injectFailure = true;
  bool adaptiveTimeout = false;

  CommandLine cmd(__FILE__);
  cmd.AddValue("failureTime", "Primary link failure time in seconds", failureTime);
  cmd.AddValue("stopTime", "Simulation stop time in seconds", stopTime);
  cmd.AddValue("probeIntervalMs", "Probe interval in milliseconds", probeIntervalMs);
  cmd.AddValue("probeTimeoutMs", "Initial/fixed timeout in milliseconds", probeTimeoutMs);
  cmd.AddValue("threshold", "Missed probes required for failure detection", threshold);
  cmd.AddValue("injectFailure", "Enable primary-link failure", injectFailure);
  cmd.AddValue("adaptiveTimeout", "Enable adaptive RTT-based timeout", adaptiveTimeout);
  cmd.AddValue("timeoutMultiplier", "K in timeout = SRTT + K * RTTVAR", timeoutMultiplier);
  cmd.AddValue("linkDelayMs", "Propagation delay per link in milliseconds", linkDelayMs);
  cmd.AddValue("jitterMs", "Maximum extra random delay for probe replies", jitterMs);
  cmd.AddValue("lossRate", "Primary-link packet-loss probability (0 to 1)", lossRate);
  cmd.AddValue("seed", "Random number generator seed", seed);
  cmd.AddValue("runNumber", "Random number generator run number", runNumber);
  cmd.Parse(argc, argv);

  NS_ABORT_MSG_IF(linkDelayMs < 0.0, "Link delay cannot be negative");
  NS_ABORT_MSG_IF(jitterMs < 0.0, "Jitter cannot be negative");
  NS_ABORT_MSG_IF(lossRate < 0.0 || lossRate > 1.0,
                  "Loss rate must be between 0 and 1");
  NS_ABORT_MSG_IF(probeIntervalMs <= 0.0, "Probe interval must be positive");
  NS_ABORT_MSG_IF(probeTimeoutMs <= 0.0, "Probe timeout must be positive");
  NS_ABORT_MSG_IF(threshold == 0, "Threshold must be positive");
  NS_ABORT_MSG_IF(timeoutMultiplier <= 0.0,
                  "Timeout multiplier must be positive");

  RngSeedManager::SetSeed(seed);
  RngSeedManager::SetRun(runNumber);

  NodeContainer source, primaryRouter, backupRouter, destination;
  source.Create(1);
  primaryRouter.Create(1);
  backupRouter.Create(1);
  destination.Create(1);

  PointToPointHelper p2p;
  p2p.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
  p2p.SetChannelAttribute("Delay", TimeValue(MilliSeconds(linkDelayMs)));

  NetDeviceContainer sourcePrimary =
      p2p.Install(source.Get(0), primaryRouter.Get(0));
  NetDeviceContainer primaryDestination =
      p2p.Install(primaryRouter.Get(0), destination.Get(0));
  NetDeviceContainer sourceBackup =
      p2p.Install(source.Get(0), backupRouter.Get(0));
  NetDeviceContainer backupDestination =
      p2p.Install(backupRouter.Get(0), destination.Get(0));

  if (lossRate > 0.0)
    {
      Ptr<RateErrorModel> forwardLoss = CreateObject<RateErrorModel>();
      forwardLoss->SetAttribute("ErrorUnit",
                                EnumValue(RateErrorModel::ERROR_UNIT_PACKET));
      forwardLoss->SetAttribute("ErrorRate", DoubleValue(lossRate));

      Ptr<RateErrorModel> reverseLoss = CreateObject<RateErrorModel>();
      reverseLoss->SetAttribute("ErrorUnit",
                                EnumValue(RateErrorModel::ERROR_UNIT_PACKET));
      reverseLoss->SetAttribute("ErrorRate", DoubleValue(lossRate));

      primaryDestination.Get(0)->SetAttribute(
          "ReceiveErrorModel", PointerValue(forwardLoss));
      primaryDestination.Get(1)->SetAttribute(
          "ReceiveErrorModel", PointerValue(reverseLoss));
    }

  InternetStackHelper internet;
  internet.Install(source);
  internet.Install(primaryRouter);
  internet.Install(backupRouter);
  internet.Install(destination);

  Ipv4AddressHelper address;
  address.SetBase("10.0.1.0", "255.255.255.0");
  address.Assign(sourcePrimary);

  address.SetBase("10.0.2.0", "255.255.255.0");
  Ipv4InterfaceContainer primaryIf = address.Assign(primaryDestination);

  address.SetBase("10.0.3.0", "255.255.255.0");
  Ipv4InterfaceContainer sourceBackupIf = address.Assign(sourceBackup);

  address.SetBase("10.0.4.0", "255.255.255.0");
  Ipv4InterfaceContainer backupIf = address.Assign(backupDestination);

  Ipv4GlobalRoutingHelper::PopulateRoutingTables();

  // Ensure source traffic to the backup IP uses the backup router.
  Ptr<Ipv4> sourceIpv4 = source.Get(0)->GetObject<Ipv4>();
  int32_t backupOutIf =
      sourceIpv4->GetInterfaceForDevice(sourceBackup.Get(0));
  NS_ABORT_MSG_IF(backupOutIf < 0, "Backup interface not found");

  Ipv4StaticRoutingHelper staticRoutingHelper;
  Ptr<Ipv4StaticRouting> sourceStaticRouting =
      staticRoutingHelper.GetStaticRouting(sourceIpv4);
  sourceStaticRouting->AddHostRouteTo(
      backupIf.GetAddress(1), sourceBackupIf.GetAddress(1),
      static_cast<uint32_t>(backupOutIf));

  PacketSinkHelper sinkHelper(
      "ns3::UdpSocketFactory",
      InetSocketAddress(Ipv4Address::GetAny(), 9000));
  ApplicationContainer sink = sinkHelper.Install(destination.Get(0));
  sink.Start(Seconds(0.5));
  sink.Stop(Seconds(stopTime));

  Ptr<DrdmProbeEchoApp> echoServer = CreateObject<DrdmProbeEchoApp>();
  echoServer->Configure(9001, jitterMs);
  destination.Get(0)->AddApplication(echoServer);
  echoServer->SetStartTime(Seconds(0.5));
  echoServer->SetStopTime(Seconds(stopTime));

  Ptr<DrdmTrafficApp> traffic = CreateObject<DrdmTrafficApp>();
  traffic->Configure(
      InetSocketAddress(primaryIf.GetAddress(1), 9000),
      InetSocketAddress(backupIf.GetAddress(1), 9000),
      9000, MilliSeconds(20));
  source.Get(0)->AddApplication(traffic);
  traffic->SetStartTime(Seconds(1.0));
  traffic->SetStopTime(Seconds(stopTime));

  Ptr<DrdmProbeApp> detector = CreateObject<DrdmProbeApp>();
  detector->Configure(
      InetSocketAddress(primaryIf.GetAddress(1), 9001), 9001,
      MilliSeconds(probeIntervalMs), MilliSeconds(probeTimeoutMs), threshold);
  detector->SetAdaptiveTimeout(adaptiveTimeout);
  detector->SetTimeoutMultiplier(timeoutMultiplier);
  detector->SetFailureCallback(MakeCallback(&DrdmTrafficApp::Failover, traffic));
  source.Get(0)->AddApplication(detector);
  detector->SetStartTime(Seconds(1.0));
  detector->SetStopTime(Seconds(stopTime));

  NS_LOG_UNCOND(
      "DRDM: jitter maximum=" << jitterMs
      << " ms, primary-link packet loss=" << lossRate * 100.0
      << "% per direction, K=" << timeoutMultiplier
      << ", seed=" << seed << ", run=" << runNumber);

  if (injectFailure)
    {
      Simulator::Schedule(Seconds(failureTime),
          [primaryRouter, destination, primaryDestination]() {
            Ptr<Ipv4> routerIpv4 = primaryRouter.Get(0)->GetObject<Ipv4>();
            Ptr<Ipv4> destIpv4 = destination.Get(0)->GetObject<Ipv4>();
            int32_t routerIf =
                routerIpv4->GetInterfaceForDevice(primaryDestination.Get(0));
            int32_t destIf =
                destIpv4->GetInterfaceForDevice(primaryDestination.Get(1));

            if (routerIf >= 0)
              {
                routerIpv4->SetDown(static_cast<uint32_t>(routerIf));
              }
            if (destIf >= 0)
              {
                destIpv4->SetDown(static_cast<uint32_t>(destIf));
              }

            NS_LOG_UNCOND(
                "DRDM: injected primary-link failure at "
                << Simulator::Now().GetSeconds() << " s");
          });
    }

  FlowMonitorHelper flowHelper;
  Ptr<FlowMonitor> monitor = flowHelper.InstallAll();
  Simulator::Stop(Seconds(stopTime));
  Simulator::Run();
  monitor->SerializeToXmlFile("drdm-flowmon.xml", true, true);
  Simulator::Destroy();
  return 0;
}
