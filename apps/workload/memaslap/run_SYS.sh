# script for running memaslap on memcached
memaslap -s 127.0.0.1:11211 -F config_SYS_1.cnf -x 10000000 -S 10s
memaslap -s 127.0.0.1:11211 -F config_SYS_2.cnf -x 10000000 -S 10s