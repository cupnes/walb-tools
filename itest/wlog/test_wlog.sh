#!/bin/sh

#
# This script will test walb-log related functionalities.
#
# If you use loopback devices, set USE_LOOP_DEV=1.
# You must have privilege of 'disk' group to use losetup commands.
# /dev/loop0 and /dev/loop1 will be used.
#
# If you set USE_LOOP_DEV=0,
# LOOP0 and LOOP1 must be ordinal block devices.
#

LDEV=ldev32M
DDEV=ddev32M
WLOG=wlog
CTL=../../walb/tool/walbctl
BIN=../../binsrc

LOOP0=/dev/loop0
LOOP1=/dev/loop1
#LOOP0=/dev/data/test-log
#LOOP1=/dev/data/test-data
USE_LOOP_DEV=1

prepare_bdev()
{
  local devPath=$1 # ex. /dev/loop0
  local devFile=$2 # ex. ldev32M.0
  if [ $USE_LOOP_DEV -eq 1 ]; then
    losetup $devPath $devFile
  else
    dd oflag=direct if=$devFile of=$devPath bs=1M
  fi
}

finalize_bdev()
{
  local devPath=$1
  local devFile=$2
  if [ $USE_LOOP_DEV -eq 1 ]; then
    losetup -d $devPath
  else
    dd oflag=direct if=$devPath of=$devFile bs=1M
  fi
}

format_ldev()
{
  dd if=/dev/zero of=$LDEV bs=1M count=32
  dd if=/dev/zero of=${DDEV}.0 bs=1M count=32
  prepare_bdev $LOOP0 $LDEV
  prepare_bdev $LOOP1 ${DDEV}.0
  $CTL format_ldev --ldev $LOOP0 --ddev $LOOP1
  RING_BUFFER_SIZE=$(${BIN}/wldev-info $LOOP0 |grep ringBufferSize |awk '{print $2}')
  echo $RING_BUFFER_SIZE
  sleep 1
  finalize_bdev $LOOP0 $LDEV
  finalize_bdev $LOOP1 ${DDEV}.0
}

echo_wlog_value()
{
  local wlogFile=$1
  local keyword=$2
  $CTL show_wlog < $wlogFile |grep $keyword |awk '{print $2}'
}

#
# Initialization.
#
format_ldev
${BIN}/wlog-gen -s 32M -z 16M --maxPackSize 4M -o ${WLOG}.0
#${BIN}/wlog-gen -s 32M -z 16M --minIoSize 512 --maxIoSize 512 --maxPackSize 1M -o ${WLOG}.0
endLsid0=$(echo_wlog_value ${WLOG}.0 end_lsid_really:)
nPacks0=$(echo_wlog_value ${WLOG}.0 n_packs:)
totalPadding0=$(echo_wlog_value ${WLOG}.0 total_padding_size:)
cp ${DDEV}.0 ${DDEV}.0z
${BIN}/wlog-redo ${DDEV}.0 < ${WLOG}.0
${BIN}/wlog-redo ${DDEV}.0z -z < ${WLOG}.0

#
# Simple test.
#
${BIN}/wlog-restore --verify $LDEV < ${WLOG}.0
${BIN}/wlog-cat $LDEV -v -o ${WLOG}.1
${BIN}/bdiff -b 512 ${WLOG}.0 ${WLOG}.1
if [ $? -ne 0 ]; then
  echo "TEST1_FAILURE"
  exit 1
fi

