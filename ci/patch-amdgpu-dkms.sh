#!/bin/bash
# Apply the amdgpu_ras.c NULL atom_context guard and rebuild DKMS.
#
# The rocjitsu device is exposed with rombar=0, so amdgpu_device_init leaves
# adev->mode_info.atom_context NULL.  amdgpu_ras_init then calls
# amdgpu_atomfirmware_mem_ecc_supported which dereferences atom_context
# unconditionally, oopsing in amdgpu_atom_parse_data_header.
#
# Fix mirrors how amdgpu_ras_get_quirks (immediately above) guards the same
# field.  Patch the DKMS source so a later dkms autoinstall on a new kernel
# also picks it up.
#
# Run as the guest user; root comes from sudo (password via VM_SUDO_PASSWORD).
set -euo pipefail
VM_SUDO_PASSWORD=$(printf '%s' "$VM_PASSWORD_B64" | base64 -d)
sudo_cmd() { printf '%s\n' "$VM_SUDO_PASSWORD" | sudo -S -p '' "$@"; }

sudo_cmd python3 - << 'PYEOF'
import glob, sys
srcs = sorted(glob.glob('/usr/src/amdgpu-*'))
if not srcs:
    sys.exit('no amdgpu DKMS source found')
path = srcs[-1] + '/amd/amdgpu/amdgpu_ras.c'
old = ('static void amdgpu_ras_query_ras_capablity_from_vbios('
       'struct amdgpu_device *adev)\n{\n\t/* mem_ecc cap */')
new = ('static void amdgpu_ras_query_ras_capablity_from_vbios('
       'struct amdgpu_device *adev)\n{\n'
       '\tif (!adev->mode_info.atom_context)\n\t\treturn;\n\n'
       '\t/* mem_ecc cap */')
content = open(path).read()
if new in content:
    print('already patched')
    sys.exit(0)
if old not in content:
    sys.exit(f'target not found in {path}')
open(path, 'w').write(content.replace(old, new, 1))
print(f'patched {path}')
PYEOF

VER=$(dkms status amdgpu | awk -F'[,/]' 'NR==1{gsub(/ /,"",$2); print $2}')
KVER=$(uname -r)
sudo_cmd dkms build  "amdgpu/${VER}" -k "${KVER}" --force
sudo_cmd dkms install "amdgpu/${VER}" -k "${KVER}" --force
echo "amdgpu DKMS patched and rebuilt for ${KVER}"
