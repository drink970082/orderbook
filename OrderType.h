#pragma once

enum class OrderType
{
    GoodTillCancel,
    FillAndKill,
    FillOrKill,
    ImmediateOrCancel,
    GoodForDay,
    Market,
};