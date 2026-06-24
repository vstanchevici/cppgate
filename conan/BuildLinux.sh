#!/bin/bash

# Default values
BUILD_TYPE=""
EDITABLE_MODE=false

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        Debug|Release|RelWithDebInfo|MinSizeRel)
            BUILD_TYPE="$1"
            shift
            ;;
        --editable)
            EDITABLE_MODE=true
            shift
            ;;
        *)
            echo "Unknown argument: $1"
            exit 1
            ;;
    esac
done

# Validate that a build type was provided
if [ -z "$BUILD_TYPE" ]; then
    echo "No BUILD_TYPE has been provided."
    echo "Usage: $0 [Debug|Release|RelWithDebInfo|MinSizeRel] [--editable]"
    exit 1
fi

# Base Conan options that apply to EVERY build
OPTIONS=(
    --output-folder=../build/linux
    --build=missing
    --settings=build_type=$BUILD_TYPE
    --profile ./linux.profile
    -c tools.cmake.cmaketoolchain:generator="Ninja Multi-Config"
)

# Dynamically handle editable mode configurations
if [ "$EDITABLE_MODE" = true ]; then
    echo "--> Enabling Local Editable Mode for tracking changes..."
    conan editable add ./ --name=cppgate --version=0.1.0
    
    # Inject your custom layout configuration flag into the options array
    OPTIONS+=(-c user.build:folder_name=linux)
else
    echo "--> Running standard isolated build (Production/Docker)..."
fi

# Target file must always be last in the options array
OPTIONS+=(conanfile.py)

# Execute the final installation step
conan install "${OPTIONS[@]}"
