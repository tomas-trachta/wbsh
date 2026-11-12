#!/bin/bash
# Streaming text filters: memory must not follow the input size.
# @stress n=200 maxmem=200
seq 1 $(( STRESS_ITERS * 1000 )) | sed 's/1/one/g' | grep -v 7 | tr a-z A-Z | cut -c1-5 | awk '{ n += length($0) } END { print n }'
