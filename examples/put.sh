#!/bin/bash
dd if=/dev/random of=input.bin bs=4096 count=1024 &&
curl --http2 -v -i --insecure -T input.bin https://localhost:7890/put --output output.bin

#Changes nothing
#curl -v -i --http2 --insecure -H "Transfer-Encoding: chunked" -T input.bin https://localhost:7890/put --output output.bin
