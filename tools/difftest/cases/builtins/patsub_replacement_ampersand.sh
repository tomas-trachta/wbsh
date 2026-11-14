# requires: bash>=5.2
x=hello
echo "${x/l/[&]}"
echo "${x//l/<\&>}"
r='&&'
echo "${x/e/$r}" "${x/e/"$r"}"
shopt -u patsub_replacement
echo "${x/l/[&]}"
