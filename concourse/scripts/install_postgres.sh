#!/bin/bash
set -xe

POSTGRES_INSTALL_DIR=/usr/local/pgsql
export PATH=${POSTGRES_INSTALL_DIR}/bin:$PATH

if [ -d ${HOME}/workspace/postgres ]; then
    POSTGRES_SRC_PATH=${HOME}/workspace/postgres
else
    POSTGRES_SRC_PATH=postgres_src
fi

if [ -d ${HOME}/workspace/vops ]; then
    VOPS_SRC_PATH=${HOME}/workspace/vops
else
    VOPS_SRC_PATH=vops_src
fi

pushd ${POSTGRES_SRC_PATH}
    ./configure ${EXTRA_CONFIGURE_FLAGS} CFLAGS='-O2 -fno-omit-frame-pointer' --enable-cassert --enable-debug --prefix=${POSTGRES_INSTALL_DIR}
    make -j32 install
popd

pushd ${VOPS_SRC_PATH}
    USE_PGXS=true make -j32 install
popd

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
