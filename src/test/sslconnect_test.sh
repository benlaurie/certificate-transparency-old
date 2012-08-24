#!/usr/bin/env bash

# Note: run make freebsd-links or make linux-links before running this test

# TODO: also test passing authz data in a TLS extension

source generate_certs.sh

PASSED=0
FAILED=0

if [ "$OPENSSLDIR" != "" ]; then
  MY_OPENSSL="$OPENSSLDIR/apps/openssl"
  export LD_LIBRARY_PATH=$OPENSSLDIR
fi

if [ ! $MY_OPENSSL ]; then
# Try to use the system OpenSSL
  MY_OPENSSL=openssl
fi

test_connect() {
  cert_dir=$1
  hash_dir=$2
  log_server=$3
  ca=$4
  port=$5
  cache=$6
  expected_retcode=$7

  if [ "$cache" != "" ]; then
    cache_args="-cache $cache"
  fi
 
  ../client/ct connect 127.0.0.1 $port -log_server_key \
    $cert_dir/$log_server-key-public.pem -ca_dir $hash_dir \
    $cache_args
  local retcode=$?

  if [ $retcode -eq $expected_retcode ]; then
    echo "PASS"
    let PASSED=$PASSED+1
  else
    echo "FAIL"
    let FAILED=$FAILED+1
  fi
}

test_range() {
  ports=$1
  cert_dir=$2
  hash_dir=$3
  log_server=$4
  ca=$5
  cache=$6
  conf=$7
  retcode=$8

  # Tell Apache where to find mod_ssl by copying the symlink in test
  if [ ! -f $cert_dir/mod_ssl.so ]; then
    cp mod_ssl.so $cert_dir/mod_ssl.so
  fi

  echo "Starting Apache"
  ./apachectl -d `pwd`/$cert_dir -f `pwd`/$conf -k start

  echo "Testing valid configurations"
  for port in $ports; do
    test_connect $cert_dir $hash_dir $log_server $ca $port "$cache" $retcode;
  done

  echo "Stopping Apache"
  ./apachectl -d `pwd`/$cert_dir -f `pwd`/httpd-valid.conf -k stop
  # Wait for Apache to die
  sleep 5
}

test_valid_range() {
  # Valid ranges from httpd-valid.conf
  ports="8125 8126 8127 8128"
  cert_dir=$1
  hash_dir=$2
  log_server=$3
  ca=$4
  cache=$5
  ret=$6
  conf="httpd-valid.conf"

  test_range "$ports" $cert_dir $hash_dir $log_server $ca "$cache" $conf $ret
}

test_invalid_range() {
  # Invalid ranges from httpd-invalid.conf
  ports="8125 8126 8127 8128 8129"
  cert_dir=$1
  hash_dir=$2
  log_server=$3
  ca=$4
  cache=$5
  ret=$6
  conf="httpd-invalid.conf"

  test_range "$ports" $cert_dir $hash_dir $log_server $ca "$cache" $conf $ret
}

# Regression tests against known good/bad certificates
mkdir -p ca-hashes
hash=$($MY_OPENSSL x509 -in testdata/ca-cert.pem -hash -noout)
cp testdata/ca-cert.pem ca-hashes/$hash.0

# test_valid_range testdata ca-hashes ct-server ca "" 0
# test_invalid_range testdata ca-hashes ct-server ca "" 2

rm -rf ca-hashes

# Generate new certs dynamically and repeat the test for valid certs
mkdir -p tmp
# A directory for trusted certs in OpenSSL "hash format"
mkdir -p tmp/ca-hashes
echo "Generating CA certificates in tmp and hashes in tmp/ca"
make_ca_certs `pwd`/tmp `pwd`/tmp/ca-hashes ca $MY_OPENSSL
echo "Generating log server keys in tmp"
make_log_server_keys `pwd`/tmp ct-server

# Start the log server and wait for it to come up
echo "Starting CT server with trusted certs in $hash_dir"
../server/ct-server 8124 $cert_dir/$log_server-key.pem $hash_dir &
server_pid=$!
sleep 2

echo "Generating test certificates"
make_certs `pwd`/tmp `pwd`/tmp/ca-hashes test ca ct-server 8124 false
# Generate a second set of certs that chain through an intermediate
make_intermediate_ca_certs `pwd`/tmp intermediate ca
make_certs `pwd`/tmp `pwd`/tmp/ca-hashes test2 intermediate ct-server 8124 true

# Stop the log server
kill -9 $server_pid  
sleep 2

mkdir -p tmp/cache
echo "Testing valid configurations with new certificates" 
test_valid_range tmp tmp/ca-hashes ct-server ca tmp/cache 0

# Start the log server again and generate a second set of (inconsistent) signatures
# echo "Starting CT server with trusted certs in $hash_dir"
# ../server/ct-server 8124 $cert_dir/$log_server-key.pem $hash_dir &
# server_pid=$!
# sleep 2

# echo "Generating test certificates"
# make_certs `pwd`/tmp `pwd`/tmp/ca-hashes test ca ct-server 8124 false
# Generate a second set of certs that chain through an intermediate
# make_intermediate_ca_certs `pwd`/tmp intermediate ca
# make_certs `pwd`/tmp `pwd`/tmp/ca-hashes test2 intermediate ct-server 8124 true

# Stop the log server
# kill -9 $server_pid  
# sleep 2

# Test again and expect failure
# echo "Testing valid configurations with new certificates against existing cache" 
# test_valid_range tmp tmp/ca-hashes ct-server ca tmp/cache 3

echo "Cleaning up"
rm -rf tmp
echo "PASSED $PASSED tests"
echo "FAILED $FAILED tests"
