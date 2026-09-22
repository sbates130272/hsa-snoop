#!/bin/sh
# qemu-tool run-vm entrypoint with --mgmt-tap.
# Replaces the default compose entrypoint when TAP bridge networking is needed.
set -e
cp -r /qemu-tool-src/qemu /tmp/qemu-tool-build
PIPX_BIN_DIR=/usr/local/bin pipx install --force /tmp/qemu-tool-build

socks=$(ls /run/vfu/*.sock 2>/dev/null | tr '\n' ',' | sed 's/,$//')
if [ -z "$socks" ]; then
    echo "ERROR: no vfio-user sockets found in /run/vfu" >&2
    exit 1
fi

exec qemu-tool run-vm \
    --images "${VM_IMAGES_DIR:-/var/lib/qemu-tool/images}" \
    --vm-name "${VM_NAME:-qemu-minimal}" \
    --vcpus "${VM_VCPUS:-4}" \
    --vmem "${VM_VMEM:-8192}" \
    --ssh-port "${VM_SSH_PORT:-2222}" \
    ${VM_MGMT_MAC:+--mac "$VM_MGMT_MAC"} \
    --vfio-userdev "$socks" \
    ${VM_NVME:+--nvme "$VM_NVME"}
