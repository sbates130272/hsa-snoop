# overnight.md — hsa-snoop overnight work plan

**Date:** 2026-09-21  
**Branch:** `copilot/add-hsa-snoop-test-workflow` (PR #37)

---

## Status

### What I found

**PR #37** adds `.github/workflows/hsa-snoop-vm-hardware-test.yml`: a full
rocjitsu-in-docker compose stack that boots a gfx1250 VM, builds + installs
hsa-snoop, runs unit and hardware tests, exercises all modes (launch, pid,
--all, Prometheus + AIS), and uploads artifacts.

**Local environment assessment:**

| Requirement | Status |
|---|---|
| `/dev/kvm` | Available |
| `docker` | Running (logged in as `sbates130272`) |
| `qemu-system-x86_64` | `/usr/bin/qemu-system-x86_64` present |
| `oras` | **Missing** — needed to pull the qcow2 VM image from ORAS registry |
| `qemu-minimal` | **Not checked out** — needed for compose stack |
| QEMU image `20260919.g359579e` | **Not cached** (have older tags) |
| rocjitsu image `20260921.gb3399b3` | **Not cached** (have older tags) |
| rocjitsu VM qcow2 image | **Not fetched** |
| Disk space | 195 GB free — sufficient |

**Blockers to spin up the exact workflow locally:**
1. `oras` is not installed — required to pull the qcow2 image from the OCI registry
2. `qemu-minimal` is not checked out
3. Exact tagged images for the workflow may need pulling (~15 GB total)

These are all solvable. None are hard blockers if I install `oras` and pull
the required images. That will take 30–60 min of download time.

**rocm-ernic perf → GitHub Pages pattern (understood):**
- Performance data is `JSONL` appended to `gh-pages` branch under `perf/history.jsonl`
- Shields badges go to `gh-pages/perf/badge-*.json`
- A composite action `publish-perf` handles: CSV → `gen-report.py` → append
  to history → push `gh-pages` (with 3-attempt retry for concurrent lanes)
- A `docs-deploy.yml` workflow pulls the history at build time and embeds it
  in a Sphinx site
- The two halves (site + perf) share `gh-pages` but write disjoint subtrees,
  using `rsync --exclude perf/` to keep them from clobbering each other

---

## Work plan (overnight)

### Phase 1 — Environment setup
- [ ] Install `oras` via binary release
- [ ] Check out `qemu-minimal` (`sbates130272/qemu-minimal`)
- [ ] Pull exact QEMU and rocjitsu images from the workflow env vars
- [ ] Fetch and unpack the rocjitsu VM qcow2 via `oras pull`
- [ ] Verify KVM + compose stack comes up, VM boots, amdgpu loads

### Phase 2 — Run PR #37 test suite locally
- [ ] Build hsa-snoop in the VM (gfx1250 target)
- [ ] Run unit tests (`ctest --exclude-regex sdma-hardware-test`)
- [ ] Run hardware test (`ctest -R sdma-hardware-test`)
- [ ] Run Prometheus / AIS / fio coverage step
- [ ] Collect and record pass/fail in this file

### Phase 3 — GitHub Pages perf infrastructure (adapted from rocm-ernic)
- [ ] Design `docs/perf-history/history.jsonl` schema for hsa-snoop metrics
  (kernel launches/sec, SDMA BW, AIS BW, with sha + date + runner metadata)
- [ ] Write `ci/report/publish-perf.py` (adapted from rocm-ernic pattern)
  to extract metrics from Prometheus output and append to history
- [ ] Write `ci/report/gen-report.py` to produce summary JSON + shields badges
- [ ] Add `gh-pages` branch (seeded with `.nojekyll` + empty `perf/history.jsonl`)
- [ ] Add `.github/actions/publish-perf/action.yml` composite action
- [ ] Wire it into the vm-hardware-test workflow with `publish: true` on `main` pushes
- [ ] Add a simple static HTML trend page (Chart.js, no Sphinx dependency)
  so GitHub Pages renders the history without a build step

### Phase 4 — hsa-snoop improvements (opportunistic, from test runs)
- [ ] Note any test failures or gaps discovered during Phase 2
- [ ] Fix any issues found
- [ ] Review whether the AIS / procmem / xnack paths have unit test coverage
  (currently only hardware tests cover these)
- [ ] Check if `ais_monitor` and `xnack_monitor` have any edge cases exposed
  by the rocjitsu emulator that wouldn't show on real hardware

---

## Progress log

- **00:12** — overnight.md created; environment assessed; work plan written
- **00:12** — `oras` v1.2.2 installed; `qemu-minimal` checked out; exact Docker images pulled
- **00:12** — qcow2 VM image fetched via oras, SSH key extracted, overlay created
- **00:15** — **Bug found in PR #37 workflow**: `oras discover --format json` changed key from `referrers` → `manifests` in oras 1.x; fixed with fallback `.get("manifests", .get("referrers", []))` in [hsa-snoop-vm-hardware-test.yml](.github/workflows/hsa-snoop-vm-hardware-test.yml:96)
- **00:16** — Compose stack up: rocjitsu healthy, QEMU VM booted (Ubuntu 26.04.1)
- **00:22** — **SSH debugging**: guest sshd socket-activated; NIC `enp0s3` (MAC `52:54:00:00:08:ae`) not in netplan (configured for `enp0s2` MAC `52:54:00:12:34:56`). Fixed via QGA: started sshd.service + networkctl brought NIC up to 10.0.2.15. SSH works from inside the QEMU container via hostfwd.
- **00:22** — **Second netplan bug found**: qemu-tool generates MAC `52:54:00:00:XX:XX` but VM image netplan has `52:54:00:12:34:56`. Need to align these or use `match: driver: virtio_net` instead.
- **00:29** — Workaround: static IP assignment via QGA, sshd manually started — SSH working inside container. CI fails at "Wait for VM SSH" because no such workaround exists in the workflow loop.
- **00:35** — **Third bug found**: `amdgpu-dkms 7.1.9` crashes with NULL deref in `amdgpu_ras_init → amdgpu_atomfirmware_mem_ecc_supported` on every modprobe attempt in emu_mode. Neither `ras_enable=0` nor `ip_block_mask=0x5f` prevents the crash. Root cause: `adev->mode_info.atom_context` is NULL (no BIOS ROM in rocjitsu), but `amdgpu_ras_init` doesn't guard against this. Needs a dkms patch.
- **00:35** — Workflow never ran successfully in CI (all runs fail at SSH wait, caused by netplan bug). Pivoting to: fixing the workflow, building GitHub Pages perf infrastructure, and writing up the bugs.
- **06:38** — Built GitHub Pages perf infrastructure on `feat/gh-pages-perf`:
  - `ci/report/publish-perf.py` — parse Prometheus text, append JSONL history, write shields badges, render Chart.js HTML trend page
  - `.github/actions/publish-perf/action.yml` — composite action: append record, push to `gh-pages/perf/`, 3-attempt retry for concurrent lanes (mirrors rocm-ernic pattern)
  - `docs/perf-history/history.jsonl` — empty placeholder committed to main
  - `gh-pages` branch seeded with `.nojekyll + perf/history.jsonl + perf/index.html`
  - `hsa-snoop-vm-hardware-test.yml` — added Prometheus collection step + publish-perf action (main pushes only), `contents: write` permission on job
  - Script tested with synthetic Prometheus data: all 4 metrics extracted correctly
