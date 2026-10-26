# DRDM: Dead Route Detection and Mitigation (ns-3)

DRDM is an ns-3 example that detects a dead primary path with UDP probes, scores the available backup paths using live measurements, and recovers either at the application layer (switching the destination IP) or at the network layer (updating the routing table so the destination IP stays the same).

The project is built in three steps:

1. **Adaptive RTT-based dead-route detection** (faster, more responsive detection).
2. **Intelligent backup-path selection** (live delay, loss and congestion scoring).
3. **Routing-table-based recovery** (mitigation moved from the application layer into the network layer).

## Topology

```
                 +--> PRIMARY router  --+
                 |                      |
 source ---------+--> BACKUP_1 router --+--> destination
                 |                      |
                 +--> BACKUP_2 router --+
                 |                      |
                 +--> BACKUP_3 router --+
```

Every path is two point-to-point links. Each link has its own subnet. Backup 2 and Backup 3 use link delays derived from `--backup2DelayMs` and `--backup3DelayMs` when `--liveMetrics=1`.

## Files

- `drdm.cc`: topology, applications, backup-path monitor, routing-table recovery, failure injection, FlowMonitor.
- `drdm-probe-app.h/.cc`: sequence-numbered UDP probes, fixed or adaptive timeout (`SRTT + K * RTTVAR`), failure declaration after consecutive misses.
- `drdm-traffic-app.h/.cc`: traffic generator, candidate-path scoring, failover (application-layer or routing-table).
- `CMakeLists.txt`: builds the sources as one ns-3 scratch program.

## Build and run

From the ns-3-dev root, with the project in `scratch/drdm-project/`:

```bash
ninja -C cmake-cache scratch_drdm-project_drdm
```

Run the default scenario (routing-table recovery):

```bash
./build/scratch/drdm/ns3-dev-drdm-default --failureTime=6 --stopTime=12 --killBackup=1 --killBackupTime=3
```

Run the same scenario with application-layer recovery for comparison:

```bash
./build/scratch/drdm/ns3-dev-drdm-default --failureTime=6 --stopTime=12 --killBackup=1 --killBackupTime=3 --routingRecovery=0
```

## Step 1: Adaptive probe timeout

The fixed-timeout detector is extended with an adaptive timeout calculated from the RTT of successful probes:

```text
Timeout = SRTT + K * RTTVAR
```

SRTT uses a smoothing factor of 1/8 and RTTVAR uses 1/4. The default multiplier is `K = 4`. The timeout is bounded between 10 ms and 500 ms. The configured timeout is used until the first RTT sample arrives. The failure threshold defaults to three consecutive missed probes.

Run fixed and adaptive modes:

```bash
./build/scratch/drdm/ns3-dev-drdm-default --failureTime=4 --stopTime=12 --probeTimeoutMs=100 --adaptiveTimeout=false
./build/scratch/drdm/ns3-dev-drdm-default --failureTime=4 --stopTime=12 --probeTimeoutMs=100 --adaptiveTimeout=true --timeoutMultiplier=4
```

### Results: stable propagation delays

100 ms probe interval, 100 ms fixed timeout, failure at 4 s, no jitter or loss.

| Delay per link | Fixed detection latency | Adaptive detection latency |
|---:|---:|---:|
| 2 ms | 300 ms | 210 ms |
| 5 ms | 300 ms | 220.27 ms |
| 10 ms | 300 ms | 240.28 ms |
| 20 ms | 300 ms | 280.30 ms |

### Results: jitter and packet loss

20 ms maximum probe-reply jitter and 5% primary-link packet loss, 30 runs per mode.

| Metric | Fixed timeout | Adaptive timeout (`K = 4`) |
|---|---:|---:|
| Healthy-path false detections | 7/30 | 7/30 |
| Premature detections before the injected failure | 2/30 | 2/30 |
| Correct detections after the failure | 28/30 | 28/30 |
| Missed detections | 0/30 | 0/30 |
| Average latency for correct detections | 296.43 ms | 235.53 ms |

