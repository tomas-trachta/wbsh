x="outer $(echo "inner \"quoted\" $(echo 'deep')")"
echo "$x"
