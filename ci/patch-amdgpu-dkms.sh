#!/bin/bash
# Apply the amdgpu_ras.c NULL atom_context guard and rebuild DKMS.
# Run as the guest user with VM_PASSWORD_B64 set in the environment.
set -euo pipefail
VM_SUDO_PASSWORD=$(printf '%s' "$VM_PASSWORD_B64" | base64 -d)
sudo_cmd() { printf '%s\n' "$VM_SUDO_PASSWORD" | sudo -S -p '' "$@"; }

PATCH_PY='
import glob, sys
srcs = sorted(glob.glob("/usr/src/amdgpu-*"))
if not srcs:
    sys.exit("no amdgpu DKMS source found")
path = srcs[-1] + "/amd/amdgpu/amdgpu_ras.c"
old = ("static void amdgpu_ras_query_ras_capablity_from_vbios("
       "struct amdgpu_device *adev)\n{\n\t/* mem_ecc cap */")
new = ("static void amdgpu_ras_query_ras_capablity_from_vbios("
       "struct amdgpu_device *adev)\n{\n"
       "\tif (!adev->mode_info.atom_context)\n\t\treturn;\n\n"
       "\t/* mem_ecc cap */")
content = open(path).read()
if new in content:
    print("already patched")
    sys.exit(0)
if old not in content:
    sys.exit("target not found in " + path)
open(path, "w").write(content.replace(old, new, 1))
print("patched " + path)
'

sudo_cmd python3 -c "$PATCH_PY"

VER=$(dkms status amdgpu | awk -F'[,/]' 'NR==1{gsub(/ /,"",$2); print $2}')
KVER=$(uname -r)
sudo_cmd dkms build  "amdgpu/${VER}" -k "${KVER}" --force
sudo_cmd dkms install "amdgpu/${VER}" -k "${KVER}" --force
echo "amdgpu DKMS patched and rebuilt for ${KVER}"
