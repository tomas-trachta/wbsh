nosuchcommand_xyz 2>/dev/null
echo "rc=$?"
./not-here.sh 2>/dev/null
echo "rc=$?"
