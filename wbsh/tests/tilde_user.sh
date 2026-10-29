#!/bin/bash
# ~name expands to that user's home (profile) directory; anything that is
# not a user stays literal. The current user stands in for "some user",
# and the name is masked in the output so the golden fits any machine.

home=$(cd "$USERPROFILE" && pwd)
mask() {
	sed -e "s|$home|HOME|g" -e "s|$USERNAME|USER|g"
}

echo "=== users"
eval "echo ~$USERNAME" | mask
eval "echo ~$USERNAME/Desktop" | mask
upper=$(echo "$USERNAME" | tr 'a-z' 'A-Z')
eval "echo ~$upper" | mask
eval "x=~$USERNAME:~$USERNAME/bin; echo \$x" | mask
eval "cd ~$USERNAME && pwd" | mask

echo "=== stays literal"
echo ~no-such-user-wbsh ~no-such-user-wbsh/x
echo ~Administrators
echo ~2 ~+1 ~-3
u=$USERNAME
echo ~$u | mask
eval "echo '~$USERNAME' \"~$USERNAME\"" | mask

echo "=== completion"
__lineedit "echo ~$USERNAME" '<TAB>' | mask
__lineedit 'echo ~no-such-user-wbsh' '<TAB>'
