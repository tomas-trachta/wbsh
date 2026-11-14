mkdir g && cd g && : > .hidden && : > shown
GLOBIGNORE=shown
echo *
GLOBIGNORE=
echo *
shopt -p dotglob
unset GLOBIGNORE
echo *
shopt -p dotglob
