set -e
export CR_IP_TOOL=/root/iproute2/ip/ip
ulimit -c unlimited
git clean -dxf
make clean
make
bash test//zdtm.sh -C -x '\(maps\|fanot\|fpu\|mmx\|sse\|rtc\)'
