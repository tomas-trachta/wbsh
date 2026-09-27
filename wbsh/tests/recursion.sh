#!/bin/bash
# Probe: recursive functions, locals, return values via stdout, return
# codes, nested calls.

echo "=== 1. factorial via recursion (returns via stdout)"
factorial() {
    local n=$1
    if [ "$n" -le 1 ]; then
        echo 1
        return
    fi
    local sub=$(factorial $((n - 1)))
    echo $((n * sub))
}
for i in 0 1 2 3 5 7 10; do
    echo "  $i! = $(factorial $i)"
done

echo "=== 2. fibonacci (return value via stdout)"
fib() {
    local n=$1
    if [ "$n" -lt 2 ]; then echo "$n"; return; fi
    local a=$(fib $((n - 1)))
    local b=$(fib $((n - 2)))
    echo $((a + b))
}
for i in 0 1 5 10; do
    echo "  fib($i) = $(fib $i)"
done

echo "=== 3. recursion respects local vars"
deep() {
    local depth=$1
    local marker="d$depth"
    if [ "$depth" -ge 3 ]; then
        echo "bottom marker=$marker"
        return
    fi
    deep $((depth + 1))
    echo "after-recurse marker=$marker"
}
deep 0

echo "=== 4. return value (rc) plumbing"
chk() {
    local x=$1
    if [ "$x" = "good" ]; then return 0; fi
    if [ "$x" = "warn" ]; then return 2; fi
    return 1
}
for arg in good bad warn; do
    chk $arg; echo "  chk $arg -> $?"
done

echo "=== 5. mutual recursion"
is_even() { local n=$1; if [ "$n" -eq 0 ]; then return 0; fi; is_odd  $((n - 1)); }
is_odd () { local n=$1; if [ "$n" -eq 0 ]; then return 1; fi; is_even $((n - 1)); }
for n in 0 1 4 7; do
    if is_even $n; then echo "  $n is even"; else echo "  $n is odd"; fi
done

echo "=== done"
