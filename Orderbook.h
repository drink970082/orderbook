#pragma once

#include <map>
#include <unordered_map>
#include <thread>
#include <condition_variable>
#include <mutex>

#include "Usings.h"
#include "Order.h"
#include "OrderModify.h"
#include "OrderbookLevelInfos.h"
#include "Trade.h"

class Orderbook
{
private:
	struct OrderEntry // a single order entry in the orderbook
	{
		OrderPointer order_{nullptr}; // the order pointer
		OrderPointers::iterator location_; // the location of the order in the orderbook
	};
	struct LevelData // metadata for each price level
	{
		Quantity quantity_{};
		Quantity count_{};

		enum class Action
		{
			Add,
			Remove,
			Match,
		};
	};
	std::unordered_map<Price, LevelData> data_;
	std::map<Price, OrderPointers, std::greater<Price>> bids_; // bids are sorted in descending order to get the best bid price
	std::map<Price, OrderPointers, std::less<Price>> asks_;	   // asks are sorted in ascending order to get the best ask price
	std::unordered_map<OrderId, OrderEntry> orders_;

	mutable std::mutex ordersMutex_;
	std::thread ordersPruneThread_;
	std::condition_variable shutdownConditionVariable_;
	std::atomic<bool> shutdown_{false};

	void PruneGoodForDayOrders();

	void CancelOrders(OrderIds orderIds);
	void CancelOrderInternal(OrderId orderId);

	void OnOrderCancelled(OrderPointer order);
	void OnOrderAdded(OrderPointer order);
	void OnOrderMatched(Price price, Quantity quantity, bool isFullyFilled);
	void UpdateLevelData(Price price, Quantity quantity, LevelData::Action action);

	bool CanFullyFill(Side side, Price price, Quantity quantity) const;
	bool CanMatch(Side side, Price price) const; // check if the order can be matched (for FillAndKill and ImmediateOrCancel orders)
	Trades MatchOrders();

public:
	Orderbook();
	// delete copy constructor and assignment operator to prevent copying
	// in our case, we don't need to copy the orderbook or move it around
	// imagine we copy the orderbook, we would have two threads pruning good for day orders, then how to join? how to wait for the prune thread to finish...
	// imagine we move the orderbook, we need to atomic transfer data structures/locks/threads...
	Orderbook(const Orderbook &) = delete;
	void operator=(const Orderbook &) = delete;
	Orderbook(Orderbook &&) = delete;
	void operator=(Orderbook &&) = delete;
	~Orderbook();

	Trades AddOrder(OrderPointer order);
	void CancelOrder(OrderId orderId);
	Trades ModifyOrder(OrderModify order);

	std::size_t Size() const;
	OrderbookLevelInfos GetOrderInfos() const;
};
