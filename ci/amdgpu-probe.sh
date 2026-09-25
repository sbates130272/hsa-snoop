#!/bin/bash
# Probe amdgpu with gfx1250 emulation parameters.
# Run after rocjitsu vfio-user server is ready and DKMS module is rebuilt.
# Copied from qemu-minimal/ansible/playbooks/vm-rocjitsu.yml.
set -euo pipefail

params=(
  emu_mode=1
  fw_load_type=0
  discovery=2
  # Upstream's qemu-vfio.md says 0x3f, which is right for its own
  # driver: there the compute IP blocks enumerate as common, gmc, ih,
  # gfx, sdma, mes -- indices 0..5. This DKMS build enumerates an
  # extra ras_v1_0 at index 5, so mes lands at 6 and 0x3f masks it
  # off. gfx_v12_1 needs MES to bring the command processor up, and
  # without it the probe oopses in gfx_v12_1_xcc_cp_resume. Count the
  # "detected ip block number" lines in dmesg before changing this.
  ip_block_mask=0x7f
  vm_update_mode=3
  gpu_recovery=0
  vramlimit=1024
)

# modprobe is a no-op on a resident module: it returns 0 without
# re-inserting, so every parameter above is discarded in silence.
# That is exactly what happens when a stale initramfs loaded amdgpu
# during boot. Compare what is live against what we asked for and
# say so, rather than exiting 0 having done nothing.
if [ -d /sys/module/amdgpu ]; then
  mismatch=()
  for kv in "${params[@]}"; do
    name=${kv%%=*}
    want=${kv#*=}
    # The kernel prints these decimal, so normalise 0x3f to 63.
    if [ "$want" != "${want#0x}" ]; then
      want=$((want))
    fi
    live=$(cat "/sys/module/amdgpu/parameters/$name" 2>/dev/null) || continue
    if [ "$live" != "$want" ]; then
      mismatch+=("$name: want $want, live $live")
    fi
  done

  if [ ${#mismatch[@]} -eq 0 ]; then
    echo "amdgpu already loaded with the emulation parameters."
    exit 0
  fi

  echo "amdgpu is already loaded with the wrong parameters:" >&2
  printf '  %s\n' "${mismatch[@]}" >&2
  echo "modprobe cannot change these on a resident module." >&2
  exit 1
fi

exec sudo modprobe amdgpu "${params[@]}"
