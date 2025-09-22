CXX = g++
CXXFLAGS = -std=c++20 -Wall -Wextra -O2
TARGET = orderbook
SOURCES = main.cpp Orderbook.cpp
HEADERS = Orderbook.h Order.h OrderModify.h OrderbookLevelInfos.h Trade.h TradeInfo.h Side.h OrderType.h LevelInfo.h Usings.h

$(TARGET): $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $(TARGET)

clean:
	rm -f $(TARGET)

run: $(TARGET)
	./$(TARGET)

.PHONY: clean run