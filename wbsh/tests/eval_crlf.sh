#!/bin/bash
# eval reads CRLF text, as Windows programs print it, like LF text: the
# CR before each newline and a trailing one left by $( ) are ignored.
# The data itself keeps its CRs.

eval "$(printf 'echo one\r\nif true; then\r\n  echo two\r\nfi\r\n')"
eval "$(printf 'greet() {\r\n  echo three\r\n}\r\n')"
greet
eval "$(printf 'case x in\r\n  x) echo four ;;\r\nesac')"

data=$(printf 'a\r\nb')
case $data in
    *$'\r'*) echo "data keeps its CR" ;;
    *) echo "data lost its CR" ;;
esac