Adaptive timeout reduced average detection latency by about 60.9 ms (20.5%) among runs that detected the failure after it occurred. It did not reduce false detections under packet loss.

### Results: multiplier comparison

10 runs each with 20 ms jitter and 5% packet loss.

| Multiplier | Healthy-path false detections | Premature detections during failure tests |
|---:|---:|---:|
| 2 | 8/10 | 4/10 |
| 4 | 2/10 | 1/10 |
| 6 | 2/10 | 1/10 |

`K = 4` was kept as the default. These results do not establish a universally optimal multiplier.

## Step 2: Intelligent backup-path selection

Every backup path is probed continuously (`--monIntervalMs`, default 100 ms) through the same network the traffic uses. Three metrics are measured per backup:

- **Delay:** half of the smoothed RTT (SRTT with factor 1/8), in ms.
- **Loss:** exponentially weighted moving average of missed probes (each miss adds 20%, each reply decays it).
- **Congestion:** RTT inflation over the lowest observed RTT, `(SRTT - minRTT) / minRTT`, capped at 1.

A backup that misses `--monThreshold` (default 3) consecutive probes is marked dead and is never selected.

At failover each live backup is scored and the lowest score wins:

```text
score = wDelay * (delay / maxDelay) + wLoss * loss + wCongestion * congestion
```

Delay is normalized by the largest delay among all candidates. Loss and congestion are already in [0, 1]. The weights default to 0.5, 0.3 and 0.2, are normalized to sum to 1, and are set with `--delayWeight`, `--lossWeight` and `--congestionWeight`.

Test options:

- `--liveMetrics=0`: use the configured metrics instead of measured ones.
- `--bgBackup=N --bgRateMbps=R --bgStart=T --bgStop=T`: inject background load on backup N.
- `--killBackup=N --killBackupTime=T`: fail the link of backup N.
- `--backup2LossRate`, `--backup3LossRate`: real packet loss on backups 2 and 3.

Verified in simulation: with Backup 1 killed at 3 s, Backup 1 is marked dead and skipped at failover, and the best remaining backup (Backup 2) is selected. With background load on Backup 1, its measured congestion rises and its score reflects it. Probe replies are pinned to the matching backup router so a primary failure does not corrupt backup measurements.

## Step 3: Routing-table-based recovery

In routing-table mode (`--routingRecovery=1`, the default) the traffic application never changes its destination IP. The traffic always goes to the primary destination address. When the primary path is declared dead:

1. The best live backup is chosen with the Step 2 scoring.
2. The source node's routing table is updated: any existing host route for the primary destination is removed, and a host route for the same destination IP is installed through the chosen backup's gateway and interface.
3. Each backup router already has a route for the primary destination IP that forwards it to the destination, so the destination is reached over the backup path.

With `--routingRecovery=0` the original behavior is kept: the application switches its destination to the backup's own address.

At the end of a run the program prints the recovery mode, the number of route updates, packets sent and delivered, and the longest gap in packet delivery at the receiver (the service interruption) with the time it started.

Expected log lines:

```text
DRDM: PRIMARY PATH DECLARED DEAD at ...
DRDM: selected backup candidate BACKUP_x with score=...
DRDM: network-layer recovery, destination stays 10.0.2.2
DRDM: routing table updated at ... s: 10.0.2.2 via <gateway> (interface N, candidate x)
DRDM: sent N traffic packets to 10.0.2.2 via BACKUP_x
DRDM: recovery mode=1 ... route updates=1
DRDM: traffic sent=... delivered=... longest delivery gap=... ms starting at ... s
```

## Scope / limitations

- Detection is based on UDP probe timeouts of the primary path.
- Recovery is a one-time switch from the primary to the best backup at the moment the primary is declared dead. A backup that later degrades is not re-evaluated, and traffic does not return to the primary.
- Routing-table recovery installs a host route for the destination on the source node. It is not a general routing protocol that repairs arbitrary routes network-wide.
- Congestion is estimated from RTT inflation, not from queue lengths.
- Experimental conclusions apply to the tested scenarios only. Additional conditions and random seeds would be needed to establish broader reliability.