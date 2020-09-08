# Content
* [Workload](#workload)
    * [VoltDB](#voltdb)
    * [MemCached](#memcached)
    * [Redis](#redis)
    * [RocksDB](#rocksdb)
    * [Spark](#spark)
    * [PowerGraph](#powergraph)
    * [TuriCreate](#turicreate) ([Graph Algorithms](#graph-algorithms), [Image Classification](#image-classifications))
    * [Metis](#metis)
    * [PARSEC](#parsec)
    * [CloudSuite](#cloudsuite)
    * [YCSB](#YCSB)
    * [Yahoo Streaming Benchmark](#Yahoo-Streaming-Benchmark)
    * [TensorFlow](#TensorFlow)
    * [Snowset](#Snowset)
* [CloudLab Configuration](#cloudlab-configuration)
    * [KVM Management](#kvm-management)
    * [Add 1TB Disk](#add-1tb-disk) 
    * [Setup Ramdisk](#setup-ramdisk)
* [Miscellaneous](#miscellaneous)
    * [Call Graph Latency Breakdown](#call-graph-latency-breakdown)
    * [Compile Linux from Source Code](#compile-linux-from-source-code)
    * [Upgrade Kernel to 4.11](#upgrade-kernel-to-4.11)

## Workload

| Use Case | Application |
| ------------- |:-------------:|
| Data Storage  | [RocksDB](#rocksdb); Memtier on [Redis](#redis); FB workload on [MemCahed](#memcached); TPC-C on [VoltDB](#voltdb); [YCSB](#YCSB) on Redis/Memcached/RocksDB; [Snowset](#Snowset) on Redis/Memcached/RocksDB |
| Graph Algorithms  | TunkRank on [PowerGraph](#powergraph); PageRank, Connected Component, Label Propagation, Graph Coloring on [TuriCreate](#turicreate) |
| Machine Learning | [Image Classification](#image-classifications) on TuriCreate; [Movie Recommendation](#movie-recommendation) on Spark; Image Classification on [TensorFlow](#TensorFlow) |
| Parallel Programming | [PARSEC](#parsec) with x264 and canneal benchmark; [Metis](#metis) with Linear Regression |
| Web Service | Olio (social-events), media streaming on [CloudSuite](#cloudsuite) |
| Data Streaming | [Yahoo Streaming Benchmark](#Yahoo-Streaming-Benchmark) on Storm |

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
- Clone the VoltDB [repo](https://github.com/VoltDB/voltdb)
    ```sh
    git clone https://github.com/VoltDB/voltdb.git
    cp ./apps/workload/voltdb/*.java ./voltdb/tests/test_apps/tpcc/client/com/
    cp ./apps/workload/voltdb/run.sh ./voltdb/tests/test_apps/tpcc/
    cp ./apps/workload/voltdb/run_tpcc.sh ./voltdb
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
    
    # in Makefile, modify "LIBS =" to "LIBS = -lpthread", and add "-fpermissive" to "CXXFLAGS"
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
    # limit the memory of memcached in 14.04
    sudo cgcreate -g memory:memcached
    sudo bash -c "echo 4465M > /sys/fs/cgroup/memory/memcached/memory.limit_in_bytes"
    sudo /etc/init.d/memcached stop
    sudo cgexec -g memory:memcached /etc/init.d/memcached start
    
    #limit the memory of memcached in 16.04 or higher
    sudo /etc/init.d/memcached stop
    sudo /etc/init.d/memcached start
    sudo systemctl set-property memcached.service MemoryLimit=4465M
    
    cd ./apps/workload/memaslap/
    
    #to run ETC workload
    ./run_ETC.sh
    
    #to run SYS workload
    ./run_SYS.sh
    ```
## Redis
- Install Redis 
    ```sh
    wget https://github.com/antirez/redis/archive/6.0.3.zip
    unzip 6.0.3.zip
    mv redis-6.0.3 redis
    cd redis 
    make distclean # important! 
    make 
    make test 
    ```
- Configure Redis:
    - Edit `/etc/sysctl.conf` and add `vm.overcommit_memory=1`
    - Then reboot or run the command `sysctl vm.overcommit_memory=1` for this to take effect
    - Disable transparent hugepage: 
        ```sh
        echo never > /sys/kernel/mm/transparent_hugepage/enabled
        echo never > /sys/kernel/mm/transparent_hugepage/defrag
        ```
        You can add this to `/etc/rc.local` to retain the setting after a reboot
    - Edit `./redis/redis.conf` to not save `*.rdb` or `*.aof` file
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

## RocksDB

- Install RocksDB
    ```sh
    sudo apt-get install libgflags-dev libsnappy-dev zlib1g-dev libbz2-dev libzstd-dev # dependency packages, libzstd-dev is available for xenial or later distributions
    git clone https://github.com/facebook/rocksdb.git
    cd rocksdb/
    git checkout tags/v6.8.1
    make all -j32
    ```
- Run [Bulk Load](https://github.com/facebook/rocksdb/wiki/Performance-Benchmarks#test-1-bulk-load-of-keys-in-random-order) benchmark
    - [Add 1TB Disk](#add-1tb-disk)
    ```sh
    export DB_DIR=/newdir/db # mkdir if /newdir/db does not exist
    export WAL_DIR=/newdir/wal
    export TEMP=/newdir/tmp
    export OUTPUT_DIR=/newdir/output
    
    ./tools/benchmark.sh bulkload
    ```
- Run [Read While Writing](https://github.com/facebook/rocksdb/wiki/Performance-Benchmarks#test-5-multi-threaded-read-and-single-threaded-write) benchmark
    - Run Bulk Load benchmark ti populate the database
    - `NUM_KEYS=10000000 NUM_THREADS=32 tools/benchmark.sh readwhilewriting` to run 10 Million reads with 32 concurrent threads
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
    
    #### Movie Recommendation
    The workload is a ML application (Alternating Least Square Matrix Factorization) over the [MovieLens full dataset](https://grouplens.org/datasets/movielens/latest/) (27M ratings, 58K movies, 280K users)
    ```sh
    #get the dataset
    wget http://files.grouplens.org/datasets/movielens/ml-latest.zip
    unzip ml-latest.zip
    
    spark-submit --master local[16] --driver-memory 12G --executor-memory 12G apps/workload/spark/als_recommender.py --path /paath/to/ml-latest --movie_name "Iron Man" --top_n 100
    ```
    The ALS MF code was taken from this [repo](https://github.com/KevinLiao159/MyDataSciencePortfolio/tree/master/movie_recommender).
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
    Download the `twitter-graph.zip` as mentioned [here](#spark)
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
    To run graph analytics on TuriCreate:
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
## Metis 
- Install [Metis](https://github.com/ydmao/Metis)
- Build and run linear regression
    ```sh
    cd Metis/data_tool
    mkdir data
    chmod +x data-gen.sh 
    ./data-gen.sh
    cd .. & obj/linear_regression ./data_tool/data/lr_4GB.txt
    ```
## PARSEC
 PARSEC is a collection of parallel programs which are used for performance studies of multiprocessor machines. I did not run this benchmark yet. We can follow this [repo](https://github.com/bamos/parsec-benchmark) to run PARSEC benchmarks.
## CloudSuite

* First, setup Cloud Suite Server inside the VM:

  ```bash
  # Install docker
  sudo apt-get install -y docker.io
  
  # Run MySQL + Memcached + WebServer
  WEB_SERVER_IP=$( ifconfig  | grep 'inet '| grep -v '127.0.0.1' | grep -v '192.168.123.1' | cut -d ' ' -f 10 | tail -1)
  DATABASE_SERVER_IP=$( ifconfig  | grep 'inet '| grep -v '127.0.0.1' | grep -v '192.168.123.1' | cut -d ' ' -f 10 | tail -1)
  MEMCACHED_SERVER_IP=$( ifconfig  | grep 'inet '| grep -v '127.0.0.1' | grep -v '192.168.123.1' | cut -d ' ' -f 10 | tail -1)
  sudo cgcreate -g memory:app
  sudo docker run -dt --net=host --name=mysql_server --cgroup-parent=/app/ cloudsuite/web-serving:db_server ${WEB_SERVER_IP}
  sudo docker run -dt --net=host --name=memcache_server --cgroup-parent=/app/ cloudsuite/web-serving:memcached_server
  sudo docker run -dt --net=host --name=web_server --cgroup-parent=/app/ cloudsuite/web-serving:web_server /etc/bootstrap.sh ${DATABASE_SERVER_IP} ${MEMCACHED_SERVER_IP}
  ```

* Then, install and run Cloud Suite Client in the hypervisor:

  ```bash
  # Install docker
  sudo apt-get install -y docker.io
  
  # Run Cloud Suite Client
  WEB_SERVER_IP=$(sudo uvt-kvm ip vm1)
  sudo docker run --net=host --name=faban_client why950708/cloudsuite:faban_client ${WEB_SERVER_IP}
  
  # When 1000 users have been generated, the client start to send requests to the server
  ```

## YCSB

* Install JDK 1.8

* Install Maven 3.6:

  ```bash
  cd /opt
  sudo wget https://archive.apache.org/dist/maven/maven-3/3.6.0/binaries/apache-maven-3.6.0-bin.tar.gz
  sudo tar -xvzf apache-maven-3.6.0-bin.tar.gz
  sudo mv apache-maven-3.6.0 maven
  ```

  Add the following lines to `/etc/profile.d/mavenenv.sh`:

  ```bash
  export M2_HOME=/opt/maven
  export PATH=${M2_HOME}/bin:${PATH}
  ```

  Then run:

  ```bash
  sudo chmod +x /etc/profile.d/mavenenv.sh
  source /etc/profile.d/mavenenv.sh
  ```

* Install YCSB:

  ```bash
  git clone https://github.com/yuhong-zhong/YCSB.git
  cd YCSB
  git checkout spot_memory
  
  # build with Redis binding
  mvn -pl site.ycsb:redis-binding -am clean package
  # build with Memcached binding
  mvn -pl site.ycsb:memcached-binding -am clean package
  # build with RocksDB binding
  mvn -pl site.ycsb:rocksdb-binding -am clean package
  ```

* Run Zipfian distributed workload on Redis/Memcached/RocksDB:

  Save the following lines as a file which located at `YCSB/workloads/workload_zipfian`:

  ```bash
  recordcount=1000000
  operationcount=200000000
  fieldcount=1
  fieldlength=4096
  workload=site.ycsb.workloads.CoreWorkload
  
  readallfields=true
  
  readproportion=0.95
  updateproportion=0.05
  scanproportion=0
  insertproportion=0
  
  requestdistribution=zipfian
  zipfian.constant=0.5
  threadcount=1
  ```

  For Redis and Memcached, make sures that they are running. For RocksDB, since it is a embedded data base, it will be started by YCSB.

  Then, load pre-exist data into the underlying database:

  ```bash
  # Redis
  ./bin/ycsb load redis -s -P workloads/workload_zipfian -p "redis.host=127.0.0.1" -p "redis.port=6379"
  
  # Memcached
  ./bin/ycsb load memcached -s -P workloads/workload_zipfian -p "memcached.hosts=127.0.0.1"
  
  # RocksDB (we use direct I/O here)
  ./bin/ycsb load rocksdb -s -P workloads/workload_zipfian -p rocksdb.dir=/newdir/ycsb-rocksdb-data -p rocksdb.direct=true
  ```

  At last, run the specified workload:

  ```bash
  # Redis
  ./bin/ycsb run redis -s -P workloads/workload_zipfian -p "redis.host=127.0.0.1" -p "redis.port=6379" -p "status.interval=1"
  
  # Memcached
  ./bin/ycsb run memcached -s -P workloads/workload_zipfian -p "memcached.hosts=127.0.0.1" -p "status.interval=1" -p "status.interval=1"
  
  # RocksDB (we use direct I/O here)
  ./bin/ycsb run rocksdb -s -P workloads/workload_zipfian -p rocksdb.dir=/newdir/ycsb-rocksdb-data -p rocksdb.direct=true -p "status.interval=1"
  ```

## Yahoo Streaming Benchmark


* Install JDK 1.8

* Install Maven 3.6:

  ```bash
  cd /opt
  sudo wget https://archive.apache.org/dist/maven/maven-3/3.6.0/binaries/apache-maven-3.6.0-bin.tar.gz
  sudo tar -xvzf apache-maven-3.6.0-bin.tar.gz
  sudo mv apache-maven-3.6.0 maven
  ```

  Add the following lines to `/etc/profile.d/mavenenv.sh`:

  ```bash
  export M2_HOME=/opt/maven
  export PATH=${M2_HOME}/bin:${PATH}
  ```

  Then run:

  ```bash
  sudo chmod +x /etc/profile.d/mavenenv.sh
  source /etc/profile.d/mavenenv.sh
  ```


* Install Yahoo Streaming Benchmark: (make sure you have enough space)

  ```bash
  sudo apt-get install leiningen jq
  git clone https://github.com/yuhong-zhong/streaming-benchmarks.git
  cd streaming-benchmarks
  ./stream-bench.sh SETUP
  g++ atomic_write.cpp -o atomic_write -std=c++14
  ```

* Config Redis which is used by the streaming benchmark:

  - Edit `/etc/sysctl.conf` and add `vm.overcommit_memory=1`

  - Then reboot or run the command `sysctl vm.overcommit_memory=1` for this to take effect

  - Disable transparent hugepage: 

    ```sh
    echo never > /sys/kernel/mm/transparent_hugepage/enabled
    echo never > /sys/kernel/mm/transparent_hugepage/defrag
    ```

    You can add this to `/etc/rc.local` to retain the setting after a reboot

  - Edit `streaming-benchmarks/redis-4.0.11/redis.conf` to not save `*.rdb` or `*.aof` file

    - make sure `appendonly` is `no` 
    - remove or comment:
      `save 900 1`
      `save 300 10`
      `save 60 10000`
    - add `save ""`; otherwise redis will store the in-memory data to a `.rdb` file at certain interval or after exceeding certain memory limit

* Run Yahoo Streaming Benchmark with a given test time:

  ```bash
  export TEST_TIME=7200  # run with 7200 seconds
  ./stream-bench.sh STORM_TEST
  ```

* To get the average task completion time with the latest 10 min window, run:

  ```bash
  # make sure Storm is running
  ./storm-stat.sh
  # Then the number will be writen to /tmp/storm (in ms)
  ```

Yahoo Streaming Benchmark can also run the benchmark on Apache Spark and Apache Flink. However, we haven't try them yet.

## TensorFlow

* Install Python environment:

  ```bash
  sudo apt-get update
  sudo apt-get install python3-dev python3-pip
  sudo apt-get install python3-venv
  sudo apt-get install git
  python3 -m venv env
  source env/bin/activate
  python -m pip install -U pip
  python -m pip install -U setuptools
  pip install tensorflow==1.5.0  # higher version might not work in VM
  ```

* Clone the workload repo:

  ```bash
  git clone https://github.com/yuhong-zhong/HW3.git
  cd HW3/cifar10_estimator
  g++ atomic_write.cpp -o atomic_write -std=c++14
  ```

* Create dataset:

  ```bash
  python generate_cifar10_tfrecords.py --data-dir=${PWD}/cifar-10-data
  ```

* Start training:

  ```bash
  python cifar10_main.py --data-dir=${PWD}/cifar-10-data \
                         --job-dir=/tmp/cifar10 \
                         --num-gpus=0 \
                         --train-steps=10000
  # will run for a few hours
  
  # number of sample processed per second will be reported to /tmp/tf
  ```

## Snowset

* Download Snowset:

  ```bash
  wget http://www.cs.cornell.edu/~midhul/snowset/snowset-main.csv.gz
  gunzip snowset-main.csv.gz
  ```

* Parse Snowset and generate a Memcachier-like workload file with the Python script `scripts/parse_snowset.py`

* Install [YCSB](#YCSB)

* Install [Redis](#Redis) or [RocksDB](#RocksDB) or [Memcached](#Memcached)

* Save the following lines to `YCSB/workload/workload_snowset`:

  ```bash
  operationcount=126726366  # number of requests generated
  threadcount=36
  workload=site.ycsb.workloads.MemcachierWorkload
  trace=/newdir/tmp/snowset-m.out  # location of the parsed workload file, should be in tmpfs
  acceleration=10  # how much do we want to accelerate the trace
  ```

* Run Snowset:

  ```bash
  # Redis
  ./bin/ycsb run redis -s -P workloads/workload_snowset -p "redis.host=127.0.0.1" -p "redis.port=6379" -p "status.interval=1"
  
  # Memcached
  ./bin/ycsb run memcached -s -P workloads/workload_snowset -p "memcached.hosts=127.0.0.1" -p "status.interval=1" -p "status.interval=1"
  
  # RocksDB (we use direct I/O here)
  ./bin/ycsb run rocksdb -s -P workloads/workload_snowset -p rocksdb.dir=/newdir/ycsb-rocksdb-data -p rocksdb.direct=true -p "status.interval=1"
  ```

## Cloudlab Configuration

#### KVM Management
- To check whether the system supports virtualization
    ```sh
    # if the system supports virtualization this shoud print any value except 0
    egrep -c '(vmx|svm)' /proc/cpuinfo
    
    # determine if your server is capable of running hardware accelerated KVM
    sudo apt install -y cpu-checker
    sudo kvm-ok
    ```
- To install KVM
    ```sh
    sudo apt update
    sudo apt install -y uvtool qemu-kvm libvirt-bin ubuntu-vm-builder bridge-utils virt-manager libosinfo-bin libguestfs-tools virt-top
    service libvirtd status
    
    #if libvirtd is not enabled
    sudo service libvirtd start
    sudo update-rc.d libvirtd enable
    
    # add your current user to group libvirtd
    sudo adduser `id -un` libvirtd
    ```
- To instantiate a KVM
    ```sh
    #sync to vm image, get the bionic (18.04 LTS) image
    uvt-simplestreams-libvirt --verbose sync --source http://cloud-images.ubuntu.com/daily release=bionic arch=amd64
    
    #check whether it is added correctly
    uvt-simplestreams-libvirt query
    ssh-keygen
    uvt-kvm create vm1 --memory 32768 --cpu 16 --disk 20 --ssh-public-key-file /root/.ssh/id_rsa.pub --packages gcc,g++,python,dstat,git,build-essential,kernel-package,fakeroot,libncurses5-dev,libssl-dev,ccache,libelf-dev,libqt4-dev,pkg-config,ncurses-dev,autoconf,automake,libpcre3-dev,libevent-dev,zlib1g-dev,vim,python-pip,openjdk-8-jdk,ant-optional,cmake #--bridge br0 --run-script-once RUN_SCRIPT_ONCE
    
    #wait until the vm is ready to use
    uvt-kvm wait vm1
    #to get the ip address of the VM
    uvt-kvm ip vm1
    #to ssh to that VM
    uvt-kvm ssh vm1

    #to see the vm status from host
    virsh list 
    #to shutdown the vm from host
    virsh shutdown vm1
    #restart the vm again from host
    virsh start vm1
    #to delete the VM
    virsh destroy vm1
    #to destroy the vm with all its storage 
    virsh undefine vm1 --remove-all-storage 
    ```
- To add a swap device to the KVM
    ```sh
    # create a disk 
    qemu-img create -f raw vm1_swap.img 20G
    #if vdc is already mounted, use vdb or vdd  at target
    virsh attach-disk vm1 --source /path/to/image/vm1_swap.img --target vdc --persistent
    
    #Inside the VM
    reboot
    #use 'gpt' partition and filesystem type '82 (linux/linux swap)'
    cfdisk /dev/vdc
    mkswap /dev/vdc1
    swapon /dev/vdc1
    mkdir /mnt/new-disk
    mount /dev/vdc1 /mnt/new-disk

    #Add the following to '/etc/fstab' for reboot persistence
    /dev/vdb1   swap            swap    defaults    0 0
    ```
    
- If you want to bridge a network, configure the interface
    ```sh
    sudo vim /etc/network/interfaces
    
    #modify the file with
    auto br0
    iface br0 inet dhcp
        bridge_ports eno1
        bridge_stp off
        bridge_fd 0
        bridge_maxwait 0
    #save and close the file and restart network
    sudo service networking restart
    
    #During KVM create, add '--bridge br0' in the 'uvt-kvm create' command
    ```
- Tutorial on setting up KVM on [CloudLab](https://wtao0221.github.io/2018/04/27/KVM-Virtual-Function-Configuration-on-CloudLab/), [general server](https://www.cyberciti.biz/faq/how-to-use-kvm-cloud-images-on-ubuntu-linux/), configuring SRI-OV for ConnectX-3 with KVM ([InfiniBand](https://community.mellanox.com/s/article/howto-configure-sr-iov-for-connectx-3-with-kvm--infiniband-x))
#### Add 1TB Disk
- To add 1TB disk to ClodLab Machines
    ```sh
    sudo mkdir /newdir
    sudo /usr/local/etc/emulab/mkextrafs.pl /newdir
    ```
    If you get a strange GPT error, do this:
    ```sh
    sudo apt-get install gdisk
    sudo sgdisk --zap /dev/sda
    sudo /usr/local/etc/emulab/mkextrafs.pl /newdir
    ```
#### Setup RAMDisk
- To setup a RAMDisk 
    ```sh
    mkdir /mnt/ramdisk
    mke2fs /dev/ram0
    mount -t ext2 /dev/ram0 /mnt/ramdisk/
    dd if=/dev/zero of=/mnt/ramdisk/sw bs=4096 count=1000000
    mkswap /mnt/ramdisk/sw 1000000
    chmod 600 /mnt/ramdisk/sw
    sync
    swapon -s
    swapoff /dev/sda3 #replace /dev/sda3 by swapon -s output
    swapon /mnt/ramdisk/sw
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
    
#### Compile Linux from Source Code
* Install package dependencies 
    ```sh
    apt-get install -y git build-essential kernel-package fakeroot libncurses5-dev libssl-dev ccache  libelf-dev libqt4-dev pkg-config ncurses-dev
      
    #if gcc/g++ has older versions, update them to gcc-7
    apt-get install -y software-properties-common
    add-apt-repository ppa:ubuntu-toolchain-r/test
    apt update
    apt install g++-7 -y
    
    #Set it up so the symbolic links gcc, g++ point to the newer version:
    update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-7 60
    update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-7 60
    update-alternatives --config gcc
    gcc --version
    g++ --version
    ```
* Now get the source code and compile
    ```sh
    wget -c https://mirrors.edge.kernel.org/pub/linux/kernel/v4.x/linux-4.10.1.tar.gz # replace the URL for different versions of linux
    tar -xzvf linux-4.10.1.tar.gz
    
    make mrproper # clean previous makes
    cp /boot/config-`uname -r` .config # if the .config is not there. This will copy the existing linux's config file, open the .config file to make necessary config change.
    #set CONFIG_IDLE_PAGE_TRACKING=y for enabling idle page trancking
    #set CONFIG_MLX_PLATFORM=y for enabling MLNX platform drivers
    yes '' | make oldconfig # localmodconfig creates a config based on current config and loaded modules
    make -j32 # -jN for parallelising the make with N cores 
    make headers_install
    make modules_install
    make install && reboot
    ```
#### Upgrade Kernel to 4.11
```sh
cd /tmp/

wget http://kernel.ubuntu.com/~kernel-ppa/mainline/v4.11/linux-headers-4.11.0-041100_4.11.0-041100.201705041534_all.deb

wget http://kernel.ubuntu.com/~kernel-ppa/mainline/v4.11/linux-headers-4.11.0-041100-generic_4.11.0-041100.201705041534_amd64.deb

wget http://kernel.ubuntu.com/~kernel-ppa/mainline/v4.11/linux-image-4.11.0-041100-generic_4.11.0-041100.201705041534_amd64.deb

sudo dpkg -i *.deb
```