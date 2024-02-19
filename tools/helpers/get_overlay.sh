#!/bin/sh

modprobe xen_gntalloc
modprobe xen_gntdev

while :
do
    overlay_node_name=""
    type_overlay="normal"
    is_partial_dtb=""

    output=`/usr/lib/xen/bin/get_overlay 0`

    if test $? -ne 0
    then
        echo error
        exit 1
    fi

    if test -z "$output"
    then
        echo ""
        exit 1
    fi

    # output: add overlay-name normal partial
    operation=`echo $output | cut -d " " -f 1`
    overlay_node_name=`echo $output | cut -d " " -f 2`
    type_overlay=`echo $output | cut -d " " -f 3`
    is_partial_dtb=`echo $output | cut -d " " -f 4`

    if test -z "$operation" || test -z "$overlay_node_name"
    then
        echo "invalid ops"
        exit 1
    fi

    if test $operation = "add"
    then
        echo "Overlay received"

        if test "$type_overlay" = "normal"
        then
            final_path="/sys/kernel/config/device-tree/overlays/$overlay_node_name"
            mkdir -p $final_path
            cat overlay.dtbo > $final_path/dtbo
        else
            # fpga overlay
            cp overlay.dtbo lib/firmware/
            mkdir /configfs
            mount -t configfs configfs /configfs
            cd /configfs/device-tree/overlays/

            if test "$is_partial_dtb"
            then
                mkdir partial
                echo 1 > /sys/class/fpga_manager/fpga0/flags
                echo -n "overlay.dtbo" > /configfs/device-tree/overlays/partial
            else
                mkdir full
                echo -n "overlay.dtbo" > /configfs/device-tree/overlays/full
            fi
        fi
    elif test $operation = "remove"
    then
        if test "$type_overlay" = "normal"
        then
            # implement remove
            path=/sys/kernel/config/device-tree/overlays/$overlay_node_name/dtbo
            if ! test -f $path
            then
                echo "error: path doesn't exist"
                exit 1
            fi
            rm $path
        fi
    else
        echo "operation unsupported"
        exit 1
    fi
done
