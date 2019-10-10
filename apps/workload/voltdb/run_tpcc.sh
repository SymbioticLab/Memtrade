#!/bin/bash
voltdb_path="./tests/test_apps/tpcc/"

sync; echo 3 > /proc/sys/vm/drop_caches
ps -aux | grep voltdb | grep -v grep | awk '{ print $2 }' | xargs kill -9
sudo bash -c "echo never > /sys/kernel/mm/transparent_hugepage/enabled";
sudo bash -c "echo never > /sys/kernel/mm/transparent_hugepage/defrag";

cd $voltdb_path
./run.sh clean &&
./run.sh jars &&
./run.sh server &&
/bin/sleep 20 &&
#./run.sh init &&
../../../bin/sqlcmd < ddl.sql
/bin/sleep 5 &&
start_vdb=$(date +%s%N) && echo "voltdb client started at $start_vdb" &&
./run.sh client | tee xxx.txt
end_vdb=$(date +%s%N)&& echo "voltdb client ended at $end_vdb" && echo "voltdb client run time $((end_vdb-start_vdb)) ns"

