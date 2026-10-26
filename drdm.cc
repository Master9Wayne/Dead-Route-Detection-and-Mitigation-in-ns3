#include <vector>
#include <cmath>

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
#include "ns3/seq-ts-header.h"

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

    m_socket = Socket::CreateSocket(
        GetNode(), UdpSocketFactory::GetTypeId());

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

// ---------------------------------------------------------------------------
// Live backup-path monitor (Step 2)
// ---------------------------------------------------------------------------

#define MON_BACKUPS 3
#define MON_BASE_PORT 9100
#define MON_PRINT_EVERY 10

static Ptr<DrdmTrafficApp> monTraffic;
static Ptr<Socket> monSock[MON_BACKUPS];
static Ipv4Address monDest[MON_BACKUPS];
static double monSrtt[MON_BACKUPS];
static double monMin[MON_BACKUPS];
static double monLoss[MON_BACKUPS];
static uint32_t monReply[MON_BACKUPS];
static uint32_t monMiss[MON_BACKUPS];
static uint32_t monSamples[MON_BACKUPS];
static uint32_t monTicks = 0;
static uint32_t monMissLimit = 3;
static double monIntervalSec = 0.1;
static double monStopSec = 12.0;

static void
MonRecv(Ptr<Socket> socket)
{
  Ptr<Packet> packet;

  while ((packet = socket->Recv()))
    {
      SeqTsHeader hdr;
      packet->RemoveHeader(hdr);

      Address local;
      socket->GetSockName(local);
      int k = InetSocketAddress::ConvertFrom(local).GetPort() - MON_BASE_PORT;

      if (k < 0 || k >= MON_BACKUPS)
        {
          continue;
        }

      double rtt = (Simulator::Now() - hdr.GetTs()).GetSeconds();

      monReply[k] = 1;
      monSamples[k]++;

      if (monSamples[k] == 1)
        {
          monSrtt[k] = rtt;
          monMin[k] = rtt;
        }
      else
        {
          monSrtt[k] = 0.875 * monSrtt[k] + 0.125 * rtt;
        }

      if (rtt < monMin[k])
        {
          monMin[k] = rtt;
        }
    }
}

static void
MonTick()
{
  if (Simulator::Now().GetSeconds() >= monStopSec)
    {
      return;
    }

  for (int k = 0; k < MON_BACKUPS; k++)
    {
      if (monTicks > 0)
        {
          if (monReply[k] == 0)
            {
              monMiss[k]++;
              monLoss[k] = 0.8 * monLoss[k] + 0.2;
            }
          else
            {
              monMiss[k] = 0;
              monLoss[k] = 0.8 * monLoss[k];
            }
        }

      uint32_t alive = 1;
      if (monMiss[k] >= monMissLimit)
        {
          alive = 0;
        }

      if (monSamples[k] > 0)
        {
          double delayMs = monSrtt[k] * 1000.0 / 2.0;
          double congestion = 0.0;
          if (monMin[k] > 0.0)
            {
              congestion = (monSrtt[k] - monMin[k]) / monMin[k];
            }
          if (congestion > 1.0)
            {
              congestion = 1.0;
            }

          monTraffic->UpdateMetrics(k + 1, delayMs, monLoss[k], congestion, alive);

          if (monTicks % MON_PRINT_EVERY == 0)
            {
              NS_LOG_UNCOND(
                  "DRDM-MON t=" << Simulator::Now().GetSeconds()
                  << " BACKUP_" << (k + 1)
                  << " delay=" << delayMs << " ms"
                  << " loss=" << monLoss[k] * 100.0 << "%"
                  << " congestion=" << congestion * 100.0 << "%"
                  << " alive=" << alive);
            }
        }

      monReply[k] = 0;

      SeqTsHeader hdr;
      hdr.SetSeq(monTicks);
      Ptr<Packet> packet = Create<Packet>(32);
      packet->AddHeader(hdr);
      monSock[k]->SendTo(packet, 0, InetSocketAddress(monDest[k], 9001));
    }

  monTicks++;
  Simulator::Schedule(Seconds(monIntervalSec), &MonTick);
}

