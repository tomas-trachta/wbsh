#!/bin/bash
# Archive members stay inside the extraction directory (#268). unzip strips
# rooted and drive prefixes with a warning and skips names that climb out
# through `..`; tar and cpio refuse `..` names and create links that could
# lead outside only after every other member. Everything happens inside a
# fresh temporary directory.

D=$(mktemp -d) || exit 1
cd "$D" || exit 1

u16() {
    printf "\\$(printf %03o $(( $1 & 255 )))\\$(printf %03o $(( ($1 >> 8) & 255 )))"
}

u32() {
    u16 $(( $1 & 65535 ))
    u16 $(( ($1 >> 16) & 65535 ))
}

# A stored entry: the name is written verbatim, whatever it contains.
local_entry() {
    printf 'PK\003\004'; u16 20; u16 0; u16 0; u32 0; u32 0
    u32 ${#2}; u32 ${#2}; u16 ${#1}; u16 0
    printf '%s%s' "$1" "$2"
}

central_entry() {
    printf 'PK\001\002'; u16 20; u16 20; u16 0; u16 0; u32 0; u32 0
    u32 ${#2}; u32 ${#2}; u16 ${#1}; u16 0; u16 0; u16 0; u16 0; u32 0; u32 $3
    printf '%s' "$1"
}

end_record() {
    printf 'PK\005\006'; u16 0; u16 0; u16 $1; u16 $1; u32 $3; u32 $2; u16 0
}

# mkzip ARCHIVE NAME DATA [NAME DATA]...
mkzip() {
    local out entries offsets k start
    out=$1
    shift
    entries=("$@")
    offsets=()
    : > "$out"
    for ((k = 0; k < ${#entries[@]}; k += 2)); do
        offsets+=($(wc -c < "$out"))
        local_entry "${entries[k]}" "${entries[k+1]}" >> "$out"
    done

    start=$(wc -c < "$out")
    for ((k = 0; k < ${#entries[@]}; k += 2)); do
        central_entry "${entries[k]}" "${entries[k+1]}" "${offsets[k/2]}" >> "$out"
    done

    end_record $(( ${#entries[@]} / 2 )) $start $(( $(wc -c < "$out") - start )) >> "$out"
}

tree() {
    find "$@" -type f | sort
}

echo "=== unzip"
mkzip evil.zip \
    good.txt fine \
    sub/inner.txt inner \
    ../up.txt UP \
    sub/../../up2.txt UP2 \
    '..\back.txt' BACK \
    '.. /dots.txt' DOTS \
    /wbsh-zipslip-rooted.txt ROOTED \
    'C:\wbsh-zipslip-drive.txt' DRIVE \
    'C:wbsh-zipslip-relative.txt' RELATIVE \
    '\\wbsh-zipslip-server\share\unc.txt' UNC
mkdir -p a/dest
( cd a/dest && unzip ../../evil.zip > ../out.txt 2> ../err.txt ); echo "status $?"
cat a/out.txt
cat a/err.txt
tree a/dest
cat a/dest/wbsh-zipslip-rooted.txt; echo
[ -e up.txt ] || [ -e a/up.txt ] || [ -e a/up2.txt ] || [ -e a/back.txt ] || echo "nothing above dest"
[ -e /wbsh-zipslip-rooted.txt ] || [ -e /c/wbsh-zipslip-drive.txt ] || echo "nothing at a root"

echo "=== unzip -d"
unzip -d b/dest evil.zip > /dev/null 2>&1; echo "status $?"
tree b

echo "=== unzip of a clean archive still exits 0"
mkzip clean.zip one.txt 1 dir/two.txt 2
unzip -d c clean.zip; echo "status $?"

echo "=== tar refuses .. members"
mkdir -p t/src t/x/y
echo secret > t/src/f
( cd t/x && tar -cPf ../../evil.tar ../src/f )
tar -tf evil.tar
( cd t/x/y && tar -xf ../../../evil.tar ); echo "status $?"
tree t/x

echo "=== tar makes an escaping link only after the other members"
mkdir -p l/mk l/x/y
echo "outside" > l/esc.txt
ln -s .. l/mk/link
( cd l/mk && tar -cf ../../link.tar link link/esc.txt )
tar -tvf link.tar | sed 's/ .*:[0-9][0-9] / /'
( cd l/x/y && tar -xf ../../../link.tar 2> /dev/null ); echo "status $?"
[ -e l/x/esc.txt ] && echo "ESCAPED" || echo "nothing written through the link"
tree l/x

echo "=== cpio refuses .. names"
mkdir -p p/x/y
echo "up" > p/x/up.txt
( cd p/x/y && echo ../up.txt | cpio -o -H newc > ../../evil.cpio 2> /dev/null )
rm p/x/up.txt
( cd p/x/y && cpio -i -d < ../../evil.cpio ); echo "status $?"
tree p/x

echo "=== cpio makes an escaping link only after the other entries"
mkdir -p q/mk q/x/y
echo "outside" > q/esc.txt
ln -s .. q/mk/link
( cd q/mk && printf 'link\nlink/esc.txt\n' | cpio -o -H newc > ../../link.cpio 2> /dev/null )
( cd q/x/y && cpio -i -d < ../../../link.cpio 2> /dev/null ); echo "status $?"
[ -e q/x/esc.txt ] && echo "ESCAPED" || echo "nothing written through the link"
tree q/x

cd /
rm -rf "$D"
