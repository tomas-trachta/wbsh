mkdir g && cd g && touch a.c b.c a.o
GLOBIGNORE='*.o'
echo *
GLOBIGNORE='a*'
echo *
