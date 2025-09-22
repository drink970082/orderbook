#include "Orderbook.h"

#include <chrono>
#include <ctime>
#include <numeric>

void Orderbook::PruneGoodForDayOrders()
{
	using namespace std::chrono;
	const auto end = hours(16); // close of the market
	while (true)
	{
		const auto now = system_clock::now();			 // current time
		const auto now_c = system_clock::to_time_t(now); // current time in seconds
		std::tm now_parts;								 // current time in parts
		localtime_r(&now_c, &now_parts);				 // convert current time to parts
		if (now_parts.tm_hour >= end.count())
		{
			now_parts.tm_mday += 1; // if the current time is after the close of the market, we need to increment the day (waiting for the next day)
		}

		now_parts.tm_hour = end.count(); // set the hour to 4 pm
		now_parts.tm_min = 0;			 // exact time of 4 pm
		now_parts.tm_sec = 0;
		auto next = system_clock::from_time_t(mktime(&now_parts)); // the time we want to wait for (today at 4 pm, or tomorrow at 4 pm if the current time is after the close of the market)
		auto till = next - now + milliseconds(100);

		{
			// unique_lock and wait_for: an atomic operation that waits for the condition variable to be notified
			// logic: we lock the mutex, then unlock+sleep, waiting for the condition variable to be notified
			// if shutdown is true, we return (cond1)
			// if the condition variable is notified (whether by shutdown(i.e. notify_one) or the condition variable (till)), we wake up and see if we are timed out (till is reached) or not (early return) (cond2)
			// once we wake up, we lock the mutex again and check the condition again
			// if it is timed out, we continue to cancel the good for day orders (out of the lock scope, so it is unlocked)
			// the lock is for wait, not for protecting the data structure (orders_, bids_, asks_)
			std::unique_lock<std::mutex> orderslock{ordersMutex_};

			if (shutdown_.load(std::memory_order_acquire) ||										 // orderbook is shut down
				shutdownConditionVariable_.wait_for(orderslock, till) == std::cv_status::no_timeout) // wait for 4pm or the condition variable to be notified (if no_timeout, it means we received a wake up signal to before 4pm)
			{
				return;
			}
		}

		OrderIds orderIds;
		{
			// we block all the threads from adding or modifying the orderbook data structure while we are canceling the good for day orders
			// every thread with the same mutex (ordersMutex_) is blocked so we can iterate over the orders_ map without worrying about data race
			std::scoped_lock ordersLock{ordersMutex_};
			for (const auto &[_, entry] : orders_)
			{
				const auto &[order, __] = entry;
				if (order->GetOrderType() != OrderType::GoodForDay)
				{
					continue;
				}
				orderIds.push_back(order->GetOrderID());
			}
		}
		CancelOrders(orderIds);
	}
}
void Orderbook::CancelOrders(OrderIds orderIds) // cancel multiple orders
{
	// we lock the mutex at once, this is more efficient than locking it for each order (i.e. calling CancelOrderInternal for each order)
	std::scoped_lock ordersLock{ordersMutex_};
	for (const auto &orderId : orderIds)
	{
		CancelOrderInternal(orderId);
	}
}
void Orderbook::CancelOrderInternal(OrderId orderId) // helper function to cancel an order
{
	if (!orders_.contains(orderId)) // order does not exist
	{
		return;
	}
	const auto &[order, iterator] = orders_.at(orderId);
	orders_.erase(orderId);
	if (order->GetSide() == Side::Sell) // cancel from asks
	{
		auto price = order->GetPrice();
		auto &orders = asks_.at(price);
		orders.erase(iterator);
		if (orders.empty()) // no more orders at this price level
		{
			asks_.erase(price);
		}
	}
	else // cancel from bids
	{
		auto price = order->GetPrice();
		auto &orders = bids_.at(price);
		orders.erase(iterator);
		if (orders.empty()) // no more orders at this price level
		{
			bids_.erase(price);
		}
	}
	OnOrderCancelled(order);
}
void Orderbook::OnOrderCancelled(OrderPointer order)
{
	UpdateLevelData(order->GetPrice(), order->GetRemainingQuantity(), LevelData::Action::Remove);
}

