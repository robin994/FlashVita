test:
	c++ -std=c++14 -O2 tests/swf_parser_test.cpp src/player/swf_parser.cpp -lz -o build/swf_parser_test
	build/swf_parser_test
