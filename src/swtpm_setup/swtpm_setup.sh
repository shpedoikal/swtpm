#!/bin/bash

#
# swtpm_setup.sh
#
# Authors: Stefan Berger <stefanb@us.ibm.com>
#
# (c) Copyright IBM Corporation 2011,2014,2015.
#
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
# Redistributions of source code must retain the above copyright notice,
# this list of conditions and the following disclaimer.
#
# Redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution.
#
# Neither the names of the IBM Corporation nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

# echo "UID=$UID EUID=$EUID"

# Dependencies:
#
# - tpm_tools (tpm-tools package with NVRAM utilities)
# - tcsd      (trousers package with tcsd with -c <configfile> option)
# - expect    (expect package)

SWTPM=`type -P swtpm`
if [ -n "$SWTPM" ]; then
    SWTPM="$SWTPM socket"
fi
TCSD=`type -P tcsd`
if [ -z "$TCSD" ]; then
    echo "Error: tcsd program not found."
    exit 1
fi

SETUP_CREATE_EK_F=1
SETUP_TAKEOWN_F=2
SETUP_EK_CERT_F=4
SETUP_PLATFORM_CERT_F=8
SETUP_LOCK_NVRAM_F=16
SETUP_SRKPASS_ZEROS_F=32
SETUP_OWNERPASS_ZEROS_F=64

SETUP_DISPLAY_RESULTS_F=4096

# default values for passwords
DEFAULT_OWNER_PASSWORD=ooo
DEFAULT_SRK_PASSWORD=sss

# default configuration file
DEFAULT_CONFIG_FILE="/etc/swtpm_setup.conf"

# TPM constants
TPM_NV_INDEX_D_BIT=$((0x10000000))
TPM_NV_INDEX_EKCert=$((0xF000))
TPM_NV_INDEX_PlatformCert=$((0xF002))

TPM_NV_INDEX_LOCK=$((0xFFFFFFFF))

# Default logging goes to stderr
LOGFILE=""

trap "cleanup" SIGTERM EXIT

logit()
{
	if [ -z "$LOGFILE" ]; then
		echo "$@" >&1
	else
		echo "$@" >> $LOGFILE
	fi
}

logit_cmd()
{
	if [ -z "$LOGFILE" ]; then
		eval "$@" >&1
	else
		eval "$@" >> $LOGFILE
	fi
}

logerr()
{
	if [ -z "$LOGFILE" ]; then
		echo "Error: $1" >&2
	else
		echo "Error: $1" >> $LOGFILE
	fi
}

call_create_certs()
{
	local ret=0

	local flags="$1"
	local configfile="$2"
	local certdir="$3"
	local ek="$4"
	local vmid="$5"

	local logparam tmp
	local params="" cmd

	if [ -n "$LOGFILE" ]; then
		logparam="--logfile $LOGFILE"
	fi

	if [ $((flags & SETUP_EK_CERT_F)) -ne 0 ] || \
	   [ $((flags & SETUP_PLATFORM_CERT_F)) -ne 0 ]; then
		if [ -r "$configfile" ]; then
		        # The config file contains lines in the format:
        		# key = value
        		# or with a comment at the end started by #:
        		# key = value # comment
			create_certs_tool="$(sed -n 's/\s*create_certs_tool\s*=\s*\([^#]*\).*/\1/p' \
				"$configfile")"

			create_certs_tool_config="$(sed -n 's/\s*create_certs_tool_config\s*=\s*\([^#]*\).*/\1/p' \
				"$configfile")"
			if [ -n "$create_certs_tool_config" ]; then
				params="$params --configfile \"$create_certs_tool_config\""
			fi

			create_certs_tool_options="$(sed -n 's/\s*create_certs_tool_options\s*=\s*\([^#]*\).*/\1/p' \
				"$configfile")"
			if [ -n "$create_certs_tool_options" ]; then
				params="$params --optsfile $create_certs_tool_options"
			fi
	      	else
			logerr "Could not access config file" \
			       "'$configfile' to get" \
			       "name of certificate tool to invoke."
			return 1
		fi
	fi

	if [ -n "$create_certs_tool" ]; then
		if [ $((flags & SETUP_EK_CERT_F)) -ne 0 ]; then
			cmd="$create_certs_tool \
				--type ek \
				--ek "$ek" \
				--dir "$certdir" \
				--vmid "$vmid" \
				${logparam} ${params}"
			logit "  Invoking: $(echo $cmd | tr -s " ")"
			tmp="$(eval $cmd 2>&1)"
			ret=$?
			if [ $ret -ne 0 ]; then
				logerr "Error running '$cmd' : $tmp"
				return $ret
			fi
		fi
		if [ $((flags & SETUP_PLATFORM_CERT_F)) -ne 0 ]; then
			cmd="$create_certs_tool \
				--type platform \
				--ek "$ek" \
				--dir "$certdir" \
				--vmid "$vmid" \
				${logparam} ${params}"
			logit "  Invoking: $(echo $cmd | tr -s " ")"
			tmp="$(eval $cmd 2>&1)"
			ret=$?
			if [ $ret -ne 0 ]; then
				logerr "Error running '$cmd' : $tmp"
				return $ret
			fi
		fi
	fi

	return $ret
}

