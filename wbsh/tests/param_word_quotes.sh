#!/bin/bash
# Quote handling in the word of ${name:+word}: inside double quotes a
# single quote is an ordinary character, and a nested "..." pair goes back
# to unquoted rules for backslashes.

v='&m'
printf '<%s>\n' "A ${v:+-D='$v'}" "B ${v:+-D=\'$v\'}" "C ${v:+-D="\'$v\'"}"
printf '<%s>\n' "D ${v:+"\p"}" "E ${v:+\p}" "F ${v:+"\\"}" "G ${v:+\'}" "H ${v:+"\'"}"
printf '<%s>\n' ${v:+-D='$v'} ${v:+-D=\'$v\'} ${v:+-D="\'$v\'"}
printf '<%s>\n' ${v:+"\p"} ${v:+\p} ${v:+\'} ${v:+"\'"}
printf '<%s>\n' "${u-'a'}" "${u:-"'b'"}" ${u:-"'c'"}
