#!/bin/bash
# A standalone script that wbsh should run via ./hello.sh
echo "hello from script, args: $#"
echo "name: $0"
for arg in "$@"; do
  echo "  arg: $arg"
done
echo "cwd: $(pwd)"
LOCAL_VAR=should_not_leak
echo "inside, LOCAL_VAR=$LOCAL_VAR"
