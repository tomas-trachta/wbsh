x=; unset y
[[ -v x ]] && echo x-set
[[ -v y ]] || echo y-unset
a=(1); [[ -v a[0] ]] && echo elem-set
