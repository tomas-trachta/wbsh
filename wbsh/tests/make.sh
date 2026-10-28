#!/bin/bash
# make: variables and functions, conditionals, include (and remaking a
# missing one), explicit / pattern / static / double-colon / suffix / built-in
# rules, intermediates, vpath, target- and pattern-specific variables,
# recipe prefixes and errors, -n -s -k -i -q -t -B -W -o -e, recursion,
# SHELL / .SHELLFLAGS, .ONESHELL, .DELETE_ON_ERROR and Ctrl+C.

D=_make_tmp
rm -rf $D 2>/dev/null
mkdir -p $D
cd $D || exit 1
TOP=$PWD

run() { make "$@" 2>&1 | sed "s|$TOP|<top>|g"; echo "[${PIPESTATUS[0]}]"; }
fresh() { cd "$TOP"; rm -rf w; mkdir -p w; cd w; }

echo "=== basic build and up to date"
fresh
cat > Makefile <<'MK'
OBJS = a.o b.o
all: prog
	@echo done $(OBJS:.o=.c)
prog: $(OBJS)
	@echo link $^ into $@
	@touch $@
%.o: %.c
	echo compile $< to $@
	@touch $@
.PHONY: all
MK
touch -d 2001-01-01 a.c b.c
run
run prog
run -n all
touch -d 2001-06-01 a.o b.o prog
touch -d 2002-01-01 b.c
run -n
run nothere

echo "=== variable flavours"
fresh
cat > Makefile <<'MK'
A = 1
B := $(A) two
A = 3
C = $(A)
C += four
D ?= dee
D ?= no
E != echo shell out
F :::= $(A)$$x
G = a.c b.c c.h
CC ?= gcc
x:
	@echo 'B=$(B) C=$(C) D=$(D) E=$(E) F=$(value F) CC=$(CC)'
	@echo $(G:.c=.o) $(G:%.c=obj/%.o) ${G} $(words $(G))
	@echo $(flavor B) $(flavor C) $(flavor NOPE) $(origin A) $(origin HOME) $(origin CC) $(origin @)
define TWO
echo line1
echo line2
endef
y:
	@$(TWO)
override O = file
o:
	@echo $(O)
MK
run
run y
run o O=cmd
run x A=9
cat > Makefile <<'MK'
A = a
define B :=
$(A) b
endef
define B +=
more
endef
override define C
c
endef
D = d
undefine D
N != printf 'one\ntwo\n'
V = value # comment
H = a\#b
.RECIPEPREFIX = >
all:
> @echo '[$(B)] [$(C)] [$(origin C)] [$(origin D)] [$(N)] [$(V)] [$(H)]'
MK
run

echo "=== functions"
fresh
cat > Makefile <<'MK'
L = foo.c bar.c baz.h src/qux.c
rev = $(2) $(1)
sw = $(call rev,$(1),$(2))
all:
	@echo '$(subst .c,.o,$(L))'
	@echo '$(patsubst %.c,%.o,$(L))'
	@echo '[$(strip   a   b  )]' '$(findstring ar,bar)' '[$(findstring x,bar)]'
	@echo '$(filter %.c,$(L))' '|' '$(filter-out %.c,$(L))'
	@echo '$(sort c b a b)' '$(word 2,$(L))' '$(wordlist 2,3,$(L))' '$(firstword $(L))' '$(lastword $(L))'
	@echo '$(dir $(L))' '|' '$(notdir $(L))' '|' '$(suffix $(L))' '|' '$(basename $(L))'
	@echo '$(addsuffix .x,a b)' '$(addprefix p/,a b)' '$(join a b c,1 2)'
	@echo '$(if $(L),yes,no)' '$(if ,yes,no)' '[$(or ,,z)]' '[$(and a,b)]' '[$(and a,,b)]'
	@echo '$(foreach w,a b c,<$(w)>)' '$(call rev,x,y)' '$(call sw,1,2)'
	@echo '$(abspath /a/b/../c/./d)' '$(intcmp 1,2,lt,eq,gt)' '$(intcmp 3,3,lt,eq)' '$(let a b,1 2 3,$(b)-$(a))'
	@echo '$(shell echo hi; echo there)' '$(.SHELLSTATUS)' '$(notdir $(realpath .))'
	@echo '$(wildcard *.mk)' '[$(wildcard nope*)]' '$(value rev)'
	$(info info line)
	$(warning warn line)
