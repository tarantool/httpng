#!/usr/bin/env sh

# set -x

LOG_DIR=/tmp/httpng/test/$(uuidgen)/listen_test
LUA_TEST_FILENAME=listen_test.lua
LOG_LINES_TO_PRINT=20

GLOBAL_RESULT=0

cd $(dirname "${BASH_SOURCE[0]}")
TEST_DIR=$(pwd)

cd $TEST_DIR/..

color_string() {
	local COLOR_NUM

	case $2 in
	"GREEN")
		COLOR_NUM=32
		;;
	"RED")
		COLOR_NUM=31
		;;
	*)
		COLOR_NUM=""
		;;
	esac

	echo "\033[;$COLOR_NUM""m$1\033[0m"
}

test_cases=(
	"only_port_80:0"
	"only_http:0"
	"2_listeners_with_tls:0"
	"localhost:0"
	"localhost:443:0"
	"443:0"
	"non_string_addr:1"
	"only_port_80_443:0"
	"http_and_80:1"
	"80_and_2_tables:0"
)

create_log_dir() {
	if [ -d "$LOG_DIR" ]
	then
		rm -rf "$LOG_DIR"
	fi
	mkdir -p $LOG_DIR
}

main() {

	create_log_dir

	for key in ${test_cases[@]}
	do
		local TEST_NAME="${key%:*}"
		local EXPECTED_TEST_RETURN_CODE="${key##*:}"
		local LOG_FILENAME="$LOG_DIR/$TEST_NAME.log"
		
		rm -f *.xlog *.snap; "$TEST_DIR/$LUA_TEST_FILENAME" >"$LOG_FILENAME" 2>&1 "$TEST_NAME";

		if [ "$?" -eq "$EXPECTED_TEST_RETURN_CODE" ]
		then
			printf "%25s %5s\n" "$TEST_NAME:" $(color_string OK GREEN)
			rm "$LOG_FILENAME"
		else
			printf "%25s %5s\n" "$TEST_NAME:" $(color_string FAIL RED)
			echo "Last $LOG_LINES_TO_PRINT lines of $LOG_FILENAME:"
			tail "-$LOG_LINES_TO_PRINT" "$LOG_FILENAME"
		fi
	done
}

main