void Orderbook::OnOrderAdded(OrderPointer order)
{
	UpdateLevelData(order->GetPrice(), order->GetInitialQuantity(), LevelData::Action::Add);
}
void Orderbook::OnOrderMatched(Price price, Quantity quantity, bool isFullyFilled)
{
	UpdateLevelData(price, quantity, isFullyFilled ? LevelData::Action::Remove : LevelData::Action::Match);
}
void Orderbook::UpdateLevelData(Price price, Quantity quantity, LevelData::Action action)
{
	auto &data = data_[price];
	data.count_ += action == LevelData::Action::Remove ? -1 : action == LevelData::Action::Add ? 1
																							   : 0;
	if (action == LevelData::Action::Remove || action == LevelData::Action::Match)
	{
		data.quantity_ -= quantity;
	}
	else
	{
		data.quantity_ += quantity;
	}
	if (data.count_ == 0)
		data_.erase(price);
}
bool Orderbook::CanFullyFill(Side side, Price price, Quantity quantity) const
{
	if (!CanMatch(side, price))
	{
		return false;
	}
	// at least one order can be matched
	std::optional<Price> threshold;
	if (side == Side::Buy)
	{
		const auto [askPrice, _] = *asks_.begin();
		threshold = askPrice;
	}
	else
	{
		const auto [bidPrice, _] = *bids_.begin();
		threshold = bidPrice;
	}
	// go through the price levels from best(threshold) to worst(your price)
	// in buy side, we want to go from best ask to your bidding price
	// in sell side, we want to go from best bid to your asking price
	for (const auto &[levelPrice, levelData] : data_)
	{
		// skip the price levels that are better than the threshold
		if (threshold.has_value())
		{
			if ((side == Side::Buy && threshold.value() > levelPrice) ||
				(side == Side::Sell && threshold.value() < levelPrice))
			{
				continue;
			}
		}
		// skip the price levels that are worse than your price
		if ((side == Side::Buy && levelPrice > price) ||
			(side == Side::Sell && levelPrice < price))
		{
			continue;
		}
		if (quantity <= levelData.quantity_)
		{
			return true;
		}
		quantity -= levelData.quantity_;
	}
	return false;
}
bool Orderbook::CanMatch(Side side, Price price) const // check if the order can be matched (for FillAndKill and ImmediateOrCancel orders)
{
	if (side == Side::Buy)
	{
		if (asks_.empty())
		{
			return false;
		}
		const auto &[bestAsk, _] = *asks_.begin(); // get the best ask price
		return price >= bestAsk;
	}
	else
	{
		if (bids_.empty())
		{
			return false;
		}
		const auto &[bestBid, _] = *bids_.begin(); // get the best bid price
		return price <= bestBid;
	}
}
Trades Orderbook::MatchOrders()
{
	Trades trades;
	trades.reserve(orders_.size());
	while (true) // keep matching orders until no more orders can be matched
	{
		if (bids_.empty() || asks_.empty()) // no more orders to match
		{
			break;
		}
		auto &[bidPrice, bids] = *bids_.begin();
		auto &[askPrice, asks] = *asks_.begin();
		if (bidPrice < askPrice) // no more valid orders to match
		{
			break;
		}
		while (bids.size() && asks.size())
		{
			auto &bid = bids.front();
			auto &ask = asks.front();
			Quantity quantity = std::min(bid->GetRemainingQuantity(),
										 ask->GetRemainingQuantity());
			bid->Fill(quantity);
			ask->Fill(quantity);

			if (bid->IsFilled())
			{
				bids.pop_front();
				orders_.erase(bid->GetOrderID());
			}
			if (ask->IsFilled())
			{
				asks.pop_front();
				orders_.erase(ask->GetOrderID());
			}
			if (bids.empty()) // no more bid orders at this price level
			{
				bids_.erase(bidPrice);
				data_.erase(bidPrice);
			}
			if (asks.empty()) // no more ask orders at this price level
			{
				asks_.erase(askPrice);
				data_.erase(askPrice);
			}
			trades.push_back(
				Trade{TradeInfo{bid->GetOrderID(), bid->GetPrice(), quantity},
					  TradeInfo{ask->GetOrderID(), ask->GetPrice(), quantity}});
			OnOrderMatched(bid->GetPrice(), quantity, bid->IsFilled());
			OnOrderMatched(ask->GetPrice(), quantity, ask->IsFilled());
		}
		if (!bids_.empty()) // check if there are any FillAndKill orders at this price level
		{
			auto &[_, bids] = *bids_.begin();
			auto &order = bids.front();
			if (order->GetOrderType() == OrderType::FillAndKill)
			{
				CancelOrder(order->GetOrderID());
			}
		}
		if (!asks_.empty())
		{
			auto &[_, asks] = *asks_.begin();
			auto &order = asks.front();
			if (order->GetOrderType() == OrderType::FillAndKill)
			{
				CancelOrder(order->GetOrderID());
			}
		}
	}
	return trades;
}
Orderbook::Orderbook() : ordersPruneThread_{[this]
											{ PruneGoodForDayOrders(); }} {}