MK
touch one.mk two.mk
run
cat > Makefile <<'MK'
define RULE
$(1):
	@echo rule for $(1)
endef
$(foreach t,x y,$(eval $(call RULE,$(t))))
$(file >list.txt,first)
$(file >>list.txt,second)
all: x y
	@echo $(words $(file <list.txt)) words, last $(lastword $(file <list.txt))
MK
run
run all
cat list.txt
printf 'all:\n\t@echo $(error boom here)\n' > Makefile
run
printf 'X = $(X)\nall: ; @echo $(X)\n' > Makefile
run
printf 'all: ; @echo $(word x,a b)\n' > Makefile
run

echo "=== syntax errors"
printf 'all\n' > Makefile
run
printf '        echo\n' > Makefile
run
printf '\techo early\n' > Makefile
run
printf 'ifeq (a,a)\nall:\n' > Makefile
run
printf 'endif\n' > Makefile
run
printf 'define X\nnever ends\n' > Makefile
run

echo "=== conditionals"
cat > Makefile <<'MK'
X = yes
EMPTY =
ifeq ($(X),yes)
R1 = a
else
R1 = b
endif
ifneq "$(X)" 'no'
R2 = c
endif
ifdef X
R3 = d
endif
ifdef EMPTY
R3 = wrong
endif
ifndef NOPE
R4 = e
endif
ifeq ($(X),no)
R5 = f
else ifeq ($(X),yes)
R5 = g
else
R5 = h
endif
ifeq (,$(filter x,a b))
  ifdef X
R6 = nested
  endif
endif
all:
	@echo $(R1) $(R2) $(R3) $(R4) $(R5) $(R6)
ifeq ($(X),yes)
	@echo inside the recipe
endif
MK
run

echo "=== include"
fresh
printf 'INC = from inc\n' > inc.mk
mkdir -p incdir
printf 'DIRINC = via -I\n' > incdir/found.mk
cat > Makefile <<'MK'
include inc.mk found.mk
-include missing.mk
sinclude also-missing.mk
all:
	@echo $(INC) $(DIRINC) [$(MAKEFILE_LIST)]
MK
run -I incdir
printf 'include nothere.mk\nall: ; @echo hi\n' > Makefile
run
cat > Makefile <<'MK'
include gen.mk
all: ; @echo $(GEN)
gen.mk:
	@echo generating
	@echo 'GEN = generated' > gen.mk
MK
run
run

echo "=== rule kinds"
fresh
cat > Makefile <<'MK'
all: log out/x.o out/y.o
log::
	@echo first
log::
	@echo second
out/%.o: %.c | out
	@echo build $@ from $< order $|
out:
	@echo mkdir $@
	@mkdir -p $@
OBJS = s1.o s2.o
$(OBJS): %.o: %.c
	@echo static $@ $< stem $*
st: $(OBJS)
grp: a.h a.c p.tab.c p.tab.h
a.h a.c &: ; @echo grouped once
%.tab.c %.tab.h: %.y
	@echo yacc $* makes $@
MK
touch x.c y.c s1.c s2.c p.y
run
run st
run grp
printf 'x: ; @echo one\nx: ; @echo two\n' > Makefile
run
printf 'a: b\n\t@echo a\nb: a\n\t@echo b\n' > Makefile
run
cat > Makefile <<'MK'
.DEFAULT:
	@echo default for $@
.PHONY: clean
all: thing
clean:
	@echo cleaning
MK
run
touch clean
run clean
printf 'all: ;\nnone:\n' > Makefile
run
run none
cat > Makefile <<'MK'
.DEFAULT_GOAL := second
first: ; @echo first [$(MAKECMDGOALS)]
second: ; @echo second [$(MAKECMDGOALS)]
MK
run
run first second

echo "=== automatic variables"
fresh
mkdir -p d
touch d/p1.c p2.h old.h
touch -d 2000-01-01 old.h
cat > Makefile <<'MK'
d/out.o: d/p1.c p2.h d/p1.c old.h | d
	@echo '@=$@ <=$< ^=$^ +=$+ |=$| *=$*'
	@echo 'D=$(@D) F=$(@F) <D=$(<D) <F=$(<F) ^F=$(^F)'
	@echo '?=$?'
MK
run
touch -d 2001-01-01 d/out.o p2.h d/p1.c d
touch -d 2003-01-01 p2.h
run

echo "=== implicit chains and built-in rules"
fresh
cat > Makefile <<'MK'
%.mid: %.src
	cp $< $@
