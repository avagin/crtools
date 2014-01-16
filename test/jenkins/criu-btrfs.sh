echo 950000 > /sys/fs/cgroup/cpu,cpuacct/system/cpu.rt_runtime_us || true
echo 950000 > /sys/fs/cgroup/cpu,cpuacct/system/jenkins.service/cpu.rt_runtime_us || true
git clean -dfx &&
make -j 4 && make -C test/zdtm &&

# A few tests is excluded due to:
# mountpoints works only from a root mount
# maps04 is too slow on btrfs
bash -x test/zdtm.sh -C -x "\(maps04\|mountpoints\)" || {
    tar -czf /home/criu-btrfs-${GIT_COMMIT}-$(date +%m%d%H%M).tar.gz .
    exit 1
}
