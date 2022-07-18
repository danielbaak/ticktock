#!/bin/bash

# This file is to calculate how many data points in a data dir.

if [ $# -ne 1 ]; then
    echo "Usage  : $0 <data file dir>"
    echo "Example: $0 /home/usr1/ticktock/data"
    exit 1
fi

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

inputDir=$1
totalDataPoints=0
FILES=$(ls --ignore='*.cp' --ignore='*.meta' $1/)

for f in $FILES
do
	numDataPoints=$($DIR/../bin/inspect -a $1/$f | grep -v "dps\|Inspecting\|TSDB" | wc -l)
	totalDataPoints=$(( $totalDataPoints + $numDataPoints ))
	echo "$f: num=$numDataPoints, total=$totalDataPoints"
done

exit 0
