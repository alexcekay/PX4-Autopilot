#!/bin/bash

# Run this from the px4 project top level directory
docker run -it --rm --privileged -v `pwd`:/usr/local/workspace alexander738/px4-dev-nuttx-noble:2025-02-10
