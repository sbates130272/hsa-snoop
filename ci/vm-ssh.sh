#!/bin/sh
# vm-ssh: wrapper that runs ssh inside the qemu compose container.
# SLIRP hostfwd accepts TCP but stalls the SSH key exchange from outside
# the container; running ssh from inside (127.0.0.1) works reliably.
exec docker exec -i vfio-user-rocjitsu-vm-qemu-1 \
  ssh -i /tmp/vm-id_rsa -o StrictHostKeyChecking=no "$@"
