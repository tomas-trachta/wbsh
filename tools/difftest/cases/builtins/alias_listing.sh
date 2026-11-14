match() { local re=$1 l; while IFS= read -r l; do [[ $l =~ $re ]] && printf '%s\n' "$l"; done; }
shopt -s expand_aliases
alias a1='echo one' a2="echo 'two'"
alias a1
alias -p | match '^alias a[12]='
unalias a1
alias a1 2>&1
echo "rc=$?"
