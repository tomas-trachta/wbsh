#!/bin/bash
# Inside "...", a backquoted command takes \" as a plain double quote.

echo "`echo \"Hello world!\"`"
echo "[`echo \"a  b\"`]"
echo `echo \"bare\"`
echo "`echo \$HOME | tr -d /`" | grep -c . >/dev/null && echo "dollar escape kept"
echo "`echo \\`"