start_tpm()
{
	local swtpm="$1"
	local swtpm_state="$2"
	local ctr=0

	export TPM_PORT
	export TPM_PATH=$swtpm_state

	while [ $ctr -lt 100 ]; do
		TPM_PORT=$(shuf -i 30000-65535 -n 1)

		# skip used ports
		if [ -n "$(netstat -lnpt 2>/dev/null |
		         gawk '{print $4}' |
		         grep ":${TPM_PORT} ")" ]; then
			let ctr=ctr+1
			continue
		fi

		$swtpm 2>&1 1>/dev/null &
		SWTPM_PID=$!

		# poll for open port (good) or the process to have
		# disappeared (bad); whatever happens first
		while :; do
			kill -0 $SWTPM_PID 2>/dev/null
			if [ $? -ne 0 ]; then
				# process dead; try next socket
				break
			fi

			# Did swtpm open the TCP port yet?
			if [ -n "$(netstat -napt 2>/dev/null | 
			           grep " $SWTPM_PID/" |
			           grep ":$TPM_PORT ")" ]; then
			        # in rare occasions tcsd refuses connections
			        # test the connection
				exec 100<>/dev/tcp/localhost/$TPM_PORT 2>/dev/null
				if [ $? -ne 0 ]; then
					stop_tpm
					break
				fi
				exec 100>&-
				echo "TPM is listening on TCP port $TPM_PORT."
				return 0
			fi
			sleep 0.1
		done

		let ctr=$ctr+1
	done

	return 1
}


stop_tpm()
{
	[ "$SWTPM_PID" != "" ] && kill -SIGTERM $SWTPM_PID
	SWTPM_PID=
}