// ---------------------------------------------------------------------------
// Network-layer recovery (Step 3)
// ---------------------------------------------------------------------------

static Ptr<Ipv4StaticRouting> recSourceRouting;
static Ipv4Address recPrimaryDest;
static Ipv4Address recGateway[MON_BACKUPS + 1];
static uint32_t recOutIf[MON_BACKUPS + 1];
static uint32_t recUpdates = 0;

static void
InstallBackupRoute(uint32_t index)
{
  uint32_t count = recSourceRouting->GetNRoutes();

  for (uint32_t r = count; r > 0; r--)
    {
      Ipv4RoutingTableEntry entry = recSourceRouting->GetRoute(r - 1);
      if (entry.IsHost() && entry.GetDest() == recPrimaryDest)
        {
          recSourceRouting->RemoveRoute(r - 1);
        }
    }

  recSourceRouting->AddHostRouteTo(recPrimaryDest, recGateway[index], recOutIf[index]);
  recUpdates++;

  NS_LOG_UNCOND(
      "DRDM: routing table updated at " << Simulator::Now().GetSeconds()
      << " s: " << recPrimaryDest << " via " << recGateway[index]
      << " (interface " << recOutIf[index] << ", candidate " << index << ")");
}

// Service-interruption measurement at the receiver.
static double rxLastTime = 0.0;
static double rxMaxGap = 0.0;
static double rxGapStart = 0.0;
static uint64_t rxPackets = 0;

static void
RxTrace(Ptr<const Packet> packet, const Address& from)
{
  double now = Simulator::Now().GetSeconds();

  if (rxPackets > 0)
    {
      double gap = now - rxLastTime;
      if (gap > rxMaxGap)
        {
          rxMaxGap = gap;
          rxGapStart = rxLastTime;
        }
    }

  rxLastTime = now;
  rxPackets++;
}

static void
KillLink(Ptr<Node> nodeA, Ptr<Node> nodeB, Ptr<NetDevice> devA, Ptr<NetDevice> devB)
{
  Ptr<Ipv4> ipA = nodeA->GetObject<Ipv4>();
  Ptr<Ipv4> ipB = nodeB->GetObject<Ipv4>();

  int32_t ifA = ipA->GetInterfaceForDevice(devA);
  int32_t ifB = ipB->GetInterfaceForDevice(devB);

  if (ifA >= 0)
    {
      ipA->SetDown(static_cast<uint32_t>(ifA));
    }
  if (ifB >= 0)
    {
      ipB->SetDown(static_cast<uint32_t>(ifB));
    }

  NS_LOG_UNCOND("DRDM: injected backup-link failure at "
                << Simulator::Now().GetSeconds() << " s");
}

