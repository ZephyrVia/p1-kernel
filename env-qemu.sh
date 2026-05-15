# To use: 
#
# xzl@granger1 (master)[p1-kernel]$ source env-qemu.sh
#

export PATH="/cs4414-shared/qemu/aarch64-softmmu/:${PATH}"

run-uart0() {
   qemu-system-aarch64 -M raspi3b -kernel ./kernel8.img -serial stdio 
}

run() {
    # FL: the following will launch VNC server w/ binding to a port. in case of many qemu instances coexisting,
    # it may fail to bind to an addr
    # qemu-system-aarch64 -M raspi3b -kernel ./kernel8.img -serial null -serial stdio

    # to avoid, launch w/o graphics...
    echo "**Note: use Ctrl-a then x to terminate QEMU"
    echo " ------------------------------------------------"
    qemu-system-aarch64 -M raspi3b -kernel ./kernel8.img -serial null -serial mon:stdio -nographic
}

run-mon() {
    qemu-system-aarch64 -M raspi3b -kernel ./kernel8.img -monitor stdio
}

# adopted from p3 env.sh
# will gen deterministic ports instead
p1-gen-hash-ports() {
    export MYGDBPORT=`echo -n ${USER} | md5sum | cut -c1-8 | printf "%d\n" 0x$(cat -) | awk '{printf "%.0f\n", 50000 + (($1 / 0xffffffff) * 10000)}'`
    echo "set gdb port: ${MYGDBPORT}"
}

run-debug() {
    # see comment above 
    # qemu-system-aarch64 -M raspi3b -kernel ./kernel8.img -serial null -serial stdio -s -S

    echo "Listen at port: ${MYGDBPORT}"
    echo "**To terminate QEMU, press Ctrl-a then x"
    echo 
    echo "  Next: in a separate window, launch gdb: "
    echo "      gdb-multiarch build/kernel8.elf "
    echo 
    echo "  Example gdb commands -- "
    echo "      (gdb) file build/kernel8.elf"
    echo "      (gdb) target remote :${MYGDBPORT}"
    echo "      (gdb) layout asm"
    echo "  You may want to have a custom ~/.gdbinit "
    echo "	Details: https://fxlin.github.io/p1-kernel/gdb/"
    echo " ------------------------------------------------"
    #qemu-system-aarch64 -M raspi3b -kernel ./kernel8.img -serial null -serial mon:stdio -nographic -s -S
    qemu-system-aarch64 -M raspi3b -kernel ./kernel8.img -serial null -serial mon:stdio -nographic -gdb tcp::${MYGDBPORT} -S
}

run-log() {
    # qemu-system-aarch64 -M raspi3b -kernel ./kernel8.img -serial null -serial stdio -d int -D qemu.log 
    echo "**Note: use Ctrl-a then x to terminate QEMU"
    qemu-system-aarch64 -M raspi3b -kernel ./kernel8.img -serial null -serial mon:stdio -nographic -d int -D qemu.log 
}

p1-run() {
    run
}

p1-run-log() {
    run-log
}

p1-run-debug() {
    run-debug
}

p1-run-gdb() {
    run-debug
}

usage () {
    cat << EOF
AVAILABLE COMMANDS 
------------------
    p1-run              Run qemu with the kernel (./kernel8.img)
    p1-run-log          Run qemu with the kernel, meanwhile writing qemu logs to qemu.log
    p1-run-debug        Run qemu with the kernel, GDB support on 

    (see env-qemu.sh for more commands)
EOF
}

echo "QEMU is set to: " `whereis qemu-system-aarch64`
p1-gen-hash-ports
usage
