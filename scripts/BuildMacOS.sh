#!/bin/bash

if [ -z "$1" ]; then
    echo "No BUILD_TYPE have been provided."
    echo "BUILD_TYPE can be one of Debug, Release, RelWithDebInfo and MinSizeRel."
    exit 1
else
    BUILD_TYPE="$1"
fi

OPTIONS=(
      --output-folder=../build/xcode/conan_libs
      --build=missing
      --settings=build_type=$BUILD_TYPE
      --profile ./xcode.profile
      ./conanfile.txt
)

conan install ${OPTIONS[@]}