%.out: %.mid
	cp $< $@
.SECONDARY: k.mid
MK
echo data > f.src
echo data > k.src
run f.out
run k.out
ls
run f.out
rm -f Makefile
printf 'int main(void) { return 0; }\n' > prog.c
run CC=echo prog
run -r prog
rm -f prog.c
cat > Makefile <<'MK'
CC = @echo cc
CFLAGS = -O2
main: main.o util.o
main.o util.o: defs.h
MK
touch main.c util.c defs.h
run
cat > Makefile <<'MK'
.SUFFIXES:
.SUFFIXES: .in .txt
.in.txt:
	@echo convert $< to $@ stem $*
	@cp $< $@
MK
echo x > doc.in
run doc.txt

echo "=== vpath"
fresh
mkdir -p src inc
touch src/v.c inc/h.h
cat > Makefile <<'MK'
VPATH = src
vpath %.h inc
v.o: v.c h.h
	@echo compile $< with $(filter %.h,$^)
MK
run

echo "=== target and pattern variables"
cat > Makefile <<'MK'
V = global
W = w
all: t1 t2 p.x
t1: V = t1val
t1: W += more
t1: dep
	@echo t1 $(V) $(W)
dep:
	@echo dep sees $(V)
t2:
	@echo t2 $(V)
%.x: V = pattern
p.x:
	@echo p.x $(V)
pr: P = parent
pr: private Q = hidden
pr: child
	@echo pr $(P) $(Q)
child:
	@echo child [$(P)] [$(Q)]
MK
run
run pr
run V=cmd

echo "=== recipe errors"
fresh
cat > Makefile <<'MK'
all: a b c
a:
	@echo a
	@exit 3
	@echo not reached
b:
	-@false
	@echo b after ignored
c:
	@echo c
MK
run
run -k
run -i
cat > Makefile <<'MK'
all: good bad other
good: ; @echo good
bad: missing ; @echo bad
other: ; @echo other
MK
run
run -k
cat > Makefile <<'MK'
.DELETE_ON_ERROR:
out.txt:
	@echo partial > $@
	@false
MK
run
test -e out.txt && echo kept || echo deleted
cat > Makefile <<'MK'
out: ; @echo partial > $@; sleep 5
MK
__ctrlc 1000
make 2>&1; echo "[$?]"
test -e out && echo kept || echo deleted

echo "=== question, touch, dry run, always"
fresh
cat > Makefile <<'MK'
t: s
	@echo build t
	cp s t
MK
echo src > s
touch -d 2001-01-01 s
run -q
run -n
run -t
run -q
run
touch -d 2002-01-01 s
touch -d 2001-06-01 t
run -s
run -B
run -W s -n
run -o s
touch -d '2001-01-01 00:00:00.2' t
touch -d '2001-01-01 00:00:00.7' s
run
touch -d '2001-01-01 00:00:00.9' t
run

echo "=== echo and silence"
cat > Makefile <<'MK'
all:
	echo one
	@echo two
	echo multi \
	  line
	+@echo plus
MK
run
run -s
printf '.SILENT:\nall:\n\techo quiet\n' > Makefile
run
cat > Makefile <<'MK'
.ONESHELL:
all:
	@x=5
	echo x is $$x
	cd ..
	pwd | sed 's|.*/||'
MK
run

echo "=== recursion and directories"
fresh
mkdir -p sub
cat > sub/Makefile <<'MK'
all:
	@echo sub level $(MAKELEVEL) flags '[$(MAKEFLAGS)]' V=$(V) EX=$$EX
MK
cat > Makefile <<'MK'
export EX = exported
all:
	$(MAKE) -C sub V=passed
	@$(MAKE) -C sub --no-print-directory
MK
run
run -s
run -n
run -C sub
run -C nodir
printf 'all: ; @echo from stdin\n' | make -f -; echo "[$?]"
printf 'all: ; @echo other $(MAKEFILE_LIST)\n' > other.mk
run -f other.mk
run -f nothere.mk
run --eval 'X = evaluated' -f /dev/null -E 'all: ; @echo $(X)'
rm -f Makefile other.mk
run

echo "=== misc options"
fresh
cat > Makefile <<'MK'
.NOTPARALLEL:
.INTERMEDIATE: mid
X = 1
all: mid
	@echo all '[$(MFLAGS)]'
mid:
	@echo making mid
	@touch mid