restore_test()
{
  local testId=$1
  local lsidDiff=$2
  local invalidLsid=$3
  local ret0
  local ret1
  local ret2

  dd if=/dev/zero of=${DDEV}.1 bs=1M count=32
  dd if=/dev/zero of=${DDEV}.1z bs=1M count=32
  dd if=/dev/zero of=${DDEV}.2 bs=1M count=32
  dd if=/dev/zero of=${DDEV}.3 bs=1M count=32
  ${BIN}/wlog-restore $LDEV --verify -d $lsidDiff -i $invalidLsid < ${WLOG}.0
  ${BIN}/wlog-cat $LDEV -v -o ${WLOG}.1
  prepare_bdev $LOOP0 ${LDEV}
  $CTL cat_wldev --wldev $LOOP0 > ${WLOG}.2
  sleep 1
  finalize_bdev $LOOP0 ${LDEV}
  if [ "$invalidLsid" = "0xffffffffffffffff" ]; then
    local endLsid0a=$(expr $endLsid0 + $lsidDiff - $nPacks0 - $totalPadding0)
    local endLsid1=$(echo_wlog_value ${WLOG}.1 end_lsid_really:)
    local endLsid2=$(echo_wlog_value ${WLOG}.2 end_lsid_really:)
    local nPacks1=$(echo_wlog_value ${WLOG}.1 n_packs:)
    local nPacks2=$(echo_wlog_value ${WLOG}.2 n_packs:)
    local totalPadding1=$(echo_wlog_value ${WLOG}.1 total_padding_size:)
    local totalPadding2=$(echo_wlog_value ${WLOG}.2 total_padding_size:)
    local endLsid1a=$(expr $endLsid1 - $nPacks1 - $totalPadding1)
    local endLsid2a=$(expr $endLsid2 - $nPacks2 - $totalPadding2)
    if [ $endLsid0a -ne $endLsid1a ]; then
      echo endLsid0a $endLsid0a does not equal to endLsid1a $endLsid1a
      echo "TEST${testId}_FAILURE"
      exit 1
    fi
    if [ $endLsid0a -ne $endLsid2a ]; then
      echo endLsid0a $endLsid0a does not equal to endLsid2a $endLsid2a
      echo "TEST${testId}_FAILURE"
      exit 1
    fi
  fi
  ${BIN}/bdiff -b 512 ${WLOG}.1 ${WLOG}.2
  if [ $? -ne 0 ]; then
    echo ${WLOG}.1 and ${WLOG}.2 differ.
    echo "TEST${testId}_FAILURE"
    exit 1
  fi

  ${BIN}/wlog-redo ${DDEV}.1 < ${WLOG}.1
  ${BIN}/wlog-redo ${DDEV}.1z -z < ${WLOG}.1
  prepare_bdev $LOOP1 ${DDEV}.2
  $CTL redo_wlog --ddev $LOOP1 < ${WLOG}.1
  sleep 1
  finalize_bdev $LOOP1 ${DDEV}.2
  sleep 1
  prepare_bdev $LOOP0 ${LDEV}
  prepare_bdev $LOOP1 ${DDEV}.3
  $CTL redo --ldev $LOOP0 --ddev $LOOP1
  sleep 1
  finalize_bdev $LOOP0 ${LDEV}
  finalize_bdev $LOOP1 ${DDEV}.3
  if [ "$invalidLsid" = "0xffffffffffffffff" ]; then
    ${BIN}/bdiff -b 512 ${DDEV}.0 ${DDEV}.1
    if [ $? -ne 0 ]; then
      echo "failed: ${BIN}/bdiff -b 512 ${DDEV}.0 ${DDEV}.1"
      echo "TEST${testId}_FAILURE"
      exit 1
    fi
    ${BIN}/bdiff -b 512 ${DDEV}.0 ${DDEV}.2
    if [ $? -ne 0 ]; then
      echo "failed: ${BIN}/bdiff -b 512 ${DDEV}.0 ${DDEV}.2"
      echo "TEST${testId}_FAILURE"
      exit 1
    fi
    ${BIN}/bdiff -b 512 ${DDEV}.0 ${DDEV}.3
    if [ $? -ne 0 ]; then
      echo "failed: ${BIN}/bdiff -b 512 ${DDEV}.0 ${DDEV}.3"
      echo "TEST${testId}_FAILURE"
      exit 1
    fi
    ${BIN}/bdiff -b 512 ${DDEV}.0z ${DDEV}.1z
    if [ $? -ne 0 ]; then
      echo "failed: ${BIN}/bdiff -b 512 ${DDEV}.0z ${DDEV}.1z"
      echo "TEST${testId}_FAILURE"
      exit 1
    fi
  else
    ${BIN}/bdiff -b 512 ${DDEV}.1 ${DDEV}.2
    if [ $? -ne 0 ]; then
      echo "failed: ${BIN}/bdiff -b 512 ${DDEV}.1 ${DDEV}.2"
      echo "TEST${testId}_FAILURE"
      exit 1
    fi
    ${BIN}/bdiff -b 512 ${DDEV}.1 ${DDEV}.3
    if [ $? -ne 0 ]; then
      echo "failed: ${BIN}/bdiff -b 512 ${DDEV}.1 ${DDEV}.3"
      echo "TEST${testId}_FAILURE"
      exit 1
    fi
  fi
}

restore_test 3 $(expr $RING_BUFFER_SIZE - 1) 0xffffffffffffffff
restore_test 4 $(expr $RING_BUFFER_SIZE - 2) 0xffffffffffffffff
restore_test 5 $(expr $RING_BUFFER_SIZE - 1024) 0xffffffffffffffff
restore_test 6 0 1024 #512KB
restore_test 7 0 8192 #4MB

echo TEST_SUCCESS
exit 0