Orderbook::~Orderbook()
{
	shutdownConditionVariable_.notify_one(); // notify the prune thread to wake up
	ordersPruneThread_.join();				 // wait for the prune thread to finish
}

Trades Orderbook::AddOrder(OrderPointer order)
{
	std::scoped_lock ordersLock{ordersMutex_};
	if (orders_.contains(order->GetOrderID())) // order already exists
	{
		return {};
	}
	if (order->GetOrderType() == OrderType::FillAndKill && !CanMatch(order->GetSide(), order->GetPrice())) // FillAndKill order cannot be matched
	{
		return {};
	}
	if (order->GetOrderType() == OrderType::FillOrKill && !CanFullyFill(order->GetSide(), order->GetPrice(), order->GetInitialQuantity()))
	{
		return {};
	}
	OrderPointers::iterator iterator;
	if (order->GetSide() == Side::Buy) // add to bids
	{
		auto &orders = bids_[order->GetPrice()];
		orders.push_back(order);
		iterator = std::next(orders.begin(), orders.size() - 1);
	}
	else // add to asks
	{
		auto &orders = asks_[order->GetPrice()];
		orders.push_back(order);
		iterator = std::next(orders.begin(), orders.size() - 1);
	}
	orders_.insert({order->GetOrderID(), OrderEntry{order, iterator}});
	OnOrderAdded(order);
	return MatchOrders();
}
void Orderbook::CancelOrder(OrderId orderId)
{
	std::scoped_lock ordersLock{ordersMutex_};
	CancelOrderInternal(orderId);
}
Trades Orderbook::ModifyOrder(OrderModify order)
{
	OrderType orderType;
	{
		std::scoped_lock ordersLock{ordersMutex_};
		if (!orders_.contains(order.GetOrderID())) // order does not exist
		{
			return {};
		}
		const auto &[existingOrder, _] = orders_.at(order.GetOrderID());
		orderType = existingOrder->GetOrderType();
	}
	CancelOrder(order.GetOrderID());
	return AddOrder(order.ToOrderPointer(orderType));
}
std::size_t Orderbook::Size() const
{
	std::scoped_lock ordersLock{ordersMutex_};
	return orders_.size();
}
OrderbookLevelInfos Orderbook::GetOrderInfos() const
{
	LevelInfos bidInfos, askInfos;
	bidInfos.reserve(orders_.size());
	askInfos.reserve(orders_.size());

	auto CreateLevelInfos = [](Price price, const OrderPointers &orders)
	{
		return LevelInfo{
			price, std::accumulate(orders.begin(), orders.end(), (Quantity)0,
								   [](Quantity runningSum, const OrderPointer &order)
								   { return runningSum + order->GetRemainingQuantity(); })};
	};
	for (const auto &[price, orders] : bids_)
	{
		bidInfos.push_back(CreateLevelInfos(price, orders));
	}
	for (const auto &[price, orders] : asks_)
	{
		askInfos.push_back(CreateLevelInfos(price, orders));
	}
	return OrderbookLevelInfos(bidInfos, askInfos);
}