start_tcsd()
{
	local TCSD=$1
	local user=$(id -u -n)
	local group=$(id -g -n)
	local ctr=0

	export TSS_TCSD_PORT

	TCSD_CONFIG=`mktemp`
	TCSD_DATA_DIR=`mktemp -d`
	TCSD_DATA_FILE=`mktemp --tmpdir=$TCSD_DATA_DIR`

	while [ $ctr -lt 100 ]; do
		TSS_TCSD_PORT=$(shuf -i 30000-65535 -n 1)

		# skip used ports
		if [ -n "$(netstat -lnpt 2>/dev/null |
		         gawk '{print $4}' |
		         grep ":${TSS_TCSD_PORT} ")" ]; then
			let ctr=$ctr+1
			continue
		fi

		cat << EOF >$TCSD_CONFIG
port = $TSS_TCSD_PORT
system_ps_file = $TCSD_DATA_FILE
EOF
		# tcsd requires tss:tss and 0600 on TCSD_CONFIG
		# -> only root can start
		chmod 600 $TCSD_CONFIG
		#ls -l $TCSD_CONFIG
		if [ $(id -u) -eq 0 ]; then
			chown tss:tss $TCSD_CONFIG 2>/dev/null
			chown tss:tss $TCSD_DATA_DIR 2>/dev/null
			chown tss:tss $TCSD_DATA_FILE 2>/dev/null
		fi
		if [ $? -ne 0 ]; then
			logerr "Could not change ownership on $TCSD_CONFIG to ${user}:${group}."
			ls -l $TCSD_CONFIG
			return 1
		fi

		case "$(id -u)" in
		0)
			$TCSD -c $TCSD_CONFIG -e -f 2>&1 1>/dev/null &
			TCSD_PID=$!
			;;
		*)
			# for tss user, use the wrapper
			$TCSD -c $TCSD_CONFIG -e -f 2>&1 1>/dev/null &
			#if [ $? -ne  0]; then
			#	swtpm_tcsd_launcher -c $TCSD_CONFIG -e -f 2>&1 1>/dev/null &
			#fi
			TCSD_PID=$!
			;;
		esac

		# TSS still alive?
		# poll for open port (good) or the process to have
		# disappeared (bad); whatever happens first
		while :; do
			kill -0 $TCSD_PID 2>/dev/null
			if [ $? -ne 0 ]; then
				# process dead; try next socket
				break
			fi

			# Did tcsd open the TCP port yet?
			if [ -n "$(netstat -naptl 2>/dev/null | 
			           grep "LISTEN" |
			           grep " $TCSD_PID/" |
			           grep ":$TSS_TCSD_PORT ")" ]; then
			        # in rare occasions tcsd refuses connections
			        # test the connection
				exec 100<>/dev/tcp/localhost/$TSS_TCSD_PORT 2>/dev/null
				if [ $? -ne 0 ]; then
					stop_tcsd
					break
				fi
				exec 100>&-
				echo "TSS is listening on TCP port $TSS_TCSD_PORT."
				return 0
			fi
			sleep 0.1
		done

		let ctr=$ctr+1
	done

	return 1
}


stop_tcsd()
{
	[ "$TCSD_PID" != "" ] && kill -SIGTERM $TCSD_PID
	TCSD_PID=
}


cleanup()
{
	stop_tpm
	stop_tcsd
	rm -rf "$TCSD_CONFIG" "$TCSD_DATA_FILE" "$TCSD_DATA_DIR"
}


