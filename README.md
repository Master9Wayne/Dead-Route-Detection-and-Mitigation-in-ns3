# DRDM: Dead Route Detection and Mitigation (ns-3.49)

This is a small multi-file ns-3 example with a primary path and a backup path. It detects primary-path failures using UDP probes and switches application traffic to a backup destination.

## Files

- `drdm.cc`: topology, applications, failure injection, simulation setup, and FlowMonitor configuration.
- `drdm-probe-app.h/.cc`: sends sequence-numbered UDP probes, measures RTT, calculates fixed or adaptive timeouts, and declares the primary path dead after a configurable number of consecutive timeouts.
- `drdm-traffic-app.h/.cc`: generates UDP traffic and switches its destination from the primary destination interface to the backup destination interface after detection.
- `CMakeLists.txt`: builds the source files as one ns-3 scratch example.

## Install

Copy the `drdm` directory into `~/ns-3.49/scratch/drdm/`. If you have older files from a previous attempt, move them out of `scratch/` first so they are not compiled as separate scratch programs.

From the ns-3.49 root:

```bash
./ns3 configure
./ns3 build
```

Run the default scenario:

```bash
./ns3 run "scratch/drdm --failureTime=4 --stopTime=12"
```

Try changing detection settings:

```bash
./ns3 run "scratch/drdm --failureTime=4 --stopTime=12 --probeIntervalMs=50 --probeTimeoutMs=30 --threshold=3"
./ns3 run "scratch/drdm --injectFailure=0 --stopTime=10"
```

Expected messages include the injected failure, `PRIMARY PATH DECLARED DEAD`, and `switched traffic to backup path`. The program writes `drdm-flowmon.xml`.

## Adaptive Probe Timeout

The original implementation uses a fixed probe timeout. The extended implementation supports an adaptive timeout calculated using the measured round-trip time (RTT) of successful UDP probes.

It maintains two estimates:

- **SRTT:** Smoothed round-trip time.
- **RTTVAR:** Smoothed variation in round-trip time.

The timeout is calculated as:

```text
Timeout = SRTT + K * RTTVAR
```

The default multiplier is `K = 4`. SRTT uses a smoothing factor of `1/8`, while RTTVAR uses `1/4`. The calculated timeout is bounded between 10 ms and 500 ms. The configured timeout is used until the first RTT sample arrives.

The failure threshold remains configurable and defaults to three consecutive missed probes. Keeping this threshold unchanged makes it possible to compare fixed and adaptive timeout behaviour separately.

## Running Fixed and Adaptive Modes

From the ns-3.49 root, using the standard scratch workflow, run the fixed-timeout baseline:

```bash
./ns3 run "scratch/drdm --failureTime=4 --stopTime=12 --probeTimeoutMs=100 --adaptiveTimeout=false"
```

Run the adaptive-timeout version:

```bash
./ns3 run "scratch/drdm --failureTime=4 --stopTime=12 --probeTimeoutMs=100 --adaptiveTimeout=true --timeoutMultiplier=4"
```

The extended implementation also supports these experiment parameters:

- `--linkDelayMs`: Propagation delay per point-to-point link.
- `--jitterMs`: Maximum extra random delay applied to probe replies.
- `--lossRate`: Packet-loss probability on the primary-router-to-destination link, applied in both directions.
- `--probeIntervalMs`: Interval between probes.
- `--probeTimeoutMs`: Fixed timeout or initial timeout before RTT estimation.
- `--threshold`: Consecutive missed probes required to declare a failure.
- `--timeoutMultiplier`: Multiplier `K` in the adaptive timeout formula.
- `--injectFailure=false`: Runs without injecting an actual link failure.
- `--seed` and `--runNumber`: Configure random-number generation for repeatable simulations.

Adaptive mode prints RTT, SRTT, RTTVAR, the selected multiplier, and the calculated timeout. FlowMonitor statistics are written to `drdm-flowmon.xml` in the current working directory.

### Local ns-3-dev Build

For a local setup with the project located at `~/ns-3-dev/scratch/drdm-project`, build it from the ns-3-dev root:

