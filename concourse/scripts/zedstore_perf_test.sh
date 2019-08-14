#!/bin/bash

set -xe

# Set the default scale factor to 1, but allow it to be overridden by a
# session level environment variable
: ${SCALE_FACTOR:=1}

export SCALE_FACTOR=${SCALE_FACTOR}

if [ -d ${HOME}/workspace/postgres ]; then
    export POSTGRES_SRC_PATH=${HOME}/workspace/postgres
else
    export POSTGRES_SRC_PATH=postgres_src
fi

/usr/local/pgsql/bin/psql -a -f ${POSTGRES_SRC_PATH}/concourse/scripts/benchmark.sql | 
    perl -MTerm::ANSIColor -ane "if (/^(Time:|-- )/) { print color('bold blue'); print $_; print color('reset');} else { print $_ }"