init_tpm()
{
	local flags="$1"
	local config_file="$2"
	local tpm_state_path="$3"
	local ownerpass="$4"
	local srkpass="$5"
	local vmid="$6"

	# where external app writes certs into
	local certsdir="$tpm_state_path"
	local ek tmp

	local PLATFORM_CERT_FILE="$certsdir/platform.cert"
	local EK_CERT_FILE="$certsdir/ek.cert"
	local nvramauth="OWNERREAD|OWNERWRITE"

	start_tpm "$SWTPM" "$tpm_state_path"
	if [ $? -ne 0 ]; then
		logerr "Could not start TPM."
		return 1
	fi

	export TCSD_USE_TCP_DEVICE=1
	export TCSD_TCP_DEVICE_PORT=$TPM_PORT
	PATH=$PATH:.

	swtpm_bios
	if [ $? -ne 0 ]; then
		logerr "swtpm_bios failed"
		return 1
	fi

	# TPM is enabled and activated upon first start

	start_tcsd $TCSD
	if [ $? -ne 0 ]; then
		return 1
	fi

	if [ $((flags & $SETUP_CREATE_EK_F)) -ne 0 ]; then
		tpm_createek 2>&1 1>/dev/null
		if [ $? -ne 0 ]; then
			logerr "tpm_createek failed"
			return 1
		fi
		logit "Successfully created EK."
		# Get the value of the EK
		ek=$(tpm_getpubek | 
		     sed '1,/Public Key:/d' |
		     tr -d " " |
		     tr -d '\t' |
		     tr -d '\n')
	fi

	# temporarily take ownership if an EK was created
	if [  $((flags & $SETUP_CREATE_EK_F)) -ne 0 ] ; then
		local parm_z=""
		local parm_y=""
		if [ $((flags & $SETUP_SRKPASS_ZEROS_F)) -ne 0 ]; then
			parm_z="-z"
		fi
		if [ $((flags & $SETUP_OWNERPASS_ZEROS_F)) -ne 0 ]; then
			parm_y="-y"
		fi
		a=$(expect -c "
			set parm_z \"$parm_z\"
			set parm_y \"$parm_y\"
			spawn tpm_takeownership \$parm_z \$parm_y
			if { \$parm_y == \"\" } {
				expect {
					\"Enter owner password:\" 
						{ send \"$ownerpass\n\" }
				}
				expect {
					\"Confirm password:\"
						{ send \"$ownerpass\n\" }
				}
			}
			if { \$parm_z == \"\" } {
				expect {
					\"Enter SRK password:\"
						{ send \"$srkpass\n\" }
				}
				expect {
					\"Confirm password:\"
						{ send \"$srkpass\n\" }
				}
			}
			expect eof
			catch wait result
			exit [lindex \$result 3]
		")
		if [ $? -ne 0 ]; then
			logerr "Could not take ownership of TPM."
			return 1
		fi
		logit "Successfully took ownership of the TPM."
	fi

	# have external program create the certificates now
	call_create_certs "$flags" "$config_file" "$certsdir" "$ek" "$vmid"
	if [ $? -ne 0 ]; then
		return 1
	fi

	# Define NVRAM are for Physical Presence Interface; unfortunately
	# there are no useful write permissions...
	#tpm_nvdefine \
	#	-i $((0x50010000)) \
	#	-p "PPREAD|PPWRITE|WRITEDEFINE" \
	#	-s 6 2>&1 > /dev/null

	if [ $((flags & SETUP_EK_CERT_F)) -ne 0 ] && \
	   [ -r "${EK_CERT_FILE}" ]; then
		tpm_nvdefine \
			-i $((TPM_NV_INDEX_EKCert|TPM_NV_INDEX_D_BIT)) \
			-p "${nvramauth}" \
			-s $(stat -c%s "${EK_CERT_FILE}") 2>&1 >/dev/null
		if [ $? -ne 0 ]; then
			logerr "Could not create NVRAM area for EK certificate."
			return 1
		fi
		tpm_nvwrite -i $((TPM_NV_INDEX_EKCert|TPM_NV_INDEX_D_BIT)) \
			-f "${EK_CERT_FILE}" 2>&1 >/dev/null
		logit "Successfully created NVRAM area for EK certificate."
		rm -f ${EK_CERT_FILE}
	fi

	if [ $((flags & SETUP_PLATFORM_CERT_F)) -ne 0 ] && \
	   [ -r "${PLATFORM_CERT_FILE}" ] ; then
		tpm_nvdefine \
			-i $((TPM_NV_INDEX_PlatformCert|TPM_NV_INDEX_D_BIT)) \
			-p "${nvramauth}" \
			-s $(stat -c%s "${PLATFORM_CERT_FILE}") 2>&1 >/dev/null
		if [ $? -ne 0 ]; then
			logerr "Could not create NVRAM area for platform" \
			       "certificate."
			return 1
		fi
		tpm_nvwrite \
			-i $((TPM_NV_INDEX_PlatformCert|TPM_NV_INDEX_D_BIT)) \
			-f "$PLATFORM_CERT_FILE" 2>&1 >/dev/null
		logit "Successfully created NVRAM area for platform" \
		       "certificate."
		rm -f ${PLATFORM_CERT_FILE}
	fi

	if [ $((flags & SETUP_DISPLAY_RESULTS_F)) -ne 0 ]; then
		local nvidxs=`tpm_nvinfo -n | grep 0x | gawk '{print $1}'`
		for i in $nvidxs; do
			logit "Content of NVRAM area $i:"
			tmp="tpm_nvread -i $i --password=$ownerpass"
			logit_cmd "$cmd"
		done
	fi

	# Last thing is to lock the NVRAM area
	if [ $((flags & SETUP_LOCK_NVRAM_F)) -ne 0 ]; then
		tpm_nvdefine -i $TPM_NV_INDEX_LOCK 2>&1 >/dev/null
		if [ $? -ne 0 ]; then
			logerr "Could not lock NVRAM access."
			return 1
		fi
		logit "Successfully locked NVRAM access."
	fi

	# give up ownership if not wanted
	if [ $((flags & SETUP_TAKEOWN_F))   -eq 0 -a \
	     $((flags & SETUP_CREATE_EK_F)) -ne 0 ] ; then
		a=$(expect -c "
			spawn tpm_clear
			expect {
				\"Enter owner password:\" { send \"$ownerpass\n\" }
			}
			expect eof
			catch wait result
			exit [lindex \$result 3]
		")
		if [ $? -ne 0 ]; then
			logerr "Could not give up ownership of TPM."
			return 1
		fi
		logit "Successfully gave up ownership of the TPM."

		# TPM is now disabled and deactivated; enable and activate it
		stop_tpm
		start_tpm "$SWTPM" "$tpm_state_path"

		if [ $? -ne 0 ]; then
			logerr "Could not re-start TPM."
			return 1
		fi

		TCSD_TCP_DEVICE_PORT=$TPM_PORT
		swtpm_bios -c
		if [ $? -ne 0 ]; then
			logerr "swtpm_bios -c -o failed"
			return 1
		fi
		logit "Successfully enabled and activated the TPM"
	fi

	return 0
}


usage()
{
	cat <<EOF
Usage: $1 [options]

The following options are supported:

--runas <user>   : Use the given user id to switch to and run this program;
                   this parameter is interpreted by swtpm_setup that switches
                   to this user and invokes swtpm_setup.sh; defaults to 'tss' 

--tpm-state <dir>: Path to a directory where the TPM's state will be written into;
                   this is a mandatory argument
--tpm <executable>
                 : Path to the TPM executable; this is an optional argument and
                   by default $SWTPM is used

--keyfile <file> : File containing the encryption key to be used by the TPM
                   emulator; this parameter will be passed to the TPM using
                   '--key file=<file>'

--createek       : Create the EK
--take-ownership : Take ownership; this option implies --createek
  --ownerpass  <password>
                 : Provide custom owner password; default is $DEFAULT_OWNER_PASSWORD
  --owner-well-known:
                 : Use an owner password of 20 zero bytes
  --srkpass <password>
                 : Provide custom SRK password; default is $DEFAULT_SRK_PASSWORD
  --srk-well-known:
                 : Use an SRK password of 20 zero bytes
--create-ek-cert : Create an EK certificate; this implies --createek
                   (NOT SUPPORTED YET)
--create-platform-cert
                 : Create a platform certificate; this implies --create-ek-cert
                   (NOT SUPPORTED YET)
--lock-nvram     : Lock NVRAM access

--display        : At the end display as much info as possible about the
                   configuration of the TPM

--config <config file>
                 : Path to configuration file; default is $DEFAULT_CONFIG_FILE

--logfile <logfile>
                 : Path to log file; default is logging to stderr

--keyfile <keyfile>
                 : Path to a key file containing the encryption key for the
                   TPM to encrypt its persistent state with. The content
                   must be a 32 hex digit number representing a 128bit AES key.
--pwdfile <pwdfile>
                 : Path to a file containing a passphrase from which the
                   TPM will derive the 128bit AES key. The passphrase ca be
                   32 bytes long.

--help,-h,-?     : Display this help screen
EOF
}


main()
{
	local flags=0
	local tpm_state_path=""
	local config_file="$DEFAULT_CONFIG_FILE"
	local vmid=""
	local ret
	local keyfile pwdfile

	while [ $# -ne 0 ]; do
		case "$1" in
		--tpm-state) shift; tpm_state_path="$1";;
		--tpm) shift; SWTPM="$1";;
		--createek) flags=$((flags | SETUP_CREATE_EK_F));;
		--take-ownership) flags=$((flags |
		                   SETUP_CREATE_EK_F|SETUP_TAKEOWN_F));;
		--ownerpass) shift; ownerpass="$1";;
		--owner-well-known) flags=$((flags | SETUP_OWNERPASS_ZEROS_F));;
		--srkpass) shift; srkpass="$1";;
		--srk-well-known) flags=$((flags | SETUP_SRKPASS_ZEROS_F));;
		--create-ek-cert) flags=$((flags |
		                   SETUP_CREATE_EK_F|SETUP_EK_CERT_F));;
		--create-platform-cert) flags=$((flags |
		                   SETUP_CREATE_EK_F|SETUP_PLATFORM_CERT_F));;
		--lock-nvram) flags=$((flags | SETUP_LOCK_NVRAM_F));;
		--display) flags=$((flags | SETUP_DISPLAY_RESULTS_F));;
		--config) shift; config_file="$1";;
		--vmid) shift; vmid="$1";;
		--keyfile) shift; keyfile="$1";;
		--pwdfile) shift; pwdfile="$1";;
		--runas) shift;; # ignore here
		--logfile) shift; LOGFILE="$1";;
		--help|-h|-?) usage $0; exit 0;;
		*) logerr "Unknown option $1"; usage $0; exit 1;;
		esac
		shift
	done

	[ "$ownerpass" == "" ] && ownerpass=$DEFAULT_OWNER_PASSWORD
	[ "$srkpass" == "" ] && srkpass=$DEFAULT_SRK_PASSWORD

	if [ -n "$LOGFILE" ]; then
		touch $LOGFILE &>/dev/null
		if [ ! -w "$LOGFILE" ]; then
			echo "Cannot write to logfile ${LOGFILE}." >&2
			exit 1
		fi
	fi
	if [ "$tpm_state_path" == "" ]; then
		logerr "--tpm-state must be provided"
		exit 1
	fi
	if [ ! -d "$tpm_state_path" ]; then
		logerr "$tpm_state_path is not a directory that user $(whoami) could access."
		exit 1
	fi

	if [ ! -r "$tpm_state_path" ]; then
		logerr "Need read rights on directory $tpm_state_path for user $(whoami)."
		exit 1
	fi

	if [ ! -w "$tpm_state_path" ]; then
		logerr "Need write rights on directory $tpm_state_path for user $(whoami)."
		exit 1
	fi

	rm -f \
		"$tpm_state_path"/*permall \
		"$tpm_state_path"/*volatilestate \
		"$tpm_state_path"/*savestate \
		2>/dev/null
	if [ $? -ne 0 ]; then
		logerr "Could not remove previous state files. Need execute access rights on the directory."
		exit 1
	fi

	if [ -z "$SWTPM" ]; then
		logerr "Default TPM 'swtpm' could not be found and was not provided using --tpm."
		exit 1
	fi

	if [ ! -x "$(echo $SWTPM | cut -d " " -f1)" ]; then
		logerr "TPM at $SWTPM is not an executable."
		exit 1
	fi

	if [ ! -x "$TCSD" ]; then
		logerr "TSS at $TCSD is not an executable."
		exit 1
	fi

	if [ ! -r "$config_file" ]; then
		logerr "Cannot access config file ${config_file}."
		exit 1
	fi

	if [ -n "$keyfile" ]; then
		if [ ! -r "$keyfile" ]; then
			logerr "Cannot access keyfile $keyfile."
			exit 1
		fi
		SWTPM="$SWTPM --key file=$keyfile"
		logit "  The TPM's state will be encrypted with a provided key."
	elif [ -n "$pwdfile" ]; then
		if [ ! -r "$pwdfile" ]; then
			logerr "Cannot access passphrase file $pwdfile."
			exit 1
		fi
		SWTPM="$SWTPM --key pwdfile=$pwdfile"
		logit "  The TPM's state will be encrypted using a key derived from a passphrase."
	fi

	logit "Starting vTPM manufacturing as $(id -n -u):$(id -n -g) @ $(date +%c)"

	init_tpm $flags "$config_file" "$tpm_state_path" "$ownerpass" "$srkpass" "$vmid"
	ret=$?
	if [ $ret -eq 0 ]; then
		logit "Successfully authored TPM state."
	else
		logerr "An error occurred. Authoring the TPM state failed."
	fi

	logit "Ending vTPM manufacturing @ $(date +%c)"

	exit $ret
}

main "$@"