MK
run -j4
ls
run -j 2 -w -r
make -p -r -R -q 2>&1 | grep -E '^(X = |all: |mid:|# Files|# Variables|# Implicit)'
run --version
run --no-such-option

echo "=== environment and exports"
fresh
export FOO=envfoo
printf 'FOO = filefoo\nall: ; @echo $(FOO) $$FOO $(origin FOO)\n' > Makefile
run
run -e
printf 'unexport FOO\nall: ; @echo [$$FOO]\n' > Makefile
run
cat > Makefile <<'MK'
.EXPORT_ALL_VARIABLES:
MAKEFLAGS += --no-print-directory
ZED = zz
all: ; @echo $$ZED '[$(MAKEFLAGS)]'
MK
run -k
unset FOO

echo "=== shells"
printf 'SHELL = cmd\nall:\n\t@echo hello from cmd\n\techo %%OS%%\n' > Makefile
run
cat > Makefile <<'MK'
SHELL := /bin/bash
.SHELLFLAGS := -eu -o pipefail -c
all:
	@true | true; echo status $$?
	@false | true; echo not reached
MK
run

echo "=== second expansion"
fresh
cat > Makefile <<'MK'
.SECONDEXPANSION:
ONEVAR = onefile
TWOVAR = twofile
myfile: $(ONEVAR) $$(TWOVAR)
	@echo '$@: $^'
ONEVAR = wrong
TWOVAR = later
main_OBJS := main.o try.o
lib_OBJS := lib.o
main lib: $$($$@_OBJS)
	@echo '$@: $^'
foo: foo.1 bar.1 $$< $$^ $$+
	@echo 'foo: $+'
foo: foo.2 bar.2 $$< $$^ $$+
foo.out bar.out: %.out: $$*.in $$(addprefix d/,$$*.h) | $$@.dir
	@echo '$@: $^ order $|'
%.o: $$(addsuffix .c,$$*)
	@echo 'implicit $@ from $<'
onefile later foo.1 bar.1 foo.2 bar.2 foo.in bar.in d/foo.h d/bar.h foo.out.dir bar.out.dir: ; @:
MK
touch main.c try.c lib.c
run myfile
run main lib
run foo
run foo.out bar.out

echo "=== regressions"
fresh
mkdir -p sub
cat > sub/Makefile <<'MK'
all: ; @echo 'sub V=$(V) W=$(W) flags=[$(MAKEFLAGS)]'
MK
cat > Makefile <<'MK'
MAKEFLAGS += -s
all:
	@$(MAKE) -C sub
	@echo 'top flags [$(MAKEFLAGS)]'
MK
run 'V=p$$q' W=1
printf 'MAKEFLAGS += -- -k\nall: ; @echo "[$(MAKEFLAGS)]"\n' > Makefile
run W=1
MAKEFLAGS=' --jobserver-auth=fifo:/tmp/x -j4 --sync-mutex=fnm:x --shuffle=random' run
printf 't2: PATH := $(PATH)\nt2: ; @echo $${PATH:+has path}\n' > Makefile
run t2
export FOO=envfoo
printf '%%.x: FOO = pat\nt.x: ; @echo $$FOO\n' > Makefile
run t.x
unset FOO
printf 'all: a b\na: ; -sh -c "exit 130"\nb: ; @echo b ran\n' > Makefile
run
printf 'all: a b\na: ; sh -c "exit 130"\nb: ; @echo b ran\n' > Makefile
run -k
printf 'all: ; @echo old\nMakefile: Makefile.in ; cp Makefile.in Makefile\n' > Makefile
printf 'all: ; @echo new\n' > Makefile.in
touch -d 2000-01-01 Makefile
run
run
rm -f Makefile.in
printf 'a: ;X=1 echo hi\nv: W = x;y\nv: ; @echo "$(W)"\n' > Makefile
run a
run v
printf 'all: dep ; @echo [$(UNDEF)]\ndep: ; @:\n' > Makefile
run --warn-undefined-variables
printf '$(info sub read with [$(MAKEFLAGS)])\nall: ; @echo sub recipe\n' > sub/Makefile
printf 'all: ; +@$(MAKE) -C sub --no-print-directory\n' > Makefile
run -q
cat > Makefile <<'MK'
.DELETE_ON_ERROR:
d: ; mkdir d; false
X = [$$(x # comment
show: ; @echo '$(value X)'
MK
run d
test -d d && echo directory kept
run show

cd "$TOP/.."
rm -rf $D
