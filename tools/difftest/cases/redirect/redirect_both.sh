{ echo out; echo err >&2; } &> both
cat both
{ echo more; echo moreerr >&2; } &>> both
cat both
