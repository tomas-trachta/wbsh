a=(apple banana cherry)
echo "${a[@]/an/AN}" "|" "${a[@]//a/_}" "|" "${a[@]/#/<}" "|" "${a[@]/%/>}"
