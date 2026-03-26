```
## Step 1: Run scripts from folder.
run BuildVS2022.bat or BuildMacOS.sh

## Step 2: Run cmake for vs2022/xcode
```
cmake -S . -G "Visual Studio 17 2022" -B ./build/vs2022 -DCMAKE_PREFIX_PATH:STRING=./build/vs2022/build/generators
```
```
cmake -S . -G "Xcode" -B ./build/xcode -DCMAKE_PREFIX_PATH:STRING=./build/xcode/conan_libs
```

