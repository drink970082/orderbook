#pragma once

#include <list>
#include <exception>
#include <format>

#include "OrderType.h"
#include "Side.h"
#include "Usings.h"

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
