#!/bin/sh
# vm-ssh: wrapper that runs ssh inside the qemu compose container.
# SLIRP hostfwd accepts TCP but may reset connections on first attempt;
# retry a few times to handle sshd startup timing on CI runners.
for attempt in 1 2 3 4 5; do
  docker exec -i vfio-user-rocjitsu-vm-qemu-1 \
    ssh -i /tmp/vm-id_rsa -o StrictHostKeyChecking=no \
        -o ConnectTimeout=10 "$@"
  rc=$?
  # Exit codes 255 = SSH connection error; retry those only
  [ $rc -ne 255 ] && exit $rc
  [ $attempt -lt 5 ] && sleep 3
done
exit 255
