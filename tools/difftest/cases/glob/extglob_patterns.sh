shopt -s extglob
mkdir g && cd g && touch a.c b.c a.h ab.txt abab.txt main.o
echo *.@(c|h)
echo !(*.c)
echo +(ab).txt
echo ?(a)b.txt
echo *(ab).txt
