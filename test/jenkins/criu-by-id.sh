echo 950000 > /sys/fs/cgroup/cpu,cpuacct/system/cpu.rt_runtime_us
echo 950000 > /sys/fs/cgroup/cpu,cpuacct/system/jenkins.service/cpu.rt_runtime_us
git checkout -f ${TEST_COMMIT}
git clean -dfx &&
make -j 4 && make -C test/zdtm &&

# execute tests in parallel
make -C test -j 32 zdtm_ns &&
true || {
    tar -czf /home/criu-by-id-${TEST_COMMIT}-$(date +%m%d%H%M).tar.gz .
    exit 1
}
