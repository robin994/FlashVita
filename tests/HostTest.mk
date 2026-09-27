test:
	@mkdir -p build/host
	c++ -std=c++17 -O2 -Wall -Wextra tests/swf_parser_test.cpp tests/host_file_io.cpp src/player/swf_parser.cpp -lz -o build/host/swf_parser_test
	build/host/swf_parser_test
