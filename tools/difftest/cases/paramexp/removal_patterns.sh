s=aaa-bbb-aaa
echo "${s#a} ${s##a*-} ${s%a} ${s%%-*} ${s#[ab]} ${s%%[!a]*}"
