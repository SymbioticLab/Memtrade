mkdir /mnt/sdb
mkfs.ext4 /dev/sdb 
mount /dev/sdb /mnt/sdb/
mkdir /mnt/sdb/img

sudo rm -rf /var/lib/uvtool/libvirt/images
sudo ln -s /mnt/sdb/img/ /var/lib/uvtool/libvirt/images

cd /mnt/sdb/img
wget https://cloud-images.ubuntu.com/bionic/20200518.1/bionic-server-cloudimg-amd64.img


# start libvirtd
sudo service libvirtd start
sudo update-rc.d libvirtd enable
virsh net-autostart default
virsh net-start default

ssh-keygen

#create the vm
sudo uvt-kvm create yahoo --backing-image-file=/mnt/sdb/img/bionic-server-cloudimg-amd64.img --memory 20000 --cpu 8 --disk 20 --ssh-public-key-file /root/.ssh/id_rsa.pub --packages screen,gcc,g++,python,dstat,git,build-essential,libssl-dev,ccache,libelf-dev,libqt4-dev,pkg-config,ncurses-dev,autoconf,automake,libpcre3-dev,libevent-dev,zlib1g-dev,vim,python-pip,openjdk-8-jdk,ant-optional,cmake,python3-dev,python3-pip,python3-venv,leiningen #--run-script-once RUN_SCRIPT_ONCE
uvt-kvm wait yahoo
uvt-kvm ip yahoo

qemu-img create -f raw yahoo.img 5G
virsh attach-disk yahoo --source /mnt/sdb/img/yahoo.img  --target vdc --persistent
uvt-kvm ssh yahoo
sudo -i
screen -S yahoo

cd /opt
sudo wget https://archive.apache.org/dist/maven/maven-3/3.6.0/binaries/apache-maven-3.6.0-bin.tar.gz
sudo tar -xvzf apache-maven-3.6.0-bin.tar.gz
sudo mv apache-maven-3.6.0 maven

####modify /etc/profile.d/mavenenv.sh
touch /etc/profile.d/mavenenv.sh
cat <<EOF | tee -a /etc/profile.d/mavenenv.sh
export PATH=/opt/maven/bin:${PATH}
EOF
sudo chmod +x /etc/profile.d/mavenenv.sh
source /etc/profile.d/mavenenv.sh

cat <<EOF | tee -a /etc/sysctl.conf 
vm.overcommit_memory=1
EOF

sudo sysctl vm.overcommit_memory=1

echo never > /sys/kernel/mm/transparent_hugepage/enabled
echo never > /sys/kernel/mm/transparent_hugepage/defrag

apt-get remove -y openjdk-11-jre-headless

cd ~/
git clone https://github.com/yuhong-zhong/streaming-benchmarks.git
cd streaming-benchmarks
./stream-bench.sh SETUP

# edit redis-4.0.11/redis.conf remove or comment: save 900 1 save 300 10 save 60 10000; add save ""
export TEST_TIME=1800
./stream-bench.sh STORM_TEST

#Inside the VM to initiate the swap device
reboot
#use 'gpt' partition and filesystem type '82 (linux/linux swap)'
cfdisk /dev/vdc
mkswap /dev/vdc1
swapon /dev/vdc1
mkdir /mnt/new-disk
mount /dev/vdc1 /mnt/new-disk

#Add the following to '/etc/fstab' for reboot persistence
cat <<EOF | tee -a /etc/fstab
/dev/vdb1   swap            swap    defaults    0 0
EOF
swapon -s


128.105.144.110
cat <<EOF > routed225.xml
<network>
  <name>routed225</name>
  <forward mode='route' dev='br0'/>
  <bridge name='virbr225' stp='on' delay='2'/>
  <ip address='192.168.225.1' netmask='255.255.255.0'>
    <dhcp>
      <range start='192.168.225.41' end='192.168.225.254'/>
      <host name='ycsb' ip='192.168.225.61'/>
    </dhcp>
  </ip>
</network>
EOF

virsh destroy tensorflow
virsh destroy yahoo
virsh destroy snowset
virsh destroy memcachier

virsh undefine tensorflow --remove-all-storage
virsh undefine yahoo --remove-all-storage
virsh undefine snowset --remove-all-storage
virsh undefine memcachier --remove-all-storage