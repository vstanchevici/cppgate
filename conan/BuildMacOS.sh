#!/bin/bash

if [ -z "$1" ]; then
    echo "No BUILD_TYPE have been provided."
    echo "BUILD_TYPE can be one of Debug, Release, RelWithDebInfo and MinSizeRel."
    exit 1
else
    BUILD_TYPE="$1"
fi

OPTIONS=(
      --output-folder=../build/xcode
      --build=missing
      --settings=build_type=$BUILD_TYPE
      --profile ./xcode.profile
      -c tools.cmake.cmaketoolchain:generator=Xcode
      -c user.build:folder_name=xcode
      conanfile.py
)

conan editable add ./

conan install ${OPTIONS[@]}