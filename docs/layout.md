# Project Layout

```
src/aql.{h,cpp}                    AQL packet formats (mirrors <hsa/hsa.h>)
src/sdma.{h,cpp}                   SDMA microcode packet formats + length decode
src/model.h                        QueueInfo / PacketRecord / SdmaRecord data model
src/discovery.{h,cpp}              queue discovery: kprobe (native), bpftrace (WSL2/librocdxg), kfd-uprobe (emulated GPU)
src/proc_mem.{h,cpp}               process_vm_readv + pagemap (VA->phys)
src/ksym.{h,cpp}                   kernel_object -> demangled name via code-object ELFs
src/parser.{h,cpp}                 per-queue ring poller / AQL + SDMA decoder / timing
src/trace_writer.{h,cpp}           Perfetto protobuf + Chrome JSON writers
src/prometheus_exporter.{h,cpp}    Prometheus metrics exporter (optional)
src/dispatch_log.{h,cpp}           NDJSON per-event log (--log-dispatches)
src/main.cpp                       CLI / orchestration
scripts/hsa-snoop.bt               zero-build discovery via bpftrace
scripts/slurm/                     multi-GFX Slurm validation + capture harness
scripts/observability/             offline Grafana/Prometheus/Loki replay stack
deploy/grafana/hsa-snoop-dashboard.json       reference Grafana dashboard
deploy/systemd/hsa-snoop-prometheus.service   systemd unit (Prometheus exporter daemon)
deploy/systemd/hsa-snoop.conf                 runtime configuration (extra ExecStart arguments)
docs/                              documentation (markdown)
docs/website/                      GitHub Pages site assets
```
