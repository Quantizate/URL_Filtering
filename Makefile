index: tests/index.cpp
	$(CXX) $(CFLAGS) -o index tests/ribbon.cpp -O3 -I src -std=c++17 -Wall -Wextra

clean:
	rm -rf index