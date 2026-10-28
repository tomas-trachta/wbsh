#!/bin/bash
# The suspend builtin: without job control it reports bash's error.

suspend; echo "status $?"
suspend -f; echo "status $?"
suspend -x; echo "status $?"
type -t suspend
