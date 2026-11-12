#!/bin/bash
# Raw pipe throughput: 200 MB from /dev/zero through two stages.
head -c $((200 * 1024 * 1024 * STRESS_N)) /dev/zero | cat | wc -c
