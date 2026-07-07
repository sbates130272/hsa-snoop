# Examples

`vecwork.hip` — a small HIP workload that launches `vadd` and `vscale` kernels
in a loop with no per-iteration sync, so packets stay queued long enough to be
captured cleanly. Used to validate hsa-snoop.

```
hipcc examples/vecwork.hip -o /tmp/vecwork
sudo ./hsa-snoop -- /tmp/vecwork
# open hsa-snoop.pftrace at https://ui.perfetto.dev
```
