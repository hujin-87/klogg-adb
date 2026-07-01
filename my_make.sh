#cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -S /home/hujin/workspace_agent/klogg-adb -B /home/hujin/workspace_agent/klogg-adb/build_root  


cmake --build /home/hujin/workspace_agent/klogg-adb/build_root -j24



#./docker/mxe-win/build-win.sh
#产物：
#- build_win/klogg-win64/klogg.exe(及 klogg_portable.exe、klogg_grep.exe + 所有 DLL/插件)
#- build_win/klogg-win64.zip(可直接发给别人的整包)
