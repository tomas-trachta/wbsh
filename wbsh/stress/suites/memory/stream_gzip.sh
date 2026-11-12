#!/bin/bash
# Streaming: 10x more data must not need more memory.
# @stress n=100 maxmem=200
head -c $(( STRESS_ITERS * 200000 )) /dev/zero | gzip -1 | gzip -dc | wc -c
