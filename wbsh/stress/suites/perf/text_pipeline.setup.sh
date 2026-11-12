#!/bin/bash
seq 1 $((200000 * STRESS_N)) | awk '{ printf "%d,user%d,%s,%d\n", $1, $1 % 977, ($1 % 3 ? "ok" : "error"), ($1 * 7919) % 10007 }' > data.csv
