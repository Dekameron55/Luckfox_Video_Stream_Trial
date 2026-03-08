#!/bin/sh
#
# Starts the trial example upon boot (optional).
# dont forget to chmod the file after upload.
#

LOG_FILE="/tmp/S99luckfox_stream.log"

# Redirect all output to a log file for debugging
exec > $LOG_FILE 2>&1

echo "--- S99luckfox_stream starting at $(date) ---"

case "$1" in
  start)
    echo "Running start command..."

    # Kill conflicting process
    echo "Stopping rkipc..."
    killall -9 rkipc

    # Give the network interface a moment to initialize
    echo "Waiting 2 seconds for network interface..."
    sleep 2

    # Bring the interface up and assign IP
    echo "Setting eth0 link up..."
    ip link set eth0 up
    echo "Assigning IP 192.168.100.50 to eth0..."
    ip addr add 192.168.100.50/24 dev eth0

    # Set the library path so the application can find Rockchip's libraries
    echo "Exporting library path to include /usr/lib..."
    export LD_LIBRARY_PATH=/usr/lib:$LD_LIBRARY_PATH
    echo "Exporting library path to include common Rockchip locations..."
    export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/lib:/oem/lib:/lib:/vendor/lib:/oem/usr/lib

    # Run streamer in background with correct arguments for ISP
    STREAMER_CMD="/data/local/rv1106_jpeg_stream_trial -w 480 -h 272"
    echo "Scheduling streamer to start in 3 seconds..."
    (
        sleep 3
        echo "Starting streamer with command: $STREAMER_CMD"
        $STREAMER_CMD
    ) &

    echo "Streamer process launch scheduled."
    ;;
  stop)
    killall -q -9 simple_vi_bind_venc_combo_rv1106
    ;;
  *)
    echo "Usage: $0 {start|stop}"
    exit 1
    ;;
esac

echo "--- S99luckfox_stream finished ---"
exit 0
