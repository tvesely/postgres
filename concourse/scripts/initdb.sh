#!/bin/bash

POSTGRES_INSTALL_DIR=/usr/local/pgsql

${POSTGRES_INSTALL_DIR}/bin/initdb -D /tmp/pg_db
${POSTGRES_INSTALL_DIR}/bin/pg_ctl -D /tmp/pg_db start -l /tmp/pg_log
