#!/usr/bin/env bash

# This script is mostly so we can test the test script for a standalone server.
# <storage file> is the sqlite3 database for the log.
# <certificate hash directory> contains the OpenSSL hashes for the
# accepted root certs.

set -e

if [ "$OPENSSLDIR" != "" ]; then
  MY_OPENSSL="$OPENSSLDIR/apps/openssl"
  export LD_LIBRARY_PATH=$OPENSSLDIR:$LD_LIBRARY_PATH
fi

if [ ! $MY_OPENSSL ]; then
# Try to use the system OpenSSL
  MY_OPENSSL=openssl
fi

if [ $# != 2 ]
then
  echo "$0 <storage file> <certificate hash directory>"
  exit 1
fi

STORAGE=$1
HASH_DIR=$2
KEY="testdata/ct-server-key.pem"

if [ ! -e $HASH_DIR ]
then
  echo "$HASH_DIR doesn't exist, creating"
  mkdir $HASH_DIR
  CERT="`pwd`/testdata/ca-cert.pem"
  hash=`$MY_OPENSSL x509 -in $CERT -hash -noout`
  ln -s $CERT $HASH_DIR/$hash.0
fi

# Set the tree signing frequency to 0 to ensure we sign as often as possible.
echo "Starting CT server with trusted certs in $HASH_DIR"

../server/ct-server --port=8124 --key=$KEY \
  --trusted_cert_dir=$HASH_DIR --logtostderr=true \
  --tree_signing_frequency_seconds=1 --sqlite_db=$STORAGE
