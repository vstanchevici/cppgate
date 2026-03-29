```
## Step 1: Run scripts from folder.
run BuildVS2022.bat or BuildMacOS.sh

## Step 2: Run cmake for vs2022/xcode
```
cmake -S . -G "Visual Studio 17 2022" -B ./build/vs2022 -DCMAKE_TOOLCHAIN_FILE=build/vs2022/generators/conan_toolchain.cmake  -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
```
```
cmake -S . -G "Xcode" -B ./build/xcode -DCMAKE_TOOLCHAIN_FILE=build/xcode/generators/conan_toolchain.cmake  -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
```

