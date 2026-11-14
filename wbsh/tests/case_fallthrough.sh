#!/bin/bash
# Probe: case terminators ;; ;& and ;;& as bash runs them.

echo "=== 1. ;& falls through every following arm until ;;"
case x in
    x) echo one ;&
    y) echo two ;&
    z) echo three ;;
    w) echo four ;;
esac

echo "=== 2. ;& into an empty arm and on"
case a in a) echo A ;& b) ;& c) echo C ;; d) echo D ;; esac

echo "=== 3. ;;& tests the patterns after it"
case x in
    x) echo first ;;&
    y) echo never ;;
    x) echo second ;&
    q) echo "fell into q" ;;&
    *) echo star ;;
esac

echo "=== 4. the last arm's ;& ends the case"
case a in a) echo last ;& esac
echo "status $?"

echo "=== 5. status of the last body run"
case a in a) false ;& b) ;; esac; echo "status $?"
case a in a) true ;& b) false ;; esac; echo "status $?"
