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

if [ -d ${HOME}/workspace/tpch-dbgen ]; then
    export TPCH_DATAGEN_SRC_PATH=${HOME}/workspace/tpch-dbgen
else
    export TPCH_DATAGEN_SRC_PATH=${HOME}/tpch_dbgen_src
fi

# Generate dataset
pushd ${TPCH_DATAGEN_SRC_PATH}
    make -j32
    ./dbgen -s ${SCALE_FACTOR} -T L -f
    cp lineitem.tbl /tmp/lineitem.tbl
popd

/usr/local/pgsql/bin/psql -a -f ${POSTGRES_SRC_PATH}/concourse/scripts/benchmark.sql | 
    perl -MTerm::ANSIColor -ane "if (/^(Time:|-- )/) { print color('bold blue'); print $_; print color('reset');} else { print $_ }"
