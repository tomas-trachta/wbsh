#!/bin/bash
# An expansion error in a case pattern ends the case command with status 1
# instead of going on to test the next pattern.

readonly xx=1
case 1 in $((xx++)) ) echo hi1 ;; *) echo hi2 ;; esac
echo ${xx}.$?
( case a in ${nosuch:?unset here}) echo one ;; *) echo two ;; esac ) 2>/dev/null
echo "after $?"
