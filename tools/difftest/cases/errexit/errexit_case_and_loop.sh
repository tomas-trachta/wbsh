set -e
for i in 1 2; do
    case $i in
        2) false ;;
    esac
    echo "iter $i"
done
echo end
