#!/bin/sh
#
# s3fs - FUSE-based file system backed by Amazon S3
#
# Copyright(C) 2007 Takeshi Nakatani <ggtakec.com>
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
#

#-----------------------------------------------------------
# Common variables
#-----------------------------------------------------------
PRGNAME=$(basename "$0")
SCRIPTDIR=$(dirname "$0")
SCRIPTDIR=$(cd "${SCRIPTDIR}" || exit 1; pwd)
CURRENTDIR=$(pwd)

#-----------------------------------------------------------
# Utility
#-----------------------------------------------------------
usage()
{
    echo ""
    echo "Usage: $1 [--dir(-d) <directory>] [--repo(-r) <pjdfstest repository url>] [--user(-u) <user>] <command>"
    echo "       <command>               specify \"setup\" or \"test\""
    echo "       --dir(-d) <directory>   directory to extract pjdfstest"
    echo "       --repo(-r) <url>        url to pjdfstest git repository"
    echo "       --user(-u) <user>       user name for github.com"
    echo "       --h(-h)                 print help"
    echo ""
}

#
# Check options
#
COMMAND_MODE=
TARGETDIR=
PJDFSTEST_GIT_URL=
USERNAME=
while [ $# -ne 0 ]; do
    if [ "X$1" = "X" ]; then
        break
    elif [ "X$1" = "X-h" ] || [ "X$1" = "X-help" ]; then
        usage "${PRGNAME}"
        exit 0
    elif [ "X$1" = "X-d" ] || [ "X$1" = "X--dir" ]; then
        shift
        if [ "X$1" = "X" ] || [ ! -d "$1" ]; then
            echo "[ERROR] \"--dir(-d)\" option parameter(\"$1\") is not specified or it is not directory."
            exit 1
        fi
        TARGETDIR="$1"
    elif [ "X$1" = "X-r" ] || [ "X$1" = "X--repo" ]; then
        shift
        if [ "X$1" = "X" ]; then
            echo "[ERROR] \"--repo(-r)\" option parameter is not specified."
            exit 1
        fi
        PJDFSTEST_GIT_URL="$1"
    elif [ "X$1" = "X-u" ] || [ "X$1" = "X--user" ]; then
        shift
        if [ "X$1" = "X" ]; then
            echo "[ERROR] \"--user(-u)\" option parameter is not specified."
            exit 1
        fi
        USERNAME="$1"
    elif [ "X$1" = "Xsetup" ] || [ "X$1" = "Xtest" ]; then
        COMMAND_MODE="$1"
    else
        echo "[ERROR] Unknown option $1"
        exit 1
    fi
    shift
done
if [ -z "${TARGETDIR}" ]; then
    TARGETDIR="${SCRIPTDIR}"
else
    TARGETDIR=$(cd "${TARGETDIR}" || exit 1; pwd)
fi
if [ -z "${PJDFSTEST_GIT_URL}" ]; then
    PJDFSTEST_GIT_URL="https://github.com/pjd/pjdfstest.git"
fi
if [ -n "${USERNAME}" ]; then
    USERNAME="${USERNAME}@"
    PJDFSTEST_GIT_URL=$(printf '%s' "${PJDFSTEST_GIT_URL}" | sed -e "s#http://#http://${USERNAME}#g" -e "s#https://#https://${USERNAME}#g")
fi
if [ -z "${COMMAND_MODE}" ]; then
    echo "[ERROR] \"<command>\" is not specified, you need to call with \"setup\" or \"test\"."
    exit 1
fi

#-----------------------------------------------------------
# Main
#-----------------------------------------------------------
if [ "${COMMAND_MODE}" = "setup" ]; then
    #-----------------------------------------------------------
    # Command : setup
    #-----------------------------------------------------------
    #
    # Clone
    #
    echo ""
    echo "[INFO] Setup pjdfstest source code"

    if [ -d "${TARGETDIR}/pjdfstest" ]; then
        echo "[INFO] Found pjdfstest directory(${TARGETDIR}/pjdfstest), pjdfstest is already cloned."
        cd "${TARGETDIR}/pjdfstest" || exit 1

    else
        echo "[INFO] Cloning pjdfstest repository"
        cd "${TARGETDIR}" || exit 1

        if ! git clone "${PJDFSTEST_GIT_URL}" 2>&1 | sed -e 's/^/       /g'; then
            echo "[ERROR] Failed to clone pjdfstest from ${PJDFSTEST_GIT_URL}."
            exit 1
        fi
        cd "${TARGETDIR}/pjdfstest" || exit 1
    fi

    #
    # Checkout master and update
    #
    echo ""
    echo "[INFO] Checkout pjdfstest master branch"

    if ! git checkout master 2>&1 | sed -e 's/^/       /g'; then
        echo "[ERROR] Failed to change branch to master."
        exit 1
    fi

    echo ""
    echo "[INFO] Update pjdfstest source code"

    if ! git checkout . 2>&1 | sed -e 's/^/       /g'; then
        echo "[ERROR] Failed to checkout all source code."
        exit 1
    fi
    if ! git pull origin master 2>&1 | sed -e 's/^/       /g'; then
        echo "[ERROR] Failed to pull new source code."
        exit 1
    fi

    #
    # Build
    #
    echo ""
    echo "[INFO] Run autoreconf in pjdfstest"

    if ! autoreconf -ifs 2>&1 | sed -e 's/^/       /g'; then
        echo "[ERROR] Failed to run autoreconf"
        exit 1
    fi

    echo ""
    echo "[INFO] Run configure in pjdfstest"

    if [ ! -f ./configure ]; then
        echo "[ERROR] Not found configure in ${TARGETDIR}/pjdfstest"
        exit 1
    fi
    if ! ./configure 2>&1 | sed -e 's/^/       /g'; then
        echo "[ERROR] Failed to run autoreconf"
        exit 1
    fi

    echo ""
    echo "[INFO] Run make in pjdfstest"

    if [ ! -f ./Makefile ]; then
        echo "[ERROR] Not found Makefile in ${TARGETDIR}/pjdfstest"
        exit 1
    fi
    if ! make 2>&1 | sed -e 's/^/       /g'; then
        echo "[ERROR] Failed to run autoreconf"
        exit 1
    fi

    #
    # Finish
    #
    cd "${CURRENTDIR}" || exit 1
else
    #-----------------------------------------------------------
    # Command : test
    #-----------------------------------------------------------
    echo ""
    echo "[INFO] Check pjdfstest binary"

    if [ ! -f "${TARGETDIR}/pjdfstest/pjdfstest" ]; then
        echo "[ERROR] Not found ${TARGETDIR}/pjdfstest/pjdfstest binary file, you must run \"setup\" before \"test\"."
        exit 1
    fi

    echo ""
    echo "[INFO] \"test\" command has not been implemented yet."
fi

#-----------------------------------------------------------
# End of command
#-----------------------------------------------------------
echo ""
echo "[INFO] Succeed"

exit 0

#
# Local variables:
# tab-width: 4
# c-basic-offset: 4
# End:
# vim600: expandtab sw=4 ts=4 fdm=marker
# vim<600: expandtab sw=4 ts=4
#
