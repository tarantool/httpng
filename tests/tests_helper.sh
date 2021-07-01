if [ -e .rocks/bin/luatest ]; then
	true
else
	tarantoolctl rocks install luatest
fi
killall process_helper || true
rm tmp_reaper_socket || true
ASAN_OPTIONS=detect_leaks=0 ./.rocks/bin/luatest -v tests/all.lua --shuffle all
