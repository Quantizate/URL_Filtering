#!/usr/bin/env bash

INFILE=$1
DATAFILE=$2
FINAL=$3
STEP=$4
OUTFILE1=$5
OUTFILE2=$6
OUTFILE3=$7
COUNT=0

percentBar ()  { 
    local prct totlen=$((8*$2)) lastchar barstring blankstring;
    printf -v prct %.2f "$1"
    ((prct=10#${prct/.}*totlen/10000, prct%8)) &&
        printf -v lastchar '\\U258%X' $(( 16 - prct%8 )) ||
            lastchar=''
    printf -v barstring '%*s' $((prct/8)) ''
    printf -v barstring '%b' "${barstring// /\\U2588}$lastchar"
    printf -v blankstring '%*s' $(((totlen-prct)/8)) ''
    printf -v "$3" '%s%s' "$barstring" "$blankstring"
}

# while getopts i:d:f:s:o flag
# do
#     case "${flag}" in
#         i) INFILE=${OPTARG};;
#         d) DATAFILE=${OPTARG};;
#         f) FINAL=${OPTARG};;
#         s) STEP=${OPTARG};;
#         o) OUTFILE=${OPTARG};;
#     esac
# done

make clean; make

# while [ $COUNT -lt $FINAL ]; do
#     let COUNT=COUNT+STEP
#     p=$(echo "scale=2; $COUNT/$FINAL*100" | bc)
#     percentBar $p $((COLUMNS-7)) bar
#     printf '\r\e[47;30m%s\e[0m%6.2f%%' "$bar" $p
#     ./"$INFILE" "$DATAFILE" "$COUNT" "$OUTFILE"
# done

bar_size=40
bar_char_done="#"
bar_char_todo="-"
bar_percentage_scale=2

function show_progress {
    current="$1"
    total="$2"

    # calculate the progress in percentage 
    percent=$(bc <<< "scale=$bar_percentage_scale; 100 * $current / $total" )
    # The number of done and todo characters
    done=$(bc <<< "scale=0; $bar_size * $percent / 100" )
    todo=$(bc <<< "scale=0; $bar_size - $done" )

    # build the done and todo sub-bars
    done_sub_bar=$(printf "%${done}s" | tr " " "${bar_char_done}")
    todo_sub_bar=$(printf "%${todo}s" | tr " " "${bar_char_todo}")

    # output the bar
    echo -ne "\rProgress : [${done_sub_bar}${todo_sub_bar}] ${percent}% ($current/$total)"

    if [ $total -eq $current ]; then
        echo -e "\nDONE"
    fi
}

# percent(){ local p=00$(($1*100000/$2));printf -v "$3" %.2f ${p::-3}.${p: -3};}

while [ $COUNT -lt $FINAL ]; do
    let COUNT=COUNT+STEP
    show_progress $COUNT $FINAL

    # percent $COUNT $FINAL percent
    # percentBar $percent $((cols-8)) prctbar
    # printf '\r\e[44;38;5;25m%s\e[0m%6.2f%%' "$prctbar" $percent;

    ./"$INFILE" "$DATAFILE" "$COUNT" "$OUTFILE1" "$OUTFILE2" "$OUTFILE3"
done