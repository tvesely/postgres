#!/bin/bash

set -xe

if [ -d ${HOME}/workspace/postgres ]; then
    export POSTGRES_SRC_PATH=${HOME}/workspace/postgres
else
    export POSTGRES_SRC_PATH=postgres_src
fi

/usr/local/pgsql/bin/psql -a -f ${POSTGRES_SRC_PATH}/concourse/scripts/benchmark.sql | 
    perl -MTerm::ANSIColor -ane "if (/^(Time:|-- )/) { print color('bold blue'); print $_; print color('reset');} else { print $_ }"
