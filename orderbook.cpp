#include <algorithm>
#include <cstdint>
#include <format>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <variant>
#include <vector>
#include <numeric>

enum class OrderType
{
	GoodTillCancel,
	FillAndKill,
	ImmediateOrCancel,
};

enum class Side
{
	Buy,
	Sell,
};

using Price = std::int32_t;
using Quantity = std::uint32_t;
using OrderId = std::uint64_t;

struct LevelInfo
{
	Price price_;
	Quantity quantity_;
};

using LevelInfos = std::vector<LevelInfo>;

class OrderbookLevelInfos
{
public:
	OrderbookLevelInfos(const LevelInfos &bids, const LevelInfos &asks)
		: bids_{bids}, asks_{asks} {}
	const LevelInfos &GetBids() const { return bids_; }
	const LevelInfos &GetAsks() const { return asks_; }

private:
	LevelInfos bids_;
	LevelInfos asks_;
};

class Order
{
public:
	Order(OrderType orderType, OrderId orderId, Side side, Price price,
		  Quantity quantity)
		: orderType_{orderType}, orderId_{orderId}, side_{side}, price_{price},
		  initialQuantity_{quantity}, remainingQuantity_{quantity} {}
	OrderId GetOrderID() const { return orderId_; }
	Side GetSide() const { return side_; }
	Price GetPrice() const { return price_; }
	OrderType GetOrderType() const { return orderType_; }
	Quantity GetInitialQuantity() const { return initialQuantity_; }
	Quantity GetRemainingQuantity() const { return remainingQuantity_; }
	Quantity GetFilledQuantity() const
	{
		return GetInitialQuantity() - GetRemainingQuantity();
	}
	bool IsFilled() const { return GetRemainingQuantity() == 0; }
	void Fill(Quantity quantity)
	{
		if (quantity > GetRemainingQuantity())
		{
			throw std::logic_error(std::format(
				"Order {} cannot fill for more than the remaining quantity",
				GetOrderID()));
		}
		remainingQuantity_ -= quantity;
	}

private:
	OrderType orderType_;
	OrderId orderId_;
	Side side_;
	Price price_;
	Quantity initialQuantity_;
	Quantity remainingQuantity_;
};

using OrderPointer =
	std::shared_ptr<Order>; // since a order can be stored in multiple data
							// structures (like bids and asks based, or a whole
							// orderbook dict)
using OrderPointers =
	std::list<OrderPointer>; // iterator cannot be invalidated (despite that the
							 // memory is not contiguous (since list is a double
							 // linked list))

class OrderModify
{
public:
	OrderModify(OrderId orderId, Side side, Price price, Quantity quantity)
		: orderId_{orderId}, side_{side}, price_{price}, quantity_{quantity} {}
	OrderId GetOrderID() const { return orderId_; }
	Side GetSide() const { return side_; }
	Price GetPrice() const { return price_; }
	Quantity GetQuantity() const { return quantity_; }
	OrderPointer ToOrderPointer(OrderType type) const
	{
		return std::make_shared<Order>(type, GetOrderID(), GetSide(), GetPrice(),
									   GetQuantity());
	}

private:
	OrderId orderId_;
	Side side_;
	Price price_;
	Quantity quantity_;
};

struct TradeInfo
{
	OrderId orderId_;
	Price price_;
	Quantity quantity_;
};
class Trade
{
public:
	Trade(const TradeInfo &bidTrade, const TradeInfo &askTrade)
		: bidTrade_{bidTrade}, askTrade_{askTrade} {}
	const TradeInfo &GetBidTrade() const { return bidTrade_; }
	const TradeInfo &GetAskTrade() const { return askTrade_; }

private:
	TradeInfo bidTrade_;
	TradeInfo askTrade_;
};
using Trades = std::vector<Trade>;
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
int main()
{
	Orderbook orderbook;
	const OrderId orderId = 1;
	orderbook.AddOrder(std::make_shared<Order>(OrderType::GoodTillCancel, orderId, Side::Buy, 100, 10));
	std::cout << orderbook.Size() << std::endl; // 1
	orderbook.CancelOrder(orderId);
	std::cout << orderbook.Size() << std::endl; // 0
	return 0;
}