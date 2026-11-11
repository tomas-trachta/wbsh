#!/bin/bash
# find over a tree of a few thousand files, with name and type tests.
find t -name '*.c' | wc -l
find t -type d | wc -l
find t -name '*.txt' -path '*deeper*' | wc -l
