test: orderbook.cpp
	g++ -std=c++20  orderbook.cpp -o test
clean:
	rm test

run: test
	./test