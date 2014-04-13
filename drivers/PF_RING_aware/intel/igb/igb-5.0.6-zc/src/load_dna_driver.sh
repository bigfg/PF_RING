#!/bin/bash

FAMILY=igb

#service udev start

# Remove old modules (if loaded)
rmmod igb
rmmod pf_ring

echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
mkdir /mnt/huge
mount -t hugetlbfs nodev /mnt/huge

# We assume that you have compiled PF_RING
insmod ../../../../PF_RING/kernel/pf_ring.ko

# As many queues as the number of processors
insmod ./igb.ko RSS=0,0,0,0,0,0,0,0

# Disable multiqueue
#insmod ./igb.ko RSS=1,1,1,1,1,1,1,1 #enable_debug=1 

sleep 1

killall irqbalance 

INTERFACES=$(cat /proc/net/dev|grep ':'|grep -v 'lo'|grep -v 'sit'|awk -F":" '{print $1}'|tr -d ' ')
for IF in $INTERFACES ; do
	TOCONFIG=$(ethtool -i $IF|grep $FAMILY|wc -l)
        if [ "$TOCONFIG" -eq 1 ]; then
		printf "Configuring %s\n" "$IF"
		ifconfig $IF up
		sleep 1
		#bash ../scripts/set_irq_affinity.sh $IF
	fi
done