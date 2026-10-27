#!/bin/bash
# Probe: backslash handling in heredoc bodies (bash 5.2 semantics).
v=val

echo "=== 1. unquoted body: backslash escapes only \$ \` \\ and newline"
cat <<E
\"quoted\" \'single\' \a \n \t
\$v \`echo no\` \\ $v
joined \
line
E

echo "=== 2. generated JSON / C keeps its escaped quotes"
cat <<E
{"name": "$v", "text": "say \"hi\""}
printf("%s\n", "\"$v\"");
E

echo "=== 3. <<- strips tabs, escapes unchanged"
cat <<-E
	\"tab\" \$v
	E

echo "=== 4. quoted delimiter: body verbatim"
cat <<'E'
\" \$v \\ \
E

echo "=== 5. heredoc inside command substitution"
x=$(cat <<E
\"in subst\" \$v
E
)
echo "$x"

echo "=== 6. double quotes still escape \" (unchanged)"
echo "a\"b" "${u:-\"}" ${u:-\"}
