#!/bin/bash

if [ -z "$1" ]; then
    echo "No BUILD_TYPE have been provided."
    echo "BUILD_TYPE can be one of Debug, Release, RelWithDebInfo and MinSizeRel."
    exit 1
else
    BUILD_TYPE="$1"
fi

OPTIONS=(
      --output-folder=../build/linux
      --build=missing
      --settings=build_type=$BUILD_TYPE
      --profile ./linux.profile
      -c tools.cmake.cmaketoolchain:generator="Ninja Multi-Config"
      -c user.build:folder_name=linux
      conanfile.py
)

conan editable add ./

conan install "${OPTIONS[@]}"
