# Prometheus Exporter

Requires `-DHSA_SNOOP_PROMETHEUS=ON` at build time.

```
sudo ./hsa-snoop --all --prometheus                  # serve on default port 9488
sudo ./hsa-snoop --all --prometheus-port 9100        # custom port
curl http://localhost:9488/metrics
```

## HSA / GPU Metrics

All metrics carry constant labels `host`, `platform`, `rocm_version`,
`discovery_mode`.

| Metric | Type | Labels | Description |
| --- | --- | --- | --- |
| `hsa_snoop_up` | Gauge | — | 1 when the exporter is running |
| `hsa_kernel_launches_total` | Counter | `kernel_name`, `gpu_id`, `gpu_type`, `pid`, `comm` | Kernel dispatch count |
| `hsa_kernel_duration_seconds` | Histogram | `kernel_name`, `gpu_id`, `gpu_type` | Kernel duration (host-observed, enqueue to queue-consumed) |
| `hsa_kernel_launches_per_second` | Gauge | `gpu_id`, `gpu_type` | Rolling 10 s launch rate |
| `hsa_last_kernel_launch_timestamp_seconds` | Gauge | `gpu_id`, `gpu_type` | Wall-clock time of most recent launch |
| `hsa_last_kernel_launch_info` | Gauge | `gpu_id`, `gpu_type`, `kernel_name` | Most recently launched kernel name (value always 1) |
| `hsa_active_queues` | Gauge | `gpu_id`, `gpu_type` | Currently tracked AQL queues |
| `hsa_barrier_packets_total` | Counter | `gpu_id`, `gpu_type`, `pid`, `comm` | Barrier AQL packets seen |
| `hsa_sdma_copies_total` | Counter | `gpu_id`, `gpu_type`, `pid`, `comm`, `direction` | SDMA copy packets, by direction (`h2d`/`d2h`/`d2d`) |
| `hsa_sdma_bytes_total` | Counter | `gpu_id`, `gpu_type`, `pid`, `comm`, `direction` | Bytes moved by SDMA copies |
| `hsa_sdma_packets_total` | Counter | `gpu_id`, `gpu_type`, `pid`, `comm`, `opcode` | SDMA packets by opcode |
| `hsa_active_sdma_queues` | Gauge | `gpu_id`, `gpu_type` | Currently tracked SDMA queues |
| `hsa_sdma_present` | Gauge | — | 1 when first SDMA queue seen; 0 on APU/unified-memory nodes |
| `hsa_triggered` | Gauge | `gpu_id`, `gpu_type` | 1 on first kernel dispatch on this GPU |
| `hsa_errors_total` | Counter | `gpu_id`, `pid` | Packets before queue registration |
| `hsa_xnack_total` | Counter | `pasid`, `comm` | GPU page-fault retries (`--xnack-snoop`) |

## AIS Metrics

All require `--ais-snoop`. Labels: `gpu_id`, `gpu_type`, `pcie_id`,
`device_type`, `vendor`.

| Metric | Type | Description |
| --- | --- | --- |
| `ais_snoop_armed` | Gauge | 1 if AIS kprobe started; absent if `--ais-snoop` not passed |
| `ais_active` | Gauge | 1 on first AIS operation |
| `ais_rx_ops_total` / `ais_tx_ops_total` | Counter | AIS ioctls completed |
| `ais_rx_bytes_total` / `ais_tx_bytes_total` | Counter | P2P DMA bytes transferred |
| `ais_rx_errors_total` / `ais_tx_errors_total` | Counter | Failed operations (extra `errno` label) |
| `ais_rx_latency_seconds` / `ais_tx_latency_seconds` | Histogram | ioctl entry-to-return latency |
| `ais_rx_io_size_bytes` / `ais_tx_io_size_bytes` | Histogram | Requested transfer size |
| `ais_pcie_device_info` | Gauge | Info metric per PCIe device |
