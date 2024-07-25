gcc_version=5.5.0
root_dir=../

version=`./${root_dir}/build/bin/novadbplus -v | awk '{print $2}'|awk -F '=' '{print $2}'`
echo ${version}

# use packname from shell.
packname=$1
rm ${packname}_back -rf
mv $packname ${packname}_back
mkdir -p $packname
mkdir -p $packname/bin
mkdir -p $packname/bin/deps
mkdir -p $packname/scripts

cp ${root_dir}/build/bin/novadbplus $packname/bin
cp ${root_dir}/build/bin/binlog_tool $packname/bin
cp ${root_dir}/build/bin/ldb_novadb $packname/bin
cp ${root_dir}/pack/start-redis.sh $packname/bin
cp ${root_dir}/pack/stop-redis.sh $packname/bin
cp ${root_dir}/bin/redis-cli $packname/bin
cp /usr/local/gcc-${gcc_version}/lib64/libstdc++.so.6 $packname/bin/deps
cp ${root_dir}/pack/start.sh $packname/scripts
cp ${root_dir}/pack/stop.sh $packname/scripts
cp ${root_dir}/novadbplus.conf $packname/scripts

mv ${packname}.tgz ${packname}_back.tgz
tar -cvzf ${packname}.tgz ${packname}/*

echo -e "\033[32mpack success: ${packname}.tgz \033[0m"
