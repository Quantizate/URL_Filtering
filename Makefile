index: tests/index.cpp
	gcc-13 $(CFLAGS) -I /opt/homebrew/Cellar/boost/1.84.0/include -o index tests/b_fuse_new.cpp -O3 -I src -std=c++17 -Wall -Wextra -L /opt/homebrew/Cellar/boost/1.84.0/lib -lstdc++ -lboost_system  -arch arm64
	# $(CXX) $(CFLAGS) -I /opt/homebrew/Cellar/boost/1.84.0/include -o index tests/Xor_filter_new.cpp -O3 -I src -std=c++17 -Wall -Wextra -L /opt/homebrew/Cellar/boost/1.84.0/lib -lstdc++ -lboost_system  -arch arm64

clean:
	rm -rf index