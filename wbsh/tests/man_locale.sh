#!/bin/bash
# man, apropos, whatis and locale. PATH is emptied around the lookups that
# could reach an external man, and every locale query names its locale
# (C, POSIX, C.UTF-8 or en_US.UTF-8, whose values Windows fixes), so the
# output does not depend on the machine's own settings.

unset LANG LANGUAGE LC_ALL LC_CTYPE LC_NUMERIC LC_TIME LC_COLLATE LC_MONETARY LC_MESSAGES
unset LC_PAPER LC_NAME LC_ADDRESS LC_TELEPHONE LC_MEASUREMENT LC_IDENTIFICATION
export MANWIDTH=72

echo "=== man pages"
man ls
man 1 cd
man -s 1 chown | head -4
man wbsh | head -9
man ls cat | grep -c '^NAME'
man bc | grep -c 'usage\|Usage'

echo "=== missing pages"
PATH=/nonexistent man no_such_page_q7; echo "status $?"
PATH=/nonexistent man 3 printf; echo "status $?"
PATH=/nonexistent man ls no_such_page_q7 | head -1; echo "status ${PIPESTATUS[0]}"
man; echo "status $?"

echo "=== apropos and whatis"
man -k 'compress or expand'
apropos '^sha[0-9]+sum'
man -f ls cat
whatis LS
whatis no_such_page_q7; echo "status $?"
apropos zzz_nothing_q7; echo "status $?"
apropos '('; echo "status $?"
apropos; echo "status $?"

echo "=== locale variables"
LC_ALL=C locale
LANG=C.UTF-8 LC_TIME=POSIX locale
LANG=en_US.UTF-8 LANGUAGE=en locale | head -3
LANG=xx_QQ.UTF-8 locale 2>&1 | head -4

echo "=== C keywords"
LC_ALL=C locale charmap codeset decimal_point thousands_sep grouping
LC_ALL=C locale -k LC_NUMERIC
LC_ALL=POSIX locale -ck d_t_fmt d_fmt t_fmt t_fmt_ampm am_pm
LC_ALL=C locale -k abday mon
LC_ALL=C locale -k LC_MONETARY
LC_ALL=C locale -k LC_MESSAGES LC_PAPER LC_MEASUREMENT
LC_ALL=C.UTF-8 locale charmap
LC_ALL=C locale -k no_such_keyword_q7; echo "status $?"

echo "=== en_US keywords"
LC_ALL=en_US.UTF-8 locale -k decimal_point thousands_sep grouping charmap
LC_ALL=en_US.UTF-8 locale -k d_fmt t_fmt am_pm abday first_weekday
LC_ALL=en_US.UTF-8 locale -k currency_symbol int_curr_symbol frac_digits p_cs_precedes
LC_ALL=en_US.UTF-8 locale -k measurement height width country_ab2 lang_ab
LANG=C LC_NUMERIC=en_US.UTF-8 locale thousands_sep decimal_point
LANG=en_US.UTF-8 LC_ALL=C locale thousands_sep

echo "=== locale -a and -m"
locale -a | head -3
locale -a | grep -c '^en_US\.UTF-8$'
locale -a | grep -c '^sr_RS\.UTF-8@latin$'
locale -m | grep -c '^UTF-8$'
locale -m | grep -c '^ISO-8859-1$'
locale -x; echo "status $?"
