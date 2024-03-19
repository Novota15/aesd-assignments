#!/bin/sh

if [[ $# -ne 2 ]]
then
    echo "Script must have 2 arguments"
    exit 1
fi

## check directory first and then create file
if [[ -d $(dirname $1) ]]
then
    touch $1
else
    mkdir -p $(dirname $1) && touch $1
fi

echo $2 > $1
