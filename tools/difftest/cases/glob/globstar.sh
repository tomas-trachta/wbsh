shopt -s globstar
mkdir -p t/a/b t/c && touch t/x.sh t/a/y.sh t/a/b/z.sh t/c/w.txt
echo t/**/*.sh
echo t/**
