
#include "drdm-probe-app.h"
#include "drdm-traffic-app.h"

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("DrdmScenario");

int
main(int argc, char* argv[])
{
  double failureTime = 4.0;
  double stopTime = 12.0;
  double probeIntervalMs = 100.0;
  double probeTimeoutMs = 60.0;
  uint32_t threshold = 3;
  bool injectFailure = true;

  CommandLine cmd(__FILE__);
  cmd.AddValue("failureTime", "Time at which the primary link fails (seconds)", failureTime);
  cmd.AddValue("stopTime", "Simulation stop time (seconds)", stopTime);
  cmd.AddValue("probeIntervalMs", "Probe interval in milliseconds", probeIntervalMs);
  cmd.AddValue("probeTimeoutMs", "Probe timeout in milliseconds", probeTimeoutMs);
  cmd.AddValue("threshold", "Consecutive missed probes before failover", threshold);
  cmd.AddValue("injectFailure", "Whether to inject the primary-link failure", injectFailure);
  cmd.Parse(argc, argv);

  NodeContainer source, primaryRouter, backupRouter, destination;
  source.Create(1);
  primaryRouter.Create(1);
  backupRouter.Create(1);
  destination.Create(1);

  PointToPointHelper p2p;
  p2p.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
  p2p.SetChannelAttribute("Delay", StringValue("2ms"));

  NetDeviceContainer sourcePrimary =
      p2p.Install(source.Get(0), primaryRouter.Get(0));
  NetDeviceContainer primaryDestination =
      p2p.Install(primaryRouter.Get(0), destination.Get(0));
  NetDeviceContainer sourceBackup =
      p2p.Install(source.Get(0), backupRouter.Get(0));
  NetDeviceContainer backupDestination =
      p2p.Install(backupRouter.Get(0), destination.Get(0));

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

  // Add a host route to the backup destination through the backup router.
  Ptr<Ipv4> sourceIpv4 = source.Get(0)->GetObject<Ipv4>();

  int32_t backupOutIf =
      sourceIpv4->GetInterfaceForDevice(sourceBackup.Get(0));

  NS_ABORT_MSG_IF(backupOutIf < 0, "Backup interface not found");

  Ipv4StaticRoutingHelper staticRoutingHelper;
  Ptr<Ipv4StaticRouting> sourceStaticRouting =
      staticRoutingHelper.GetStaticRouting(sourceIpv4);

  sourceStaticRouting->AddHostRouteTo(
      backupIf.GetAddress(1),
      sourceBackupIf.GetAddress(1),
      static_cast<uint32_t>(backupOutIf));

  // Traffic receiver and probe echo server bind to all destination interfaces.
  PacketSinkHelper sinkHelper(
      "ns3::UdpSocketFactory",
      InetSocketAddress(Ipv4Address::GetAny(), 9000));

  ApplicationContainer sink = sinkHelper.Install(destination.Get(0));
  sink.Start(Seconds(0.5));
  sink.Stop(Seconds(stopTime));

  UdpEchoServerHelper echo(9001);
  ApplicationContainer echoServer = echo.Install(destination.Get(0));
  echoServer.Start(Seconds(0.5));
  echoServer.Stop(Seconds(stopTime));

  Ptr<DrdmTrafficApp> traffic = CreateObject<DrdmTrafficApp>();

  traffic->Configure(
      InetSocketAddress(primaryIf.GetAddress(1), 9000),
      InetSocketAddress(backupIf.GetAddress(1), 9000),
      9000,
      MilliSeconds(20));

  source.Get(0)->AddApplication(traffic);
  traffic->SetStartTime(Seconds(1.0));
  traffic->SetStopTime(Seconds(stopTime));

  Ptr<DrdmProbeApp> detector = CreateObject<DrdmProbeApp>();

  detector->Configure(
      InetSocketAddress(primaryIf.GetAddress(1), 9001),
      9001,
      MilliSeconds(probeIntervalMs),
      MilliSeconds(probeTimeoutMs),
      threshold);

  detector->SetFailureCallback(
      MakeCallback(&DrdmTrafficApp::Failover, traffic));

  source.Get(0)->AddApplication(detector);
  detector->SetStartTime(Seconds(1.0));
  detector->SetStopTime(Seconds(stopTime));

  if (injectFailure)
    {
      Simulator::Schedule(
          Seconds(failureTime),
          [primaryRouter, destination, primaryDestination]() {
            Ptr<Ipv4> routerIpv4 =
                primaryRouter.Get(0)->GetObject<Ipv4>();
            Ptr<Ipv4> destIpv4 =
                destination.Get(0)->GetObject<Ipv4>();

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
