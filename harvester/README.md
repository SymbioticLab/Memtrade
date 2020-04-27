# Memory Harvester

Besides free memory, we also want to harvest cold memory occupied by the application. To achieve this goal, we use a control loop to dynamically adjust the cgroup limit in order to search for the minimum cgroup limit w/o severe performance degradation. We implemented three types of Additive-Decrease & Multiplicative-Increase control loops: throughput-based control loop `cmanager`, latency-based control loop `cmanager_latency`, and promotion-rate-based control loop `rcmanager`. Each of them collects feedback from different metrics (i.e., throughput, latency, and promotion rate) and has its own set of suitable applications.

Using a control loop to dynamically search for the minimum cgroup limit might cause severe performance drops when the cgroup limit is close to the point where most of the pages swapped out are hot pages. To alleviate this issue, we implemented `tswap`, which temporarily buffers swapped-out pages in memory before moving them to swap devices. tswap will write a buffered page to the swap device when it has not been accessed for a given time (i.e., quarantine time). Since the quarantine time will delay the effect of swapping pages to disk on application's performance, when the cgroup limit is about to go below the application's RSS, we set a bottom line for cgroup limit which lives for at least quarantine time to constrain the control loop from exploring too much before noticing the effect of writing cold pages to disk. In addition, to deal with workload shifts, we let the control loop to notify tswap to prefetch some in-disk pages into memory when the performance becomes really bad.

Finally, to calculate how much memory to harvest, we implemented `balloon`, a user-space balloon driver which collects the application's RSS and tswap's in-memory size and computes the amount of memory to harvest. In order to be conservative, we used Exponentially Weighted Moving Average (EWMA) to estimate the free memory size and increase the harvested size if the estimated free memory size goes beyond a threshold. Also, when the estimated memory size drops below another threshold (smaller than the one used to harvest more memory in order to reduce the oscillation of the size of the harvested memory), we will subtract some memory from the harvested size to make sure the producer's application always get the amount of memory it needs.

## Directory Structure

* **harvester**

  * **cmanager**

    throughput-based feedback control loop

  * **cmanager_latency**

    latency-based feedback control loop

  * **rcmanager**

    promotion-rate-based feedback control loop

  * **tswap**

  * **balloon**

## Prerequisites

OS: Ubuntu 14.04 or 16.04 (kernel 4.11)

## Build

1. Install cmake and cgroup-bin:

   ```bash
	sudo apt-get update
	sudo apt-get install cmake cgroup-bin -y
   ```

2. Clone the spot-memory repository:

   ```bash
   git clone https://github.com/SymbioticLab/spot-memory.git
   ```

3. Compile cmanager (or cmanager_latency, rcmanager):

   ```bash
    cd spot-memory/harvester/cmanager
    cmake .
    cmake --build .
   ```

4. Compile tswap:

   ```bash
    cd ../tswap
    make
   ```

5. Compile balloon:

   ```bash
    cd ../balloon
    cmake .
    cmake --build .
   ```

6. Install MVN

   You might need to install Java 8 and set environment variables

   ```	bash
   wget https://archive.apache.org/dist/maven/maven-3/3.6.0/binaries/apache-maven-3.6.0-bin.tar.gz
   sudo tar -xvzf apache-maven-3.6.0-bin.tar.gz
   sudo mv apache-maven-3.6.0 maven
   
   sudo echo "export M2_HOME=/opt/maven\nexport PATH=${M2_HOME}/bin:${PATH}\nexport" > /etc/profile.d/mavenenv.sh
   source /etc/profile.d/mavenenv.sh
   ```

7. Download and Compile YCSB

	```bash
	git clone https://github.com/nanua/YCSB.git
	cd YCSB
	git checkout spot_memory
	 mvn -pl site.ycsb:redis-binding -am clean package
	mvn -pl site.ycsb:memcached-binding -am clean package
	```


## Run

We show how to run the memory harvester with YCSB-Redis as the application.

1. To monitor the size of the swap used by a certain cgroup, we need to enable Linux swap accounting by appending `cgroup_enable=memory swapaccount=1` to the `GRUB_CMDLINE_LINUX` item within the `/etc/default/grub` file (and make sure `GRUB_CMDLINE_LINUX` is not overwritten elsewhere in that file). Then, we need to update grub and reboot:

	```bash
	sudo update-grub
	sudo reboot
	```


2. Install tswap:

   ```bash
   sudo insmod tswap.ko
	```

