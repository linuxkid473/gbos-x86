#!/bin/sh
# Run in an isolated privileged test container with tracefs and Python available.
set -eu
group=gboscamera_test$$
mountpoint=$(mktemp -d)
instance=
send=0
simple=0
simple_return=0
response=0
mounted=0
cleanup() {
    if [ -n "$instance" ] && [ -f "$instance/events/$group/enable" ]; then
        echo 0 > "$instance/events/$group/enable"
    fi
    [ "$response" = 0 ] || echo "-:$group/response" >> "$mountpoint/kprobe_events"
    [ "$simple_return" = 0 ] || echo "-:$group/simple_return" >> "$mountpoint/kprobe_events"
    [ "$simple" = 0 ] || echo "-:$group/simple" >> "$mountpoint/kprobe_events"
    [ "$send" = 0 ] || echo "-:$group/send" >> "$mountpoint/kprobe_events"
    [ -z "$instance" ] || rmdir "$instance"
    [ "$mounted" = 0 ] || umount "$mountpoint"
    rmdir "$mountpoint"
}
trap cleanup EXIT
mount -t tracefs -o nosuid,nodev,noexec tracefs "$mountpoint"
mounted=1
instance=$mountpoint/instances/$group
mkdir "$instance"
printf '%s\n' "p:$group/send __x64_sys_getpid command=%r9:u16 stream=%si:u32" >> "$mountpoint/kprobe_events"
send=1
printf '%s\n' "p:$group/simple __x64_sys_getpid command=%dx:u16 stream=%si:u32" >> "$mountpoint/kprobe_events"
simple=1
printf '%s\n' "r:$group/simple_return __x64_sys_getpid result=\$retval:s32" >> "$mountpoint/kprobe_events"
simple_return=1
printf '%s\n' "r:$group/response __x64_sys_getpid valid=\$retval:x64 stream=+40(\$retval):u8 type=+41(\$retval):u8 error=+24(\$retval):u32" >> "$mountpoint/kprobe_events"
response=1
echo 'valid != 0' > "$instance/events/$group/response/filter"
echo 1 > "$instance/events/$group/enable"
echo 1 > "$instance/tracing_on"
python3 -c 'import os; os.getpid()'
echo 0 > "$instance/tracing_on"
if ! grep -q ' send:' "$instance/trace" ||
   ! grep -q ' simple:' "$instance/trace" ||
   ! grep -q ' simple_return:' "$instance/trace" ||
   ! grep -q ' response:' "$instance/trace"; then
    cat "$instance/trace"
    cat "$mountpoint/kprobe_profile"
    echo "tracefs did not record the test events" >&2
    exit 1
fi
echo CAMERA_FW_TRACE_KERNEL_TEST_PASS
