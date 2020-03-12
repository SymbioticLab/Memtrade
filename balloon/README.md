# User-Space Balloon Driver

This user-space balloon driver (USBD) is implemented as an alternative for the kernel-space balloon driver, aiming at being more convenient and can run without any support from the hypervisor. It is similar to the kernel-space balloon driver in grabbing free memory and adding pressure to the applications running in the same VM.

While the kernel-space balloon driver is originally developed to share memory between VMs in the same physical server, USBD is designed to sell **ephemeral** spot-memory to the consumers at a lower price. Therefore, USBD can provide a higher performance guarantee for the producer by discarding data. Also, since the producer chooses to engage in spot-memory instead of just allocating less memory to the VM, it is reasonable to assume that stable performance is critical to the producer. With those factors taken into consideration, USBD prioritizes the performance of the producer and is more clever regarding how and when to reclaim the cold pages of the applications by swapping them out.

## Build

First, we need to install cmake:

```bash
sudo apt-get update
sudo apt-get install cmake
```

Then, since USBD depends on the swap space accounting extension of Linux, we need to append `cgroup_enable=memory swapaccount=1` to the `GRUB_CMDLINE_LINUX` item within the `/etc/default/grub` file. After that, we enable swap accounting by:

```bash
sudo update-grub
sudo reboot
```

Next, we can build USBD with cmake:

```bash
cd Balloon
cmake .
cmake --build .
```

## Run

First, we need to create a cgroup within which the applications should be run:

```bash
sudo cgcreate -g memory:app
```

Then, run USBD by:

```
sudo ./Balloon app
```

where the parameter is the name of the cgroup.

Within the command-line interface of USBD, `target swap size`, `target grab size lower bound`, `target grab size upper bound`, `current grab size`, `current swap size` will be showed. We can edit `target swap size`, `target grab size lower bound` and `target grab size upper bound`  by entering the desired numbers with keyboard and click "Enter" when the "Confirm" button is selected.

## Design

USBD has three control parameters: `target swap size`, `target grab size lower bound` and `target grab size upper bound`. Those parameters determine whether USBD will inflate or deflate, and whether USBD will try to swap some pages of the applications out.

USBD will inflate when there is still a lot of free memory within the VM and the `target grab size lower bound` is larger than the `current grab size`. At here, `target grab size lower bound` represents the spot-memory requirement of the spot-memory coordinator.

In contrast, USBD will deflate when `current grab size` is larger than `target grab size upper bound`. `target grab size upper bound` is used by the spot-memory coordinator to ensure that there is enough free memory in the producer for maintaining the performance of the applications running on it.

In addition, USBD will deflate when the applications start to allocate more memory, or when it receives medium/critical memory pressure notification from the root memory cgroup. Therefore, USBD can make sure that the performance of the applications is prioritized.

In terms of swapping, USBD will monitor the RSS of the applications, and will only try to swap cold pages when the RSS growth is small enough. This design is motivated by the fact that swapping will block malloc requests and can thus cause severe performance degradation.