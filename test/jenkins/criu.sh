# systemd executes jenkins in a separate cpu cgroup
echo 950000 > /sys/fs/cgroup/cpu,cpuacct/system/cpu.rt_runtime_us || true
echo 950000 > /sys/fs/cgroup/cpu,cpuacct/system/jenkins.service/cpu.rt_runtime_us || true

git clean -dfx &&
make -j 4 && make -C test/zdtm &&

# Dump/restore
bash test/zdtm.sh -C &&

# Three iterations of dump/restore
bash test/zdtm.sh -C -i 3 &&

# Use page-server
bash test/zdtm.sh -p -C &&

# Dump w/o restore
bash test/zdtm.sh -d -C &&

# Make three iterative snapshots and check the last one
bash test/zdtm.sh -s -i 3 -C -x '\(unlink\|socket-tcp\)' &&

true || {
    tar -czf /home/criu-p-${GIT_COMMIT}-$(date +%m%d%H%M).tar.gz .
    exit 1
}

# Execute per-commit job
git rev-parse tested || ( git tag tested; exit )
for i in `git rev-list --reverse tested..HEAD`; do
    curl "http://localhost:8080/job/CRIU-by-id/buildWithParameters?token=d6edab71&TEST_COMMIT=$i" || exit 1
done
git tag -f tested
