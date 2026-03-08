adb push build/rv1106_jpeg_stream_trial /data/local
adb shell chmod a+x ./data/local/rv1106_jpeg_stream_trial
adb shell killall -9 rkipc
adb shell ip addr add 192.168.100.50/24 dev eth0
adb shell ./data/local/rv1106_jpeg_stream_trial -w 480 -h 272