#!/system/bin/sh
# Run by init right after apexd has activated all APEXes (and before zygote): bind the SHA-NI/ADX-free libcrypto
# over every matching APEX copy (Conscrypt, ClearKey, Widevine, ...). Mounts go into init's (default) namespace.
# The "apex" lines of patched/MANIFEST (tools/port_build.py) give the path inside an APEX, the sha256 of the original
# and the patched file; a copy with any other hash is left alone and logged, so a new Google build shows up here.
M=/postinstall/gbos/patched/MANIFEST
[ -f $M ] || { log -t gbos "apex_patch: $M missing"; exit 0; }
RELS=
while read -r kind rel want file rest; do
  [ "$kind" = apex ] || continue
  case " $RELS " in *" $rel "*) ;; *) RELS="$RELS $rel";; esac
done < $M
for rel in $RELS; do
  for d in /apex/*; do
    case $d in *@*) continue;; esac
    f=$d/$rel
    [ -f $f ] || continue
    grep -q " $f " /proc/mounts 2>/dev/null && continue
    have=$(sha256sum $f | cut -d' ' -f1)
    matched=0; wants=
    while read -r kind mrel want file rest; do
      [ "$kind" = apex ] && [ "$mrel" = "$rel" ] || continue
      wants="${wants:+$wants,}$want"
      [ "$have" = "$want" ] || continue
      matched=1
      P=/postinstall/gbos/$file
      if [ ! -f $P ]; then
        log -t gbos "apex_patch: $P missing"
      else
        mount -o bind $P $f && log -t gbos "apex_patch: patched $f" || log -t gbos "apex_patch: bind FAILED $f"
      fi
      break
    done < $M
    [ "$matched" = 1 ] || log -t gbos "apex_patch: $f left alone (sha256 $have, patch is for $wants)"
  done
done
