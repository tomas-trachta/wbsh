#!/bin/bash
# In an unquoted here-document a backslash-newline joins the next line, so
# that line cannot be the delimiter.

cat <<EOF2
line
next\
EOF2
moreline
EOF2
echo "after unquoted"

cat <<'EOF2'
quoted\
EOF2
echo "after quoted"