```bash
ninja -C cmake-cache scratch_drdm-project_drdm
```

Run the fixed baseline:

```bash
./build/scratch/drdm/ns3-dev-drdm-default --failureTime=4 --stopTime=12 --probeTimeoutMs=100 --adaptiveTimeout=false
```

Run adaptive mode:

```bash
./build/scratch/drdm/ns3-dev-drdm-default --failureTime=4 --stopTime=12 --probeTimeoutMs=100 --adaptiveTimeout=true --timeoutMultiplier=4
```

## Experimental Results

The experiments compare the original fixed timeout with adaptive RTT-based timeout detection. The failure threshold remained at three consecutive missed probes.

### 1. Stable Propagation Delays

These tests used a 100 ms probe interval, a 100 ms fixed timeout, a failure time of 4 seconds, and no added jitter or packet loss.

| Delay per link | Fixed detection latency | Adaptive detection latency |
|---:|---:|---:|
| 2 ms | 300 ms | 210 ms |
| 5 ms | 300 ms | 220.27 ms |
| 10 ms | 300 ms | 240.28 ms |
| 20 ms | 300 ms | 280.30 ms |

Detection latency is measured relative to the injected failure at 4 seconds. Adaptive timeout reduced detection latency in all four tested scenarios.

### 2. Jitter and Packet Loss

The implementation was also tested with 20 ms maximum additional probe-reply jitter and 5% primary-link packet loss.

Across 30 runs per mode:

| Metric | Fixed timeout | Adaptive timeout (`K = 4`) |
|---|---:|---:|
| Healthy-path false detections | 7/30 | 7/30 |
| Premature detections before the injected failure | 2/30 | 2/30 |
| Correct detections after the failure | 28/30 | 28/30 |
| Missed detections | 0/30 | 0/30 |
| Average latency for correct detections | 296.43 ms | 235.53 ms |

Under these conditions, adaptive timeout reduced average detection latency by approximately 60.9 ms, or 20.5%, among runs that detected the failure after it occurred. It did not reduce the observed false-detection rate under packet loss.

### 3. Multiplier Comparison

An initial 10-run comparison tested `K = 2`, `K = 4`, and `K = 6` with 20 ms maximum jitter and 5% packet loss.

| Multiplier | Healthy-path false detections | Premature detections during failure tests |
|---:|---:|---:|
| 2 | 8/10 | 4/10 |
| 4 | 2/10 | 1/10 |
| 6 | 2/10 | 1/10 |

`K = 4` was retained as the default candidate because it had the same observed false-detection counts as `K = 6` while detecting failures slightly earlier in these preliminary trials. These results do not establish a universally optimal multiplier.

## Scope / Limitations

This is intentionally a simple demonstration. Detection is based on UDP probe timeouts. Mitigation is implemented by changing the traffic application's destination IP to the destination's backup-link interface address.

It demonstrates detection and application-level failover; it is not a general-purpose IPv4 routing protocol that repairs arbitrary routes while preserving a single stable destination IP.

The experiments show faster failure detection in the tested scenarios, but adaptive timeout did not reduce false detections under the tested packet-loss conditions. Additional testing across more network conditions and random seeds would be needed to establish broader reliability.

## Future Work

The following two modifications are planned for future development.

### 1. Intelligent Backup Route Selection

Currently, the simulation switches to a predefined backup destination when the primary path fails. A future extension will support multiple candidate backup routes and dynamically select the most suitable route based on normalized network metrics such as delay, packet loss, and congestion.

A weighted scoring mechanism will be explored to rank candidate routes. The results will be compared against fixed-backup selection and shortest-path routing using metrics such as recovery time, packet loss, and end-to-end delay.

### 2. Dynamic Routing-Table Repair

Currently, failover is implemented at the application level by changing the destination IP address. A future extension will implement routing-level recovery by updating the forwarding state when the primary path fails, while keeping the destination IP unchanged.

This modification will be evaluated using route convergence time, packet loss during recovery, and end-to-end connectivity before and after a failure.

These extensions are planned improvements and have not yet been implemented or evaluated in the current version.

