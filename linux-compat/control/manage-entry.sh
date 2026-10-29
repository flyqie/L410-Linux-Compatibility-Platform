#!/system/bin/sh
set -eu
. /data/local/tmp/audit-alignment-v306/external_guard.sh
guard_external_data
bundle=${2:-com.l410.linux.mousepad}
printf '%s\n' "$bundle" | grep -Eq '^com\.l410\.linux($|\.(app\.)?[a-z][a-z0-9_]{0,80}$)' || exit 4
source_hap=${3:-/data/local/tmp/l410-linux-install/Mousepad.signed.hap}
if [ "$source_hap" != "/data/vendor/l410-linux/launchers/$bundle.hap" ] \
 && [ "$source_hap" != "/data/vendor/l410-linux/tray-hosts/$bundle.hap" ] \
 && [ "$source_hap" != /data/local/tmp/l410-linux-install/Mousepad.signed.hap ];then exit 5;fi
result=$(timeout 8 /data/local/tmp/audit-alignment-v306/native-install-token-probe)
line=$(printf '%s\n' "$result" | grep '^bms_install_tool_acl token=[0-9][0-9]* install_permission=0$')
token=${line#bms_install_tool_acl token=};token=${token% install_permission=0}
case "$token" in ''|0|*[!0-9]*) exit 3;; esac
case "${1:-}" in
 enable)
  test -f "$source_hap"
  staged=/data/local/tmp/l410-linux-install/$bundle.hap
  rollback=/data/local/tmp/l410-linux-install/$bundle.rollback.hap
  cp "$source_hap" "$staged"
  chmod 0644 "$staged"
  if test -f "/data/app/el1/bundle/public/$bundle/entry.hap"; then
   cp "/data/app/el1/bundle/public/$bundle/entry.hap" "$rollback"
   chmod 0644 "$rollback"
   uninstall=$(timeout 45 /data/local/tmp/identity_token_exec_v3 "$token" 2000 2000 /system/bin/bm uninstall -n "$bundle" -u 100)
   printf '%s\n' "$uninstall"
   case "$uninstall" in
    *'uninstall bundle successfully'*|*'uninstall missing installed bundle'*) ;;
    *) exit 7;;
   esac
  fi
  if ! output=$(timeout 45 /data/local/tmp/identity_token_exec_v3 "$token" 2000 2000 /system/bin/bm install -p "$staged" -u 100); then
   printf '%s\n' "$output"
   if test -f "$rollback"; then
    timeout 45 /data/local/tmp/identity_token_exec_v3 "$token" 2000 2000 /system/bin/bm install -p "$rollback" -u 100 || true
   fi
   rm -f "$staged" "$rollback"
   exit 6
  fi
  printf '%s\n' "$output"
  printf '%s\n' "$output" | grep -F 'install bundle successfully'
  rm -f "$staged" "$rollback"
  ;;
 disable)
  output=$(timeout 45 /data/local/tmp/identity_token_exec_v3 "$token" 2000 2000 /system/bin/bm uninstall -n "$bundle" -u 100)
  printf '%s\n' "$output"
  case "$output" in
   *'uninstall bundle successfully'*|*'uninstall missing installed bundle'*) ;;
   *) exit 7;;
  esac
  ;;
 *) exit 2;;
esac
