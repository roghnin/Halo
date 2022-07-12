# export LD_LIBRARY_PATH=/usr/local/lib64/vmem_debug:$LD_LIBRARY_PATH
export LD_LIBRARY_PATH=/home/aim/hdk/Halo/third/pmdk/src/nondebug:$LD_LIBRARY_PATH
# for w in ycsba ycsbb ycsbc ycsbd ycsbe ycsbf ycsbg ycsbh ycsbj ycsbak ycsbal ycsbam PiBench1 PiBench2 PiBench3 PiBench4 PiBench5 PiBench6 PiBench7 PiBench8
for w in ycsba ycsbb
do
    for t in 8 16 24 32 40
    do  
        # for h in HALO CCEH DASH CLEVEL PCLHT VIPER SOFT CLHT
        for h in HALO
        do
            numactl -N 0 ./$h $w $t
            rm /mnt/pmem0/hash/* -rf
            echo "------------------------------------------------\n"
        done
    done
done

# update: HALO CLEVEL VIPER 