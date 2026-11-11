#!/bin/bash
# gzip both ways over ~10 MB, streamed through pipes.
gzip -c big.txt | gzip -dc | wc -c
