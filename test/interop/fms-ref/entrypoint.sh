#!/bin/bash
# Fill the fms.ini placeholders the interactive installer would normally set, then
# run the master in the foreground. Blank LICENSEINFO = free developer edition
# (enough connections for interop captures). We launch fmsmaster directly rather
# than via ./server, which does container-hostile things (sysctl, ulimit -u, a
# root check).
set -e
cd /opt/fms

INI=conf/fms.ini
set_ini() { # key value  -- replace "KEY = ..." (key may contain dots)
	local k="$1" v="$2" ev
	ev=$(printf '%s' "$v" | sed -e 's/[\/&]/\\&/g')
	sed -i -E "s|^[[:space:]]*${k//./\\.}[[:space:]]*=.*|${k} = ${ev}|" "$INI"
}

set_ini SERVER.ADMIN_USERNAME admin
set_ini SERVER.ADMINSERVER_HOSTPORT ":1111"
set_ini SERVER.PROCESS_UID 0
set_ini SERVER.PROCESS_GID 0
set_ini SERVER.LICENSEINFO ""
set_ini LIVE_DIR /opt/fms/applications/live
set_ini VOD_COMMON_DIR /opt/fms/applications/vod/media
set_ini VOD_DIR /opt/fms/applications/vod/media
set_ini SERVER.HTTPD_ENABLED false
set_ini ADAPTOR.HOSTPORT ":1935"
set_ini VHOST.APPSDIR /opt/fms/applications
set_ini APP.JS_SCRIPTLIBPATH /opt/fms/scriptlib
set_ini LOGGER.LOGDIR /opt/fms/logs

mkdir -p logs tmp
chmod -R 777 tmp logs

echo "===== filled fms.ini ====="; grep -vE '^\s*#|^\s*$' "$INI"
echo "===== ldd fmsmaster (missing libs?) ====="
LD_LIBRARY_PATH=".:Apache2.2/lib" ldd ./fmsmaster | grep -i "not found" || echo "  all libs resolved"
echo "===== launching fmsmaster -console ====="
export LD_LIBRARY_PATH=".:Apache2.2/lib"
exec ./fmsmaster -console "$@"
