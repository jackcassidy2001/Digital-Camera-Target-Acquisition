#!/bin/bash

# Memory addresses (Replace these with actual addresses)
BUTTON_ADDR="0x41210000"
SWITCH_ADDR="0x41220000"
LED_ADDR="0x41200000"

# Function to read switch state
read_switch_state() {
    switch_state=$(busybox devmem "$SWITCH_ADDR")
    switch_state=$(printf "%02x" "$switch_state")
    echo $switch_state 
}

# Function to write LED state
write_led_state() {
    led_state="$1"
    devmem "$LED_ADDR" 16 0x$led_state
}

# Function to check number of buttons pressed
check_buttons() {
    buttons_pressed=$(devmem "$BUTTON_ADDR")
    #echo $buttons_pressed
    buttons_pressed=$(printf "%08x" "$buttons_pressed")
    #echo $buttons_pressed
    buttons_pressed=$(hex_to_binary "$buttons_pressed")
    #echo $buttons_pressed
    num_buttons=$(echo "$buttons_pressed" | tr -cd '1' | wc -c)
    #echo $num_buttons
}

hex_to_binary() {
    local hex="$1"
    local bin=""
    for (( i=0; i<${#hex}; i++ )); do
        case "${hex:$i:1}" in
            0) bin+="0000" ;;
            1) bin+="0001" ;;
            2) bin+="0010" ;;
            3) bin+="0011" ;;
            4) bin+="0100" ;;
            5) bin+="0101" ;;
            6) bin+="0110" ;;
            7) bin+="0111" ;;
            8) bin+="1000" ;;
            9) bin+="1001" ;;
            a|A) bin+="1010" ;;
            b|B) bin+="1011" ;;
            c|C) bin+="1100" ;;
            d|D) bin+="1101" ;;
            e|E) bin+="1110" ;;
            f|F) bin+="1111" ;;
            *) echo "Invalid hexadecimal digit ${hex:$i:1}" >&2; exit 1 ;;
        esac
    done
    echo "$bin"
}

invert_hex() {
    local hex="$1"
    local inverted_hex=""
    for (( i=0; i<${#hex}; i++ )); do
        case "${hex:$i:1}" in
            0) inverted_hex+="F" ;;
            1) inverted_hex+="E" ;;
            2) inverted_hex+="D" ;;
            3) inverted_hex+="C" ;;
            4) inverted_hex+="B" ;;
            5) inverted_hex+="A" ;;
            6) inverted_hex+="9" ;;
            7) inverted_hex+="8" ;;
            8) inverted_hex+="7" ;;
            9) inverted_hex+="6" ;;
            a|A) inverted_hex+="5" ;;
            b|B) inverted_hex+="4" ;;
            c|C) inverted_hex+="3" ;;
            d|D) inverted_hex+="2" ;;
            e|E) inverted_hex+="1" ;;
            f|F) inverted_hex+="0" ;;
            *) echo "Invalid hexadecimal digit ${hex:$i:1}" >&2; exit 1 ;;
        esac
    done
    echo "$inverted_hex"
}

# Main loop
while true; do
    check_buttons

    case $num_buttons in
        0) # No buttons pressed, LEDs reflect switch state
            echo "None Pressed"
	    switch_state=$(read_switch_state)
	    echo $switch_state
            write_led_state "$switch_state"
            ;;
        1) # 1 button pressed, LEDs reflect inverse of switch state
            echo "1 Pressed"
	    switch_state=$(read_switch_state)
            echo $switch_state
	    inverse_state=$(invert_hex "$switch_state")
            echo $inverse_state
	    write_led_state "$inverse_state"
            ;;
        *) # More than 1 button pressed, LEDs all 1s
	    echo "Multi-Press"	
            write_led_state "FF"
            ;;
    esac

    sleep 0.1 # Adjust sleep time as needed
done

