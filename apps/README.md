# Content
* [Workload](#workload)
-- [VoltDB](#voltdb)
-- [MemCached](#memcached)
-- [Redis](#redis)
-- [Spark](#spark)
-- [PowerGraph](#powergraph)
-- [TuriCreate](#turicreate) ([Graph Algorithms](#graph-algorithms), [Image Classification](#image-classifications))
-- [Metis](#metis)
* [CloudLab Configuration](#cloudlab-configuration)
* [Miscellaneous](#miscellaneous)
-- [Call Graph Latency Breakdown](#call-graph-latency-breakdown)

## Workload
## Voltdb
- To build VoltDB, you need OpenJDK, so [install](https://stackoverflow.com/questions/14788345/how-to-install-the-jdk-on-ubuntu-linux) it if you don?t have it.
    ```sh
    sudo add-apt-repository ppa:openjdk-r/ppa  
    sudo apt update
    sudo apt install -y openjdk-8-jdk
    export JAVA_HOME=/usr/lib/jvm/java-8-openjdk #replace by your installation path
    export PATH=$PATH:$JAVA_HOME/bin
    java -version
    ```
- Clone the VoltDB [repo](https://github.com/hasan3050/voltdb_tpcc)
    ```sh
    git clone https://github.com/hasan3050/voltdb_tpcc.git
    ```
- [Build VoltDB](https://github.com/VoltDB/voltdb/wiki/Building-VoltDB)
    ```sh
    sudo apt-get -y install ant build-essential ant-optional default-jdk python cmake valgrind ntp ccache git-arch git-completion git-core git-svn git-doc git-email python-httplib2 python-setuptools python-dev apt-show-versions
    cd voltdb
    ant clean
    ```

    Now, we are ready to run TPCC (client) on a VoltDB server. Before that, disable transparent huge page 
    ```sh
    echo never > /sys/kernel/mm/transparent_hugepage/enabled
    echo never > /sys/kernel/mm/transparent_hugepage/defrag
    ```

- To run tpcc, 
    ```sh
    # to run without memory limit
    ./run_tpcc.sh
    
    # to run within a limited amount of memory
    cgcreate -g memory:voltdb
    echo 5G > /sys/fs/cgroup/memory/voltdb/memory.limit_in_bytes
    cgexec -g memory:voltdb ./run_tpcc.sh
    ```
## Memcached

- Install MemCached
    ```sh
    wget https://launchpad.net/libmemcached/1.0/1.0.18/+download/libmemcached-1.0.18.tar.gz
    tar -zxvf libmemcached-1.0.18.tar.gz
    cd libmemcached-1.0.18
    sudo apt-get update
    sudo apt-get install -y memcached
    ./configure --enable-memaslap | grep libevent
    
    # in Makefile, modify "LIBS =" to "LIBS = -lpthread"
    make | grep error
    sudo make install
    sudo ldconfig
    
    # modify max memory from default 64M to a higher value
    sudo updatedb
    locate memcached.conf
    vim /etc/memcached.conf # path to memcached.conf; set the max memory to a higher value: -m 640000
    ```
- Run MemCached
    ```sh
    cgcreate -g memory:memcached
    echo 4465M > /sys/fs/cgroup/memory/memcached/memory.limit_in_bytes
    /etc/init.d/memcached stop
    cgexec -g memory:memcached /etc/init.d/memcached start
    cd ./apps/workload/memaslap/
    
    #to run ETC workload
    ./run_ETC.sh
    
    #to run SYS workload
    ./run_SYS.sh
    ```
## Redis
- Install Redis 
    ```sh
    git clone https://github.com/antirez/redis.git
    cd redis 
    make distclean # important! 
    make 
    make test 
    ```
- Configure Redis:
-- Edit `/etc/sysctl.conf` and add `vm.overcommit_memory=1`
-- Then reboot or run the command `sysctl vm.overcommit_memory=1` for this to take effect
-- Disable transparent hugepage: 
    ```sh
    echo never > /sys/kernel/mm/transparent_hugepage/enabled
    echo never > /sys/kernel/mm/transparent_hugepage/defrag
    ```
    You can add this to `/etc/rc.local` to retain the setting after a reboot
    -- Edit `./redis/redis.conf` to not save `*.rdb` or `*.aof` file
   - make sure `appendonly` is `no` 
   - remove or comment:
	`save 900 1`
	 `save 300 10`
	 `save 60 10000`
    - add `save ""`; otherwise redis will store the in-memory data to a `.rdb` file at certain interval or after exceeding certain memory limit

- Install Memtier workload
    ```sh
    git clone https://github.com/RedisLabs/memtier_benchmark.git
    cd memtier_benchmark
    autoreconf -ivf
    ./configure
    make 
    make install 
    ```
- Run Redis:
    ```sh
    # to run without memory limit
    ~/redis/src/redis-server ~/redis/redis.conf
    
    # to run within a limited amount of memory
    cgcreate -g memory:redis
    echo 5G > /sys/fs/cgroup/memory/redis/memory.limit_in_bytes
    cgexec -g memory:redis ./redis/src/redis-server ./redis/redis.conf
    ```

- Run memtier:
    ```sh
    # to run with random key pattern
    ./memtier_benchmark -t 10 -n 400000 --ratio 1:1 -c 20 -x 1 --key-pattern R:R --hide-histogram --distinct-client-seed -d 300 --pipeline=1000
    
    # to run with sequential key pattern
    ./memtier_benchmark -t 10 -n 400000 --ratio 1:1 -c 20 -x 1 --key-pattern S:S --hide-histogram --distinct-client-seed -d 300 --pipeline=1000
    ```
    Here, `-x` changes run count; add `--out-file=FILE` to the command to specify output file 

## Spark
- Install Spark (to build Spark, you need JDK8; see how to install OpenJDK [here](#voltdb))
    ```sh
    git clone https://github.com/yiwenzhang92/spark.git
    cd spark
    git fetch && git checkout spark-mod
    build/mvn -DskipTests clean package
    ```
- Configure PageRank workload
    ```sh
    pip install gdown
    cd ./apps/workload/
    gdown https://drive.google.com/uc?id=1pcE_QpfPym5Nko32kC3yWhaD3amoINy9
    unzip twitter-graph.zip
    mkdir /home/ubuntu
    cp ./apps/workload/twitter-graph/edgein.txt /home/ubuntu
    ```
- Run GraphX
    ```sh
    sync; echo 3 > /proc/sys/vm/drop_caches
    cgcreate -g memory:graphx
    echo 11510M > /sys/fs/cgroup/memory/graphx/memory.limit_in_bytes
    cd ./spark
    rm -r /home/ubuntu/out_spark
    cgexec -g memory:graphx bin/spark-submit run-example --master local[32] --driver-memory 12G --conf "spark.network.timeout=1200" SparkPageRank 10
    ```
## Powergraph
- Install PowerGraph
    ```sh
    sudo apt-get update
    sudo apt-get install -y gcc g++ build-essential libopenmpi-dev r-base-core openmpi-bin default-jdk cmake zlib1g-dev git
    git clone https://github.com/hasan3050/PowerGraph.git
    cd PowerGraph
    ./configure
    cd release/toolkits/graph_analytics
    make -j32
    ```
    
    Note: If you get `error: null argument where non-null required (argument 1)` while compiling zookeeper comment out all `fprintf` in `./PowerGraph/deps/zookeeper/src/zookeeper/src/c/src/zookeeper.c`
- Run TunkRank
-- Download the `twitter-graph.zip` as mentioned [here](#spark)
    ```sh
    cgcreate -g memory:powergraph
    echo 4721M > /sys/fs/cgroup/memory/powergraph/memory.limit_in_bytes
    cgexec -g memory:powergraph ./PowerGraph/release/toolkits/graph_analytics/tunkrank --graph=./apps/workload/twitter-graph/edgein.txt --format=tsv --ncpus=2 --engine=asynchronous
    ```
## Turicreate
- Install [TuriCreate](https://github.com/apple/turicreate)
    ```sh
    pip install virtualenv
    cd ~
    virtualenv venv
    source ~/venv/bin/activate
    pip install -U turicreate
    ```
#### Graph Algorithms
- To run graph analytics on TuriCreate
    ```sh
    cd apps/workload/turicreate
    cgexec -g memory:graph_analytics
    echo 2G > /sys/fs/cgroup/memory/graph_analytics/memory.limit_in_bytes
    cgexec -g memory:graph_analytics python graph_analytics.py -g <twitter/wiki> -a <pagerank/connectedcomp/labelprop/graphcol> -t 32
    ```
#### Image Classification
- Download and extract dataset from [here](https://www.microsoft.com/en-us/download/details.aspx?id=54765) (large dataset)
- Open image_classif_create.py and change home to your dataset 
- Run `python image_classif_create.py` one time only to create the train and test data
- Run `python image_classif_model.py` (Takes a few hours)
- Run `python image_classif_predict.py` with cgroup memory limitation (Takes ~15 minutes without memory limit)
- Run `python image_classif_evaluate.py` with cgroup memory limitation (Takes ~15 minutes without memory limit)

## Cloudlab Configuration
- Add 1TB Disk to the machine
    ```sh
    sudo mkdir /somedir
    sudo /usr/local/etc/emulab/mkextrafs.pl /somedir
    ```
    If you get a strange GPT error, do this:
    ```sh
    sudo apt-get install gdisk
    sudo sgdisk --zap /dev/sda
    sudo /usr/local/etc/emulab/mkextrafs.pl /somedir
    ```
    
## Miscellaneous
#### Call Graph Latency Breakdown
* Use [ftrace](https://jvns.ca/blog/2017/03/19/getting-started-with-ftrace/) to trace a function call graph ([documentation](https://www.kernel.org/doc/Documentation/trace/ftrace.txt))
    ```sh
    apt-get install -y trace-cmd
    cd /sys/kernel/debug/tracing
    cat /dev/null >  trace
    echo 10 > max_graph_depth
    echo function_graph > current_tracer
    echo funcgraph-tail > trace_options
    echo 1 > tracing_on
    
    trace-cmd record -p function_graph -g <function_name> -P <pid> 
    #example: trace-cmd record -p function_graph -g __do_page_fault -P 90326
    ```
* Use `./apps/scripts/parse_ftrace.py` to generate CDF from the `ftrace` record
    ```sh
    python ./apps/scripts/parse_ftrace.py -file <path/to/ftrace/record> -func <function_to_parse> -count <number_of_events>
    ```
    Default function is `__do_page_fault()` and CDF is generated out of first 100000 events.