#pragma once

#include <map>
#include <unordered_map>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <numeric>

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
		OrderPointers::iterator
			location_; // the location of the order in the orderbook
	};
	std::map<Price, OrderPointers, std::greater<Price>>
		bids_; // bids are sorted in descending order to get the best bid price
	std::map<Price, OrderPointers, std::less<Price>>
		asks_; // asks are sorted in ascending order to get the best ask price
	std::unordered_map<OrderId, OrderEntry> orders_;
	bool CanMatch(Side side,
				  Price price) const // check if the order can be matched (for
									 // FillAndKill and ImmediateOrCancel orders)
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
	Trades MatchOrders()
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
				}
				if (asks.empty()) // no more ask orders at this price level
				{
					asks_.erase(askPrice);
				}
				trades.push_back(
					Trade{TradeInfo{bid->GetOrderID(), bid->GetPrice(), quantity},
						  TradeInfo{ask->GetOrderID(), ask->GetPrice(), quantity}});
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

public:
	Trades AddOrder(OrderPointer order)
	{
		if (orders_.contains(order->GetOrderID()))
		{ // order already exists
			return {};
		}
		if (order->GetOrderType() == OrderType::FillAndKill && !CanMatch(order->GetSide(), order->GetPrice()))
		{ // FillAndKill order cannot be matched
			return {};
		}
		OrderPointers::iterator iterator;
		if (order->GetSide() == Side::Buy)
		{ // add to bids
			auto &orders = bids_[order->GetPrice()];
			orders.push_back(order);
			iterator = std::next(orders.begin(), orders.size() - 1);
		}
		else
		{ // add to asks
			auto &orders = asks_[order->GetPrice()];
			orders.push_back(order);
			iterator = std::next(orders.begin(), orders.size() - 1);
		}
		orders_.insert({order->GetOrderID(), OrderEntry{order, iterator}});
		return MatchOrders();
	}
	void CancelOrder(OrderId orderId)
	{
		if (!orders_.contains(orderId))
		{ // order does not exist
			return;
		}
		const auto &[order, iterator] = orders_.at(orderId);
		orders_.erase(orderId);
		if (order->GetSide() == Side::Sell)
		{ // cancel from asks
			auto price = order->GetPrice();
			auto &orders = asks_.at(price);
			orders.erase(iterator);
			if (orders.empty())
			{ // no more orders at this price level
				asks_.erase(price);
			}
		}
		else
		{ // cancel from bids
			auto price = order->GetPrice();
			auto &orders = bids_.at(price);
			orders.erase(iterator);
			if (orders.empty())
			{ // no more orders at this price level
				bids_.erase(price);
			}
		}
	}
	Trades MatchOrder(OrderModify order)
	{
		if (!orders_.contains(order.GetOrderID()))
		{ // order does not exist
			return {};
		}
		const auto &[existingOrder, _] = orders_.at(order.GetOrderID());
		CancelOrder(order.GetOrderID());
		return AddOrder(order.ToOrderPointer(existingOrder->GetOrderType()));
	}
	std::size_t Size() const { return orders_.size(); }
	OrderbookLevelInfos GetLevelInfos() const
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
};
