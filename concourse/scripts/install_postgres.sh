#!/bin/bash
set -xe

if [ -d $HOME/workspace/postgres ]; then
    POSTGRES_SRC_PATH=$HOME/workspace/postgres
else
    POSTGRES_SRC_PATH=postgres_src
fi

pushd ${POSTGRES_SRC_PATH}
    ./configure ${EXTRA_CONFIGURE_FLAGS} CFLAGS='-O2 -fno-omit-frame-pointer' --enable-cassert --enable-debug
    make -j32 install
popd

/usr/local/pgsql/bin/initdb -D /tmp/pg_db
/usr/local/pgsql/bin/pg_ctl -D /tmp/pg_db start -l /tmp/pg_log
/usr/local/pgsql/bin/createdb
