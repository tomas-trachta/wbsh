#!/bin/bash
# In a restricted shell, `.` refuses a file name with a slash, and
# `command -p`, `exec` with a command and `hash -p` with a path are
# refused; `set +r` cannot leave the mode.

exec 2>&1
set -r
. ./sub/file.sh
command -p date
exec date
hash -p /bin/sh sh
hash -p no-such-command-here a
BASH_CMDS[x]=/bin/sh
BASH_CMDS[y]=no-such-command-here
set +r
echo "still running: $?"
