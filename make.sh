#!/bin/sh
set -e

# Build with the FH8862 cross compiler selected by Makefile.
make clean
make -j4

# Copy the application, LED module and test tool to the same NFS directory.
mkdir -p /mnt/nfs_share/Project
cp -f project_fh8862 /mnt/nfs_share/Project/
cp -f led_driver/stream_led.ko /mnt/nfs_share/Project/
cp -f led_driver/stream_led_test /mnt/nfs_share/Project/
cp -a driver /mnt/nfs_share/Project/