3. Build and configure Redis

   ```bash
   git clone https://github.com/antirez/redis.git
   cd redis 
   # modify the fifth argument of createIntConfig("tracking-table-max-fill", ...) in file "src/config.c" from 100 to 1000000 to pass make test
   make distclean # important! 
   make 
   ```
   
   Configure Redis:
   
   * Edit `/etc/sysctl.conf` and add `vm.overcommit_memory=1`. Then reboot or run the command `sysctl vm.overcommit_memory=1` for this to take effect
   
   * Disable transparent hugepage: 
   
   ```sh
   echo never > /sys/kernel/mm/transparent_hugepage/enabled
   echo never > /sys/kernel/mm/transparent_hugepage/defrag
   ```
   
   You can add this to `/etc/rc.local` to retain the setting after a reboot
   
   * Edit `./redis/redis.conf` to not save `*.rdb` or `*.aof` file  
     * make sure `appendonly` is `no` 
     * remove or comment:
       * `save 900 1`
       * `save 300 10`
       * `save 60 10000`
     * add `save ""`; otherwise redis will store the in-memory data to a `.rdb` file at certain interval or after exceeding certain memory limit

4. Make the cgroup memory limit unlimited and start Redis with cgroup

	```bash
	sudo cgcreate -g memory:[cgroup name]
	#sudo cgcreate -g memory:redis
	echo -1 > 	/sys/fs/cgroup/memory/cgroup_name/memory.limit_in_bytes
	#echo -1 > /sys/fs/cgroup/memory/redis/memory.limit_in_bytes
	cgexec -g memory:[cgroup_name] ./redis/src/redis-server ./redis/redis.conf
	#cgexec -g memory:redis ./redis/src/redis-server ./redis/redis.conf
	```

5. YCSB load data 
	```bash
	cd ../YCSB
	./bin/ycsb load redis -s -P workloads/[workload name] -p "redis.host=127.0.0.1" -p "redis.port=6379"
	#./bin/ycsb load redis -s -P workloads/workloada -p "redis.host=127.0.0.1" -p "redis.port=6379"
	```
	Run workload
	```bash
	./bin/ycsb run redis -s -P workloads/[workload name] -p "redis.host=127.0.0.1" -p "redis.port=6379" -p "status.interval=1"
	#./bin/ycsb run redis -s -P workloads/workloada -p "redis.host=127.0.0.1" -p "redis.port=6379" -p "status.interval=1"
	```
	
	Optional run two workloads one after another:
	
	```bash
	./bin/ycsb run redis -s -P workloads/[workload a name] -p "redis.host=127.0.0.1" -p "redis.port=6379" -p "status.interval=1" && ./bin/ycsb run redis -s -P workloads/[workload b name] -p "redis.host=127.0.0.1" -p "redis.port=6379" -p "status.interval=1"
	#./bin/ycsb run redis -s -P workloads/workload_oliver_th -p "redis.host=127.0.0.1" -p "redis.port=6379" -p "status.interval=1" && ./bin/ycsb run redis -s -P workloads/workload_oliver_bh -p "redis.host=127.0.0.1" -p "redis.port=6379" -p "status.interval=1"
	```
	
6. Run control loop (cmanager, cmanager_latency, or rcmanager):

	```bash
	# run cmanager
   
	#sudo <cgroup name> <performance file path> <initial cgroup size (MB)> <logging file path> <tswap stat path (optional)>
   
	sudo ./cmanager redis /tmp/ycsb 9000 /tmp/cman_ycsb /sys/kernel/tswap/tswap_stat
   
	# run cmanager_latency
   
	# usage: <cgroup name> <latency file path> <initial cgroup size (MB)> <logging file path> <tswap stat path (optional)>
   
	sudo ./cmanager redis /tmp/ycsb_latency 9000 /tmp/cman_ycsb /sys/kernel/tswap/tswap_stat
   
	# run rcmanager
   
	# usage: <cgroup name> <promotion rate file path> <disk promotion rate file path> <initial cgroup size (MB)> <logging file path> <tswap stat path> <performance file path (optional)>
   
	sudo ./rcmanager redis /sys/kernel/tswap/tswap_nr_promoted_page  /sys/kernel/tswap/tswap_nr_disk_promoted_page 9000 /tmp/cman_ycsb /sys/kernel/tswap/tswap_stat
	```

7. Run balloon:

	```bash
	cd balloon
   
	# usgae: <cgroup name> <harvested size file path>
	sudo ./balloon redis /tmp/harvested_size
   
	# Then, the harvested size will be written to /tmp/harvested_size in bytes (with advisory file lock)
	```

   