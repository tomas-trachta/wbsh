#!/bin/bash
seq 1 $((50000 * STRESS_N)) | awk '{ print $1, "field-" $1 % 13, "some text to split" }' > lines.txt
