#!/bin/bash
# `!` before the end of a command negates an empty one, and `! !` cancels.

!; echo "bare: $?"
! !; echo "double: $?"
! ! true; echo "double true: $?"
! ! false; echo "double false: $?"
!
echo "own line: $?"