static void
ApplyLinkLoss(NetDeviceContainer devs, double rate)
{
  if (rate <= 0.0)
    {
      return;
    }

  Ptr<RateErrorModel> fwd = CreateObject<RateErrorModel>();
  fwd->SetAttribute("ErrorUnit", EnumValue(RateErrorModel::ERROR_UNIT_PACKET));
  fwd->SetAttribute("ErrorRate", DoubleValue(rate));

  Ptr<RateErrorModel> rev = CreateObject<RateErrorModel>();
  rev->SetAttribute("ErrorUnit", EnumValue(RateErrorModel::ERROR_UNIT_PACKET));
  rev->SetAttribute("ErrorRate", DoubleValue(rate));

  devs.Get(0)->SetAttribute("ReceiveErrorModel", PointerValue(fwd));
  devs.Get(1)->SetAttribute("ReceiveErrorModel", PointerValue(rev));
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

  double delayWeight = 0.5;
  double lossWeight = 0.3;
  double congestionWeight = 0.2;

  double backup2DelayMs = 8.0;
  double backup2LossRate = 0.01;
  double backup2Congestion = 0.20;

  double backup3DelayMs = 12.0;
  double backup3LossRate = 0.00;
  double backup3Congestion = 0.05;

  uint32_t threshold = 3;
  uint32_t seed = 1;
  uint64_t runNumber = 1;

  bool injectFailure = true;
  bool adaptiveTimeout = false;

  // Step 2 options.
  uint32_t liveMetrics = 1;
  double monIntervalMs = 100.0;
  uint32_t monThreshold = 3;
  uint32_t bgBackup = 0;
  double bgRateMbps = 9.0;
  double bgStart = 2.0;
  double bgStop = 0.0;
  uint32_t killBackup = 0;
  double killBackupTime = 3.0;

  // Step 3 option: 1 = routing-table recovery, 0 = application-layer recovery.
  uint32_t routingRecovery = 1;

  CommandLine cmd(__FILE__);

  cmd.AddValue(
      "failureTime", "Primary link failure time in seconds", failureTime);
  cmd.AddValue(
      "stopTime", "Simulation stop time in seconds", stopTime);
  cmd.AddValue(
      "probeIntervalMs", "Probe interval in milliseconds", probeIntervalMs);
  cmd.AddValue(
      "probeTimeoutMs", "Initial/fixed timeout in milliseconds", probeTimeoutMs);
  cmd.AddValue(
      "threshold", "Missed probes required for failure detection", threshold);
  cmd.AddValue(
      "injectFailure", "Enable primary-link failure", injectFailure);
  cmd.AddValue(
      "adaptiveTimeout", "Enable adaptive RTT-based timeout", adaptiveTimeout);
  cmd.AddValue(
      "timeoutMultiplier", "K in timeout = SRTT + K * RTTVAR", timeoutMultiplier);

  cmd.AddValue(
      "delayWeight", "Weight for normalized delay in backup scoring", delayWeight);
  cmd.AddValue(
      "lossWeight", "Weight for packet loss in backup scoring", lossWeight);
  cmd.AddValue(
      "congestionWeight", "Weight for congestion in backup scoring",
      congestionWeight);

  cmd.AddValue(
      "backup2DelayMs", "Delay for backup path 2 (one-way, ms)", backup2DelayMs);
  cmd.AddValue(
      "backup2LossRate", "Loss for backup path 2 (0 to 1)", backup2LossRate);
  cmd.AddValue(
      "backup2Congestion", "Fallback congestion for backup path 2 (0 to 1)",
      backup2Congestion);

  cmd.AddValue(
      "backup3DelayMs", "Delay for backup path 3 (one-way, ms)", backup3DelayMs);
  cmd.AddValue(
      "backup3LossRate", "Loss for backup path 3 (0 to 1)", backup3LossRate);
  cmd.AddValue(
      "backup3Congestion", "Fallback congestion for backup path 3 (0 to 1)",
      backup3Congestion);

  cmd.AddValue(
      "linkDelayMs", "Propagation delay per link in milliseconds", linkDelayMs);
  cmd.AddValue(
      "jitterMs", "Maximum extra random delay for probe replies", jitterMs);
  cmd.AddValue(
      "lossRate", "Primary-link packet-loss probability (0 to 1)", lossRate);
  cmd.AddValue("seed", "Random number generator seed", seed);
  cmd.AddValue("runNumber", "Random number generator run number", runNumber);

  cmd.AddValue(
      "liveMetrics", "1 = measure backup metrics by probing, 0 = use configured values",
      liveMetrics);
  cmd.AddValue(
      "monIntervalMs", "Backup monitor probe interval in milliseconds", monIntervalMs);
  cmd.AddValue(
      "monThreshold", "Consecutive missed monitor probes before a backup is dead",
      monThreshold);
  cmd.AddValue(
      "bgBackup", "Backup index (1-3) receiving background load, 0 = none", bgBackup);
  cmd.AddValue("bgRateMbps", "Background load rate in Mbps", bgRateMbps);
  cmd.AddValue("bgStart", "Background load start time in seconds", bgStart);
  cmd.AddValue("bgStop", "Background load stop time in seconds (0 = end)", bgStop);
  cmd.AddValue(
      "killBackup", "Backup index (1-3) whose link fails, 0 = none", killBackup);
  cmd.AddValue("killBackupTime", "Time of backup link failure", killBackupTime);
  cmd.AddValue(
      "routingRecovery",
      "1 = recover by updating the routing table (same destination IP), "
      "0 = recover by switching the application destination IP",
      routingRecovery);

  cmd.Parse(argc, argv);

  NS_ABORT_MSG_IF(linkDelayMs < 0.0, "Link delay cannot be negative");
  NS_ABORT_MSG_IF(jitterMs < 0.0, "Jitter cannot be negative");
  NS_ABORT_MSG_IF(lossRate < 0.0 || lossRate > 1.0,
                  "Loss rate must be between 0 and 1");
  NS_ABORT_MSG_IF(probeIntervalMs <= 0.0,
                  "Probe interval must be positive");
  NS_ABORT_MSG_IF(probeTimeoutMs <= 0.0,
                  "Probe timeout must be positive");
  NS_ABORT_MSG_IF(threshold == 0, "Threshold must be positive");
  NS_ABORT_MSG_IF(timeoutMultiplier <= 0.0,
                  "Timeout multiplier must be positive");
  NS_ABORT_MSG_IF(monIntervalMs <= 0.0, "Monitor interval must be positive");
  NS_ABORT_MSG_IF(monThreshold == 0, "Monitor threshold must be positive");
  NS_ABORT_MSG_IF(bgBackup > 3 || killBackup > 3,
                  "Backup index must be between 0 and 3");

  NS_ABORT_MSG_IF(backup2DelayMs < 0.0 || backup3DelayMs < 0.0,
                  "Candidate delay cannot be negative");

  NS_ABORT_MSG_IF(
      backup2LossRate < 0.0 || backup2LossRate > 1.0 ||
      backup3LossRate < 0.0 || backup3LossRate > 1.0,
      "Candidate loss rates must be in [0, 1]");

  NS_ABORT_MSG_IF(
      backup2Congestion < 0.0 || backup2Congestion > 1.0 ||
      backup3Congestion < 0.0 || backup3Congestion > 1.0,
      "Candidate congestion must be in [0, 1]");

  if (bgStop <= 0.0)
    {
      bgStop = stopTime;
    }

  RngSeedManager::SetSeed(seed);
  RngSeedManager::SetRun(runNumber);

  NodeContainer source;
  NodeContainer primaryRouter;
  NodeContainer backupRouter;
  NodeContainer backupRouter2;
  NodeContainer backupRouter3;
  NodeContainer destination;

  source.Create(1);
  primaryRouter.Create(1);
  backupRouter.Create(1);
  backupRouter2.Create(1);
  backupRouter3.Create(1);
  destination.Create(1);

  PointToPointHelper p2p;
  p2p.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
  p2p.SetChannelAttribute(
      "Delay", TimeValue(MilliSeconds(linkDelayMs)));

  // Backup links 2 and 3 use the configured candidate delay when live
  // metrics are enabled (two links per path, so half the delay per link).
  double backup2LinkDelayMs = linkDelayMs;
  double backup3LinkDelayMs = linkDelayMs;

  if (liveMetrics == 1)
    {
      backup2LinkDelayMs = backup2DelayMs / 2.0;
      backup3LinkDelayMs = backup3DelayMs / 2.0;
    }

  PointToPointHelper p2pBackup2;
  p2pBackup2.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
  p2pBackup2.SetChannelAttribute(
      "Delay", TimeValue(Seconds(backup2LinkDelayMs / 1000.0)));

  PointToPointHelper p2pBackup3;
  p2pBackup3.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
  p2pBackup3.SetChannelAttribute(
      "Delay", TimeValue(Seconds(backup3LinkDelayMs / 1000.0)));

  NetDeviceContainer sourcePrimary =
      p2p.Install(source.Get(0), primaryRouter.Get(0));

  NetDeviceContainer primaryDestination =
      p2p.Install(primaryRouter.Get(0), destination.Get(0));

  NetDeviceContainer sourceBackup =
      p2p.Install(source.Get(0), backupRouter.Get(0));

  NetDeviceContainer backupDestination =
      p2p.Install(backupRouter.Get(0), destination.Get(0));

  NetDeviceContainer sourceBackup2 =
      p2pBackup2.Install(source.Get(0), backupRouter2.Get(0));

  NetDeviceContainer backupDestination2 =
      p2pBackup2.Install(backupRouter2.Get(0), destination.Get(0));

  NetDeviceContainer sourceBackup3 =
      p2pBackup3.Install(source.Get(0), backupRouter3.Get(0));

  NetDeviceContainer backupDestination3 =
      p2pBackup3.Install(backupRouter3.Get(0), destination.Get(0));

  if (lossRate > 0.0)
    {
      ApplyLinkLoss(primaryDestination, lossRate);
    }

  if (liveMetrics == 1)
    {
      ApplyLinkLoss(backupDestination2, backup2LossRate);
      ApplyLinkLoss(backupDestination3, backup3LossRate);
    }

  InternetStackHelper internet;
  internet.Install(source);
  internet.Install(primaryRouter);
  internet.Install(backupRouter);
  internet.Install(backupRouter2);
  internet.Install(backupRouter3);
  internet.Install(destination);

  Ipv4AddressHelper address;

  address.SetBase("10.0.1.0", "255.255.255.0");
  address.Assign(sourcePrimary);

  address.SetBase("10.0.2.0", "255.255.255.0");
  Ipv4InterfaceContainer primaryIf =
      address.Assign(primaryDestination);

  address.SetBase("10.0.3.0", "255.255.255.0");
  Ipv4InterfaceContainer sourceBackupIf =
      address.Assign(sourceBackup);

  address.SetBase("10.0.4.0", "255.255.255.0");
  Ipv4InterfaceContainer backupIf =
      address.Assign(backupDestination);

  address.SetBase("10.0.5.0", "255.255.255.0");
  Ipv4InterfaceContainer sourceBackupIf2 =
      address.Assign(sourceBackup2);

  address.SetBase("10.0.6.0", "255.255.255.0");
  Ipv4InterfaceContainer backupIf2 =
      address.Assign(backupDestination2);

  address.SetBase("10.0.7.0", "255.255.255.0");
  Ipv4InterfaceContainer sourceBackupIf3 =
      address.Assign(sourceBackup3);

  address.SetBase("10.0.8.0", "255.255.255.0");
  Ipv4InterfaceContainer backupIf3 =
      address.Assign(backupDestination3);

  Ipv4GlobalRoutingHelper::PopulateRoutingTables();

  Ptr<Ipv4> sourceIpv4 = source.Get(0)->GetObject<Ipv4>();

  int32_t backupOutIf =
      sourceIpv4->GetInterfaceForDevice(sourceBackup.Get(0));

  int32_t backupOutIf2 =
      sourceIpv4->GetInterfaceForDevice(sourceBackup2.Get(0));

  int32_t backupOutIf3 =
      sourceIpv4->GetInterfaceForDevice(sourceBackup3.Get(0));

  NS_ABORT_MSG_IF(
      backupOutIf < 0 || backupOutIf2 < 0 || backupOutIf3 < 0,
      "Candidate backup interface not found");

  Ipv4StaticRoutingHelper staticRoutingHelper;

  Ptr<Ipv4StaticRouting> sourceStaticRouting =
      staticRoutingHelper.GetStaticRouting(sourceIpv4);

  sourceStaticRouting->AddHostRouteTo(
      backupIf.GetAddress(1),
      sourceBackupIf.GetAddress(1),
      static_cast<uint32_t>(backupOutIf));

  sourceStaticRouting->AddHostRouteTo(
      backupIf2.GetAddress(1),
      sourceBackupIf2.GetAddress(1),
      static_cast<uint32_t>(backupOutIf2));

  sourceStaticRouting->AddHostRouteTo(
      backupIf3.GetAddress(1),
      sourceBackupIf3.GetAddress(1),
      static_cast<uint32_t>(backupOutIf3));

  // Pin the reply path of each backup probe to its own backup router.
  Ptr<Ipv4> destIpv4Main = destination.Get(0)->GetObject<Ipv4>();

  int32_t destOutIf1 =
      destIpv4Main->GetInterfaceForDevice(backupDestination.Get(1));
  int32_t destOutIf2 =
      destIpv4Main->GetInterfaceForDevice(backupDestination2.Get(1));
  int32_t destOutIf3 =
      destIpv4Main->GetInterfaceForDevice(backupDestination3.Get(1));

  NS_ABORT_MSG_IF(
      destOutIf1 < 0 || destOutIf2 < 0 || destOutIf3 < 0,
      "Destination backup interface not found");

  Ptr<Ipv4StaticRouting> destStaticRouting =
      staticRoutingHelper.GetStaticRouting(destIpv4Main);

  destStaticRouting->AddHostRouteTo(
      sourceBackupIf.GetAddress(0),
      backupIf.GetAddress(0),
      static_cast<uint32_t>(destOutIf1));

  destStaticRouting->AddHostRouteTo(
      sourceBackupIf2.GetAddress(0),
      backupIf2.GetAddress(0),
      static_cast<uint32_t>(destOutIf2));

  destStaticRouting->AddHostRouteTo(
      sourceBackupIf3.GetAddress(0),
      backupIf3.GetAddress(0),
      static_cast<uint32_t>(destOutIf3));

  // Network-layer recovery state: gateway and interface used on the source
  // for each candidate, and routes on each backup router so it can forward
  // packets addressed to the primary destination IP to the destination.
  recSourceRouting = sourceStaticRouting;
  recPrimaryDest = primaryIf.GetAddress(1);

  recGateway[1] = sourceBackupIf.GetAddress(1);
  recGateway[2] = sourceBackupIf2.GetAddress(1);
  recGateway[3] = sourceBackupIf3.GetAddress(1);
  recOutIf[1] = static_cast<uint32_t>(backupOutIf);
  recOutIf[2] = static_cast<uint32_t>(backupOutIf2);
  recOutIf[3] = static_cast<uint32_t>(backupOutIf3);

  Ptr<Ipv4> r1Ipv4 = backupRouter.Get(0)->GetObject<Ipv4>();
  Ptr<Ipv4> r2Ipv4 = backupRouter2.Get(0)->GetObject<Ipv4>();
  Ptr<Ipv4> r3Ipv4 = backupRouter3.Get(0)->GetObject<Ipv4>();

  staticRoutingHelper.GetStaticRouting(r1Ipv4)->AddHostRouteTo(
      recPrimaryDest, backupIf.GetAddress(1),
      static_cast<uint32_t>(r1Ipv4->GetInterfaceForDevice(backupDestination.Get(0))));
  staticRoutingHelper.GetStaticRouting(r2Ipv4)->AddHostRouteTo(
      recPrimaryDest, backupIf2.GetAddress(1),
      static_cast<uint32_t>(r2Ipv4->GetInterfaceForDevice(backupDestination2.Get(0))));
  staticRoutingHelper.GetStaticRouting(r3Ipv4)->AddHostRouteTo(
      recPrimaryDest, backupIf3.GetAddress(1),
      static_cast<uint32_t>(r3Ipv4->GetInterfaceForDevice(backupDestination3.Get(0))));

  PacketSinkHelper sinkHelper(
      "ns3::UdpSocketFactory",
      InetSocketAddress(Ipv4Address::GetAny(), 9000));

  ApplicationContainer sink = sinkHelper.Install(destination.Get(0));
  sink.Start(Seconds(0.5));
  sink.Stop(Seconds(stopTime));

  Ptr<PacketSink> dataSink = DynamicCast<PacketSink>(sink.Get(0));
  dataSink->TraceConnectWithoutContext("Rx", MakeCallback(&RxTrace));

  Ptr<DrdmProbeEchoApp> echoServer =
      CreateObject<DrdmProbeEchoApp>();

  echoServer->Configure(9001, jitterMs);
  destination.Get(0)->AddApplication(echoServer);
  echoServer->SetStartTime(Seconds(0.5));
  echoServer->SetStopTime(Seconds(stopTime));

  std::vector<DrdmTrafficApp::CandidatePath> candidatePaths;

  DrdmTrafficApp::CandidatePath primaryPath;
  primaryPath.name = "PRIMARY";
  primaryPath.destination =
      InetSocketAddress(primaryIf.GetAddress(1), 9000);
  primaryPath.delayMs = linkDelayMs * 2.0;
  primaryPath.lossRate = lossRate;
  primaryPath.congestion = 0.10;

  candidatePaths.push_back(primaryPath);

  DrdmTrafficApp::CandidatePath backupPath1;
  backupPath1.name = "BACKUP_1";
  backupPath1.destination =
      InetSocketAddress(backupIf.GetAddress(1), 9000);
  backupPath1.delayMs = linkDelayMs * 2.0;
  backupPath1.lossRate = 0.0;
  backupPath1.congestion = 0.15;

  candidatePaths.push_back(backupPath1);

  DrdmTrafficApp::CandidatePath backupPath2;
  backupPath2.name = "BACKUP_2";
  backupPath2.destination =
      InetSocketAddress(backupIf2.GetAddress(1), 9000);
  backupPath2.delayMs = backup2DelayMs;
  backupPath2.lossRate = backup2LossRate;
  backupPath2.congestion = backup2Congestion;

  candidatePaths.push_back(backupPath2);

  DrdmTrafficApp::CandidatePath backupPath3;
  backupPath3.name = "BACKUP_3";
  backupPath3.destination =
      InetSocketAddress(backupIf3.GetAddress(1), 9000);
  backupPath3.delayMs = backup3DelayMs;
  backupPath3.lossRate = backup3LossRate;
  backupPath3.congestion = backup3Congestion;

  candidatePaths.push_back(backupPath3);

  Ptr<DrdmTrafficApp> traffic = CreateObject<DrdmTrafficApp>();

  traffic->Configure(
      candidatePaths,
      9000,
      MilliSeconds(20),
      delayWeight,
      lossWeight,
      congestionWeight);

  traffic->SetRoutingMode(routingRecovery);
  traffic->SetRouteUpdateCallback(MakeCallback(&InstallBackupRoute));

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

  detector->SetAdaptiveTimeout(adaptiveTimeout);
  detector->SetTimeoutMultiplier(timeoutMultiplier);

  detector->SetFailureCallback(
      MakeCallback(&DrdmTrafficApp::Failover, traffic));

  source.Get(0)->AddApplication(detector);
  detector->SetStartTime(Seconds(1.0));
  detector->SetStopTime(Seconds(stopTime));

  // Backup monitor setup.
  if (liveMetrics == 1)
    {
      monTraffic = traffic;
      monIntervalSec = monIntervalMs / 1000.0;
      monMissLimit = monThreshold;
      monStopSec = stopTime;

      monDest[0] = backupIf.GetAddress(1);
      monDest[1] = backupIf2.GetAddress(1);
      monDest[2] = backupIf3.GetAddress(1);

      for (int k = 0; k < MON_BACKUPS; k++)
        {
          monSrtt[k] = 0.0;
          monMin[k] = 0.0;
          monLoss[k] = 0.0;
          monReply[k] = 0;
          monMiss[k] = 0;
          monSamples[k] = 0;

          monSock[k] = Socket::CreateSocket(
              source.Get(0), UdpSocketFactory::GetTypeId());
          monSock[k]->Bind(
              InetSocketAddress(Ipv4Address::GetAny(), MON_BASE_PORT + k));
          monSock[k]->SetRecvCallback(MakeCallback(&MonRecv));
        }

      Simulator::Schedule(Seconds(0.6), &MonTick);
    }

  // Optional background load on one backup path.
  if (bgBackup > 0)
    {
      Ipv4Address bgDest = backupIf.GetAddress(1);
      if (bgBackup == 2)
        {
          bgDest = backupIf2.GetAddress(1);
        }
      if (bgBackup == 3)
        {
          bgDest = backupIf3.GetAddress(1);
        }

      PacketSinkHelper bgSinkHelper(
          "ns3::UdpSocketFactory",
          InetSocketAddress(Ipv4Address::GetAny(), 9500));
      ApplicationContainer bgSink = bgSinkHelper.Install(destination.Get(0));
      bgSink.Start(Seconds(0.5));
      bgSink.Stop(Seconds(stopTime));

      OnOffHelper bg("ns3::UdpSocketFactory", InetSocketAddress(bgDest, 9500));
      bg.SetConstantRate(
          DataRate(static_cast<uint64_t>(bgRateMbps * 1000000.0)), 1000);
      ApplicationContainer bgApps = bg.Install(source.Get(0));
      bgApps.Start(Seconds(bgStart));
      bgApps.Stop(Seconds(bgStop));
    }

  // Optional backup link failure.
  if (killBackup > 0)
    {
      Ptr<Node> killRouter = backupRouter.Get(0);
      Ptr<NetDevice> killRouterDev = backupDestination.Get(0);
      Ptr<NetDevice> killDestDev = backupDestination.Get(1);

      if (killBackup == 2)
        {
          killRouter = backupRouter2.Get(0);
          killRouterDev = backupDestination2.Get(0);
          killDestDev = backupDestination2.Get(1);
        }
      if (killBackup == 3)
        {
          killRouter = backupRouter3.Get(0);
          killRouterDev = backupDestination3.Get(0);
          killDestDev = backupDestination3.Get(1);
        }

      Simulator::Schedule(
          Seconds(killBackupTime), &KillLink,
          killRouter, destination.Get(0), killRouterDev, killDestDev);
    }

  NS_LOG_UNCOND(
      "DRDM: jitter maximum=" << jitterMs
      << " ms, primary-link packet loss=" << lossRate * 100.0
      << "% per direction, K=" << timeoutMultiplier
      << ", seed=" << seed
      << ", run=" << runNumber
      << ", liveMetrics=" << liveMetrics);

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
                routerIpv4->SetDown(
                    static_cast<uint32_t>(routerIf));
              }

            if (destIf >= 0)
              {
                destIpv4->SetDown(
                    static_cast<uint32_t>(destIf));
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

  NS_LOG_UNCOND(
      "DRDM: recovery mode=" << routingRecovery
      << " (1 = routing table, 0 = application layer), route updates="
      << recUpdates);
  NS_LOG_UNCOND(
      "DRDM: traffic sent=" << traffic->GetSent()
      << " delivered=" << rxPackets
      << " longest delivery gap=" << rxMaxGap * 1000.0
      << " ms starting at " << rxGapStart << " s");

  monitor->SerializeToXmlFile(
      "drdm-flowmon.xml", true, true);

  Simulator::Destroy();

  return 0;
}