#!/bin/bash
# Bundled tools answer the version probes scripts run, such as
# `tar --version | head -1` (#291). The number is wbsh's own.

ver() { sed 's/[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*$/X.Y.Z/'; }

tar --version | ver; echo "rc=$?"
wget --version | ver
wget -V | ver
unzip -v | ver
nohup --version | ver
setsid --version | ver
mkfifo --version | ver
mknod --version | ver
script --version | ver
