eval 'false'; echo $?
eval ''; echo $?
eval 'exit_code() { return 4; }; exit_code'; echo $?
