# DRDM: Dead Route Detection and Mitigation (ns-3.49)

This is a small multi-file ns-3 example with a primary path and a backup path.

## Files

- `drdm.cc`: topology, applications, failure injection, and simulation setup.
- `drdm-probe-app.h/.cc`: sends sequence-numbered UDP probes and declares the primary path dead after a configurable number of consecutive timeouts.
- `drdm-traffic-app.h/.cc`: generates UDP traffic and switches its destination from the primary destination interface to the backup destination interface after detection.
- `CMakeLists.txt`: builds all three `.cc` files as one ns-3 scratch example.

## Install

Copy the `drdm` directory into `~/ns-3.49/scratch/drdm/`. If you have older files from the previous attempt, move them out of `scratch/` first so they are not compiled as separate scratch programs.

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

## Scope / limitation

This is intentionally a simple demonstration. Detection is based on UDP probe timeouts. Mitigation is implemented by changing the traffic application's destination IP to the destination's backup-link interface address. It demonstrates detection and application-level failover; it is not a general-purpose IPv4 routing protocol that repairs arbitrary routes while preserving a single stable destination IP.
