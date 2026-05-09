# LDLB-NS3

**Label-Based Dynamic Load Balance Algorithm for Data Center Networks** — ns-3 Simulation

> **Note:** This project is developed on top of the [HPCC ns-3 simulator](https://github.com/hpcc-group/High-Precision-Congestion-Control) ([HPCC: High Precision Congestion Control, SIGCOMM' 2019](https://rmiao.github.io/publications/hpcc-li.pdf)). We gratefully acknowledge the HPCC team for open-sourcing their simulation platform.

## Overview

LDLB is a load balancing algorithm for data center networks that uses congestion-aware labels to dynamically reroute traffic away from congested links. The key idea is:

1. **Local Congestion Detection**: Each switch monitors its output port queue using RED/ECN thresholds to detect local congestion
2. **Congestion Label Generation**: When congestion is detected, the switch generates a congestion label identifying the affected downstream destinations
3. **Label Propagation & Broadcast**: Labels are broadcast to upstream neighbor switches with dampening and stability guard to prevent oscillation
4. **Label-Guided Routing**: Upstream switches use received labels to select congestion-free alternative paths via hash-based tie-breaking among multiple candidate routes

This simulator also includes implementations of:
- **HPCC** — High Precision Congestion Control (SIGCOMM '19)
- **HPCC-PINT** — HPCC with Probabilistic In-band Network Telemetry (SIGCOMM '20)
- **DCQCN** — Data Center QCN
- **TIMELY** — RTT-based congestion control
- **DCTCP** — Data Center TCP

## Prerequisites

- Ubuntu 18.04 or 20.04
- GCC/G++ 5 (GCC > 5 may cause compilation errors due to ns-3 code style)
- Python 2.7 or 3.x

## Build

```bash
cd simulation/
CC='gcc-5' CXX='g++-5' ./waf configure
./waf build
```

## Run

### Quick Run

```bash
cd simulation/
python run.py --cc none --trace mine --bw 25 --topo topo1
```

### Available CC Modes (--cc)

| Value | Algorithm |
|-------|-----------|
| `none` | LDLB (no rate control, label-based load balancing) |
| `dcqcn` | DCQCN |
| `dctcp` | DCTCP |
| `timely` | TIMELY |
| `hp` | HPCC |
| `hpccPint` | HPCC-PINT |

### LDLB-Specific Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `--enable_mmu_stability_guard` | 1 | Enable stability guard for congestion detection |
| `--mmu_stability_ewma_alpha` | 0.25 | EWMA alpha for stability guard |
| `--mmu_stability_enter_samples` | 2 | Samples required to declare congestion |
| `--mmu_stability_exit_samples` | 3 | Samples required to declare congestion cleared |
| `--enable_broadcast_dampening` | 1 | Enable broadcast dampening to prevent oscillation |
| `--broadcast_stable_rounds` | 2 | Stable rounds required before broadcasting |
| `--min_broadcast_interval_ns` | 50000 | Minimum interval between broadcasts (ns) |
| `--enable_congestion_label_compression` | 0 | Enable prefix compression for labels |

### Experiment Configuration

Edit `simulation/mix/config.txt` or use `run.py` to auto-generate configs.
See `simulation/mix/config_doc.txt` for parameter documentation.

## Project Structure

```
simulation/
├── scratch/
│   └── third2.cc              # Main simulation entry point
├── src/point-to-point/model/
│   ├── switch-node.cc/h      # Switch node with label management & routing
│   ├── switch-mmu.cc/h       # Switch MMU with congestion detection (RED/ECN)
│   ├── qbb-net-device.cc/h   # Net device with label encoding/decoding
│   ├── rdma-hw.cc/h          # RDMA hardware (congestion control core)
│   ├── rdma-driver.cc/h      # RDMA driver layer
│   └── rdma-queue-pair.cc/h  # Queue pair management
├── mix/                      # Topology, traffic, and config files
├── run.py                    # Experiment runner

```

## Key LDLB Source Files

The LDLB-specific modifications are primarily in:

- **`switch-mmu.cc/h`**: Local congestion detection via RED/ECN thresholds, `congestedlabel` map tracking congested ports
- **`switch-node.cc/h`**: Label table management, label-to-node mapping, broadcast with dampening
- **`qbb-net-device.cc/h`**: Congestion label encoding (`BuildCongestionLabelPayload`), decoding (`DecodeCongestionLabelPayload`), multi-candidate route selection with hash tie-breaking

## Citation

If you use this code in your research, please cite:

> [Your LDLB paper citation here]

## License

This project is licensed under **GPL-2.0**, inherited from the base ns-3 and HPCC simulator. See `simulation/LICENSE` for details.
