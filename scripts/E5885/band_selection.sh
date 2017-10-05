#!/system/busybox sh

NETWORK_B1ONLY='AT^SYSCFGEX="99",3FFFFFFF,2,2,1,,'
NETWORK_B3ONLY='AT^SYSCFGEX="99",3FFFFFFF,2,2,4,,'
NETWORK_B5ONLY='AT^SYSCFGEX="99",3FFFFFFF,2,2,10,,'
NETWORK_B7ONLY='AT^SYSCFGEX="99",3FFFFFFF,2,2,40,,'
NETWORK_B3B7='AT^SYSCFGEX="99",3FFFFFFF,2,2,44,,'
NETWORK_ALL='AT^SYSCFGEX="99",3FFFFFFF,2,2,1A0000C00DF,,'

CONF_FILE="/var/band_selection"
ATC="/app/bin/oled_hijack/atc"

# Mode caching to prevent menu slowdowns
if [[ ! -f "$CONF_FILE" ]]
then
    CURRENT_BAND="$($ATC 'AT^SYSCFGEX?' | grep 'SYSCFGEX' | cut -d, -f5)"
    echo $CURRENT_BAND > $CONF_FILE
else
    CURRENT_BAND="$(cat $CONF_FILE)"
fi

echo $CURRENT_BAND

if [[ "$1" == "get" ]]
then
    if echo $CURRENT_BAND | grep -w "1A0000C00DF"; then exit 0
    elif [[ "$CURRENT_BAND" == "1" ]]; then exit 1
    elif [[ "$CURRENT_BAND" == "4" ]]; then exit 2
    elif [[ "$CURRENT_BAND" == "44" ]]; then exit 3
    elif [[ "$CURRENT_BAND" == "10" ]]; then exit 4
    elif [[ "$CURRENT_BAND" == "40" ]]; then exit 5
    else exit 255
    fi
fi

if [[ "$1" == "set_next" ]]
then
    if echo $CURRENT_BAND | grep -w "1A0000C00DF"; then echo -e "$NETWORK_B1ONLY\r" > /dev/appvcom && echo 1 > $CONF_FILE
    elif [[ "$CURRENT_BAND" == "1" ]]; then echo -e "$NETWORK_B3ONLY\r" > /dev/appvcom && echo 4 > $CONF_FILE
    elif [[ "$CURRENT_BAND" == "4" ]]; then echo -e "$NETWORK_B3B7\r" > /dev/appvcom && echo 44 > $CONF_FILE
    elif [[ "$CURRENT_BAND" == "44" ]]; then echo -e "$NETWORK_B5ONLY\r" > /dev/appvcom && echo 10 > $CONF_FILE
    elif [[ "$CURRENT_BAND" == "10" ]]; then echo -e "$NETWORK_B7ONLY\r" > /dev/appvcom && echo 40 > $CONF_FILE
    elif [[ "$CURRENT_BAND" == "40" ]]; then echo -e "$NETWORK_ALL\r" > /dev/appvcom && echo 1A0000C00DF > $CONF_FILE
    fi
    /system/bin/sleep 1
fi
