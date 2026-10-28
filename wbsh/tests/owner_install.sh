#!/bin/bash
# chown, chgrp and install. Ownership only ever goes to the current user
# (who already owns everything this script creates), so nothing here can
# give a file away; failures are tested with accounts that do not exist.

D=_owner_install_tmp
rm -rf $D
mkdir -p $D
cd $D

me=$(id -un)
mine() { tr '[:upper:]' '[:lower:]' | sed "s/$(echo "$me" | tr '[:upper:]' '[:lower:]')/ME/g"; }
mtime() { stat -c %Y "$1"; }

echo hello > f
echo other > g
mkdir -p tree/sub
echo x > tree/sub/leaf

echo "=== chown to the current user succeeds and changes nothing more"
chown "$me" f; echo "status $?"
chown -v "$me" f | mine
chown -c "$me" f; echo "changes-only output above: none, status $?"
chown "$me": f; echo "login group status $?"
chown -v "$me:$me" f | mine
chown -v ":$me" f | mine
chown -v : f
chgrp "$me" f; echo "chgrp status $?"
chgrp -v "$me" f | mine
chown 1000 f; echo "numeric id of id -u: status $?"
chgrp "$(id -g)" f; echo "numeric group: status $?"
chown -R "$me:$me" tree; echo "recursive status $?"
chown -Rv "$me" tree | mine
chown --reference=f g; echo "reference status $?"
chown -v --reference=f g | mine
chgrp -v --reference=f g | mine
chown -v --from="$me" "$me" f | mine
chown -h "$me" f; echo "no-dereference status $?"

echo "=== chown and chgrp errors"
chown nosuch_wbsh_user_q7 f; echo "status $?"
chown :nosuch_wbsh_group_q7 f; echo "status $?"
chgrp nosuch_wbsh_group_q7 f; echo "status $?"
chown "$me" missing_file; echo "status $?"
chown -f "$me" missing_file; echo "silent status $?"
chown; echo "status $?"
chown somebody; echo "status $?"
chgrp; echo "status $?"
chown -R --dereference "$me" tree; echo "status $?"
chown --reference=missing_file f; echo "status $?"
chown -q "$me" f; echo "status $?"

echo "=== install SOURCE DEST"
printf 'payload\n' > src
install src dest; echo "status $?"
cmp src dest && echo same
install -v src dest
[ -w dest ] && echo writable
install -m 444 src ro; echo "status $?"
[ -w ro ] || echo read-only
install -m 0644 src ro; echo "replaces read-only: status $?"
[ -w ro ] && echo writable again
install -m u=r,go= src ro2; [ -w ro2 ] || echo "symbolic u=r is read-only"
install -m +x src ro3; [ -w ro3 ] || echo "symbolic +x is read-only"
install -m a=rw src rw; [ -w rw ] && echo "symbolic a=rw is writable"
install -m 9 src x; echo "status $?"
install -m u+q src x; echo "status $?"

echo "=== install into directories"
mkdir dir
install src g dir; echo "status $?"
ls dir
install -v -t dir src
install -T src dir; echo "status $?"
install src g dest; echo "status $?"
install src; echo "status $?"
install; echo "status $?"
install -t dir; echo "status $?"
install -t dir -T src; echo "status $?"

echo "=== install -d and -D"
install -d -v a/b/c
install -d a/b/c; echo "again status $?"
ls -d a/b/c
install -D -v src deep/er/file
cmp src deep/er/file && echo deep copy
install -D -v -t made/here src g
ls made/here
install -d -t dir x; echo "status $?"

echo "=== install source errors"
install missing_src x; echo "status $?"
install dir x; echo "status $?"
install src src; echo "status $?"
install src no/such/dir/x; echo "status $?"

echo "=== install backups"
printf 'one\n' > b1
printf 'two\n' > b2
printf 'three\n' > b3
install b1 target
install -b -v b2 target
cat target target~
install -S .bak b3 target
cat target.bak
rm -f target~ target.bak
install --backup=numbered b1 nt
install --backup=numbered b2 nt
install --backup=numbered -v b3 nt
cat nt.~1~ nt.~2~ nt
install --backup=existing -v b1 nt
VERSION_CONTROL=simple install -b b2 sv
VERSION_CONTROL=simple install -b -v b3 sv
SIMPLE_BACKUP_SUFFIX=.orig install -b -v b1 sv
install --backup=bogus b1 sv; echo "status $?"
install --backup=none -v b2 sv

echo "=== install -C and -p"
install src cmpd
install -C -v src cmpd; echo "unchanged: status $?"
printf 'changed\n' > src2
install -C -v src2 cmpd
touch -d @1000000000 old
install -p old kept
[ "$(mtime kept)" = 1000000000 ] && echo preserved
install old fresh
[ "$(mtime fresh)" != 1000000000 ] && echo "fresh time"

echo "=== install -o, -g and -s"
install -o "$me" -g "$me" src owned; echo "status $?"
install -o nosuch_wbsh_user_q7 src owned; echo "status $?"
install -g nosuch_wbsh_group_q7 src owned; echo "status $?"
PATH=/nonexistent install -s src stripped; echo "strip absent: status $?"
install -s --strip-program=nosuch_strip_q7 src stripped; echo "status $?"
install -c -Z src ignored; echo "ignored options: status $?"

cd ..
rm -rf $D
