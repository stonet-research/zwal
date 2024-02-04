#!/bin/bash
# Change "image"

qemu -name qemuzns                                                  \
	--enable-kvm                                                    \
	-vnc :0                                                         \
	-m 124G -cpu host -smp 20                                       \
	-hda "$image"                                                   \
	-net user,hostfwd=tcp::7777-:22 -net nic                          \
	-device femu,devsz_mb=$((8192*12)),id=nvme7,femu_mode=3,queues=64 

