BASE_DIR=$PWD
CMANAGER_DIR=$BASE_DIR"/cmanager"
TSWAP_DIR=$BASE_DIR"/tswap"
BALLOON_DIR=$BASE_DIR"/balloon"
PRODUCER_CLI=$BASE_DIR"/producer-cli"

sudo apt-get update
sudo apt-get install cmake cgroup-bin libevent-dev -y

# build harvester
cd $CMANAGER_DIR
cmake .
cmake --build .

# build tswap
cd $TSWAP_DIR
make
sudo insmod tswap.ko

# build balloon
cd $BALLOON_DIR
cmake .
cmake --build .

# build producer-cli
cd $PRODUCER_CLI
make producer-cli
