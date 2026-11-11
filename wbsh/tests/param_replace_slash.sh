#!/bin/bash
# ${var/pattern/replacement}: the separating `/` is the first one that is
# not escaped, quoted or inside a nested expansion.

s=/usr/local/lib
echo "${s/\/usr/U}"
echo "${s//\//:}"
echo "${s/#\/usr/U}"
echo "${s/%\/lib/L}"
echo "${s/'/usr'/Q}"
echo "${s/"/local"/D}"
sep=/
echo "${s//${sep}/|}"
echo "${s/$(echo /usr)/C}"
echo "${s/\/usr\/local/X}"
a=(/a/b /c/d)
echo "${a[@]/\//-}"
