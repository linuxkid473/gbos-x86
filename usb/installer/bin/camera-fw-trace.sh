#!/system/bin/sh
# Run one capture with isolated kprobes of IPU6 command and response queues.
helper=$1
sensor=$2
group=gboscam$$
mountpoint=
instance=
send_added=0
simple_added=0
simple_return_added=0
response_added=0
trace_ready=0

cleanup_trace() {
    cleanup_failed=0
    if [ "$trace_ready" = 1 ]; then
        echo 0 > "$instance/tracing_on" || { echo "ERROR: could not stop camera trace"; cleanup_failed=1; }
        echo "== IPU6 firmware queue trace (type numbers follow kernel #61)"
        sed 's/ valid=0x[0-9a-fA-F]*//g' "$instance/trace" || echo "ERROR: could not read camera trace"
        echo "== Per-CPU trace loss statistics"
        for stats in "$instance"/per_cpu/cpu*/stats; do
            [ -r "$stats" ] || continue
            echo "$stats"
            cat "$stats" || echo "ERROR: could not read trace statistics"
        done
        echo 0 > "$instance/events/$group/enable" || { echo "ERROR: could not disable camera trace events"; cleanup_failed=1; }
        trace_ready=0
    fi
    if [ "$response_added" = 1 ]; then
        echo "-:$group/response" >> "$mountpoint/kprobe_events" || { echo "ERROR: could not remove camera response probe"; cleanup_failed=1; }
        response_added=0
    fi
    if [ "$simple_return_added" = 1 ]; then
        echo "-:$group/simple_return" >> "$mountpoint/kprobe_events" || { echo "ERROR: could not remove camera command return probe"; cleanup_failed=1; }
        simple_return_added=0
    fi
    if [ "$simple_added" = 1 ]; then
        echo "-:$group/simple" >> "$mountpoint/kprobe_events" || { echo "ERROR: could not remove camera simple command probe"; cleanup_failed=1; }
        simple_added=0
    fi
    if [ "$send_added" = 1 ]; then
        echo "-:$group/send" >> "$mountpoint/kprobe_events" || { echo "ERROR: could not remove camera command probe"; cleanup_failed=1; }
        send_added=0
    fi
    if [ -n "$instance" ]; then
        rmdir "$instance" || { echo "ERROR: could not remove camera trace instance"; cleanup_failed=1; }
        instance=
    fi
    if [ -n "$mountpoint" ]; then
        umount "$mountpoint" || { echo "ERROR: could not unmount camera tracefs"; return 1; }
        rmdir "$mountpoint" || { echo "ERROR: could not remove camera tracefs directory"; return 1; }
        mountpoint=
    fi
    [ "$cleanup_failed" = 0 ]
}

trap 'cleanup_trace' EXIT
trap 'exit 1' HUP INT TERM

case "$(uname -v)" in
    '#61 '*) supported=1 ;;
    *) supported=0; echo "TRACE UNAVAILABLE: response offsets verified only for kernel #61" ;;
esac
if [ "$supported" = 1 ]; then
    mountpoint=$(mktemp -d /dev/gbos-camera-trace.XXXXXX) || mountpoint=
    if [ -z "$mountpoint" ]; then
        echo "TRACE UNAVAILABLE: cannot create mount point"
    elif ! mount -t tracefs -o nosuid,nodev,noexec tracefs "$mountpoint"; then
        echo "TRACE UNAVAILABLE: cannot mount tracefs"
        rmdir "$mountpoint" || echo "ERROR: could not remove trace directory"
        mountpoint=
    else
        instance=$mountpoint/instances/$group
        if ! mkdir "$instance"; then
            echo "TRACE UNAVAILABLE: cannot create isolated trace instance"
            instance=
        elif ! printf '%s\n' "p:$group/send ipu6_fw_isys_complex_cmd command=%r9:u16 stream=%si:u32" >> "$mountpoint/kprobe_events"; then
            echo "TRACE UNAVAILABLE: cannot register command probe"
        else
            send_added=1
            if ! printf '%s\n' "p:$group/simple ipu6_fw_isys_simple_cmd command=%dx:u16 stream=%si:u32" >> "$mountpoint/kprobe_events"; then
                echo "TRACE UNAVAILABLE: cannot register simple command probe"
            else
                simple_added=1
                if ! printf '%s\n' "r:$group/simple_return ipu6_fw_isys_simple_cmd result=\$retval:s32" >> "$mountpoint/kprobe_events"; then
                    echo "TRACE UNAVAILABLE: cannot register simple command return probe"
                else
                    simple_return_added=1
                    if ! printf '%s\n' "r:$group/response ipu6_fw_isys_get_resp valid=\$retval:x64 stream=+40(\$retval):u8 type=+41(\$retval):u8 error=+24(\$retval):u32" >> "$mountpoint/kprobe_events"; then
                        echo "TRACE UNAVAILABLE: cannot register response probe"
                    else
                        response_added=1
                        if ! echo 'valid != 0' > "$instance/events/$group/response/filter" ||
                           ! echo 1 > "$instance/events/$group/enable" ||
                           ! echo 1 > "$instance/tracing_on"; then
                            echo "TRACE UNAVAILABLE: cannot enable isolated camera trace"
                            echo 0 > "$instance/events/$group/enable" || echo "ERROR: could not disable partial trace"
                        else
                            trace_ready=1
                            echo "TRACE ACTIVE: send=complex simple=simple; command 5=flush 6=close; response 5=flush ACK 6=close ACK"
                        fi
                    fi
                fi
            fi
        fi
    fi
fi

timeout 45 "$helper" /dev/media0 "$sensor"
rc=$?
cleanup_trace || rc=3
trap - EXIT
exit "$rc"